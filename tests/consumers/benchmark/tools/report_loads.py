#!/usr/bin/env python3
"""Rehash raw loader observations; report CPU worker/submit scopes faithfully."""
import argparse
import json
from pathlib import Path
import statistics

from gbuffer_asset_suite import sha256_file, write
from load_suite import validate_marker, prepared_bundle, PHASES, io_observation
from load_async_suite import validate_async
from tools.benchmark_suite import read_frames, frame_summary
from tools.jce_determinism import assert_backend


def checked(path, expected):
    if sha256_file(path) != expected:
        raise ValueError('retained artifact changed: '+str(path))


def checked_frames(path, limit):
    frames=read_frames(path,warmup=0,sample_count=None)
    if len(frames["frame_ms"])!=limit:
        raise ValueError("complete frame stream required")
    return frames


def load(path):
    value=json.loads(path.read_text(encoding='utf-8'))
    if value.get('status')!='LOAD_PHASE_OBSERVATION_ONLY':
        raise ValueError('complete loader observation required')
    root=path.parent
    checked(Path(value['exe']),value['exe_sha256'])
    if set(value['inputs'])!={'glb','texture','audio','bundle'}:
        raise ValueError('four loader input kinds required')
    for record in value['inputs'].values():checked(Path(record['path']),record['sha256'])
    if set(value['cases'])!={kind+'-'+str(i) for kind in value['inputs'] for i in range(3)}:
        raise ValueError('complete twelve-case loader matrix required')
    preparation=value.get('preparation')
    if preparation:
        for relative,digest in preparation['raw_sha256'].items():checked(root/'prepare'/relative,digest)
        prep_log=(root/'prepare/engine.log').read_text(encoding='utf-8')
        if prep_log.count('benchmark-load-prepare: ok=1 result=0')!=1 or '] ERROR -' in prep_log:
            raise ValueError('SDK preparation failed')
        bundle,entry=prepared_bundle(root/'prepare/bundles',Path(value['inputs']['glb']['path']).stat().st_size)
        if bundle!=Path(value['inputs']['bundle']['path']):raise ValueError('prepared archive identity differs')
    limit=value.get('frame_limit',200)
    if not 200<=limit<=100000:raise ValueError('invalid diagnostic frame limit')
    computed={}
    backend=None
    for label,case in value['cases'].items():
        directory=root/label
        raw=case['raw_sha256']
        required={'engine.log','frames.csv','scene.json',str(Path('engine_process/samples.json')),
                  str(Path('engine_process/result.json')),str(Path('engine_process/stdout.log')),
                  str(Path('engine_process/stderr.log'))}
        if not required.issubset(raw):raise ValueError('raw frame/process evidence missing')
        for relative,digest in raw.items():checked(directory/relative,digest)
        process=case['process']['process']
        if process.get('returncode')!=0 or process.get('timed_out'):raise ValueError('loader process failed')
        retained=json.loads((directory/'engine_process/result.json').read_text())
        if retained.get('returncode')!=0 or retained.get('timed_out') or retained.get('elapsed_s')!=process.get('elapsed_s'):
            raise ValueError('process record differs from raw telemetry')
        samples=json.loads((directory/'engine_process/samples.json').read_text())
        if samples!=case['process']['process_samples'] or io_observation(samples)!=case['io_observation']:
            raise ValueError('stored process samples/IO differ')
        peak=max((s['rss_bytes'] for s in samples),default=None)
        if peak!=process.get('peak_rss_bytes'):raise ValueError('sampled RSS peak differs')
        log=(directory/'engine.log').read_text(encoding='utf-8')
        env=case['environment']
        actual=assert_backend(log,env['JCE_BACKEND'])
        if '] ERROR -' in log or actual!=case['process']['backend_actual']:
            raise ValueError('loader backend/errors differ')
        if backend is not None and backend!=actual:raise ValueError('mixed backends')
        backend=actual
        kind=label.rsplit('-',1)[0]
        asset=Path(value['inputs'][kind]['path'])
        if env.get('JCE_BENCH_LOAD_KIND')!=kind or Path(env['JCE_BENCH_LOAD_FILE'])!=asset:
            raise ValueError('loader input authority differs')
        phase=validate_marker(log,kind,asset.stat().st_size,
            payload_size=Path(value['inputs']['glb']['path']).stat().st_size if kind=='bundle' else None)
        if phase!=case['phase']:raise ValueError('stored phases differ from raw log')
        asynchronous=bool(value.get('async_cpu_phases'))
        async_row=validate_async(log,phase,limit) if asynchronous else None
        if asynchronous and (env.get('JCE_BENCH_LOAD_ASYNC')!='1' or async_row!=case['async_evidence']):
            raise ValueError('stored worker evidence differs')
        if not asynchronous and 'JCE_BENCH_LOAD_ASYNC' in env:raise ValueError('sync authority contaminated')
        frames=checked_frames(directory/'frames.csv',limit)
        if case.get('actual_frames',limit)!=limit or case.get('actual_elapsed_s',process['elapsed_s'])!=process['elapsed_s']:
            raise ValueError('stored frame count or elapsed time differs')
        summary=frame_summary(frames)
        indices=list(frames['frame_indices']) if 'frame_indices' in frames else list(range(limit))
        wall=dict(zip(indices,frames['frame_ms']))
        completion=async_row['completion_frame'] if async_row else 20
        if 20 not in wall or completion not in wall:raise ValueError('loader frame timing unavailable')
        stall=(phase['submit_ns'] if asynchronous else phase['total_ns'])/1e6
        if case['main_thread_critical_section_ms']!=stall:raise ValueError('main-thread scope differs')
        computed[label]={'kind':kind,'phase':phase,'async_evidence':async_row,
            'frames':summary,'frame_ms':list(frames['frame_ms']),
            'frame_indices':indices,'request_frame_ms':wall[20],
            'completion_frame':completion,'completion_frame_ms':wall[completion],
            'main_thread_critical_section_ms':stall,'sampled_peak_rss_bytes':peak,
            'io_observation':io_observation(samples)}
    return value,{'backend':backend,'cases':computed}


def matched(first,second):
    a,ar=first;b,br=second
    if a['exe_sha256']!=b['exe_sha256'] or ar['backend']!=br['backend']:
        raise ValueError('direct comparison requires same executable and backend')
    if {k:v['sha256'] for k,v in a['inputs'].items()}!={k:v['sha256'] for k,v in b['inputs'].items()}:
        raise ValueError('direct comparison requires identical resource bytes')
    if bool(a.get('async_cpu_phases'))==bool(b.get('async_cpu_phases')):
        raise ValueError('comparison requires one synchronous and one asynchronous suite')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite',type=Path,action='append',required=True)
    parser.add_argument('--compare',action='store_true',help='require matched sync/async identities')
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    loaded=[load(path) for path in args.suite]
    if args.compare:
        if len(loaded)!=2:raise ValueError('comparison requires two suites')
        matched(*loaded)
    args.out.mkdir(parents=True,exist_ok=False)
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig,axes=plt.subplots(2,2,figsize=(13,8))
    colors=('tab:blue','tab:orange','tab:green','tab:red')
    for kind,ax in zip(('glb','texture','audio','bundle'),axes.flat):
        for index,(value,result) in enumerate(loaded):
            mode='worker' if value.get('async_cpu_phases') else 'synchronous'
            for label,case in result['cases'].items():
                if case['kind']!=kind:continue
                ax.plot(case['frame_indices'],case['frame_ms'],color=colors[index%4],alpha=.55,
                    label='%u %s'%(index,mode) if label.endswith('-0') else None)
                ax.scatter([20,case['completion_frame']],
                    [case['request_frame_ms'],case['completion_frame_ms']],color=colors[index%4],s=18)
        ax.set(title=kind,xlabel='Actual frame index (all retained frames)',ylabel='Frame wall ms')
        ax.legend()
    fig.tight_layout();fig.savefig(args.out/'frames.png',dpi=150);plt.close(fig)
    fig,ax=plt.subplots(figsize=(12,5))
    labels=[];cpu=[];submit=[]
    for index,(value,result) in enumerate(loaded):
        for kind in ('glb','texture','audio','bundle'):
            cases=[c for c in result['cases'].values() if c['kind']==kind]
            labels.append('%u/%s/%s'%(index,'worker' if value.get('async_cpu_phases') else 'sync',kind))
            cpu.append(statistics.median(sum(c['phase'][p] for p in PHASES[:-1])/1e6 for c in cases))
            submit.append(statistics.median(c['phase']['submit_ns']/1e6 for c in cases))
    x=list(range(len(labels)));ax.bar(x,cpu,label='Read/open/verify/decode API work')
    ax.bar(x,submit,bottom=cpu,label='Main-thread upload/register CPU submission')
    ax.set_xticks(x,labels,rotation=30,ha='right');ax.set_ylabel('Median phase work ms (not GPU completion)');ax.legend()
    fig.tight_layout();fig.savefig(args.out/'phases.png',dpi=150);plt.close(fig)
    lines=['# Loader phase observations','',
        'Cache uncontrolled. CPU worker phase time is not a main-thread stall. GPU submission is not GPU completion.',
        'All frames include initialization and diagnostics. Sampled process RSS is not incremental loader allocation, physical VRAM or old-device qualification.',
        'Different executable hashes remain separate observations; no unmatched speed ratio is asserted. Moving decode to workers does not remove upload or every frame stall.','',
        '| Suite/case | Binary | Main critical ms | Request frame ms | Completion frame/ms | All-frame p99/max ms | >16.67/33.33/50 ms | Sampled peak RSS MiB |',
        '|---|---|---:|---:|---|---|---|---:|']
    evidence=[]
    for index,(value,result) in enumerate(loaded):
        for label,case in result['cases'].items():
            f=case['frames'];peak=case['sampled_peak_rss_bytes']
            lines.append('| %u/%s | %s | %.4f | %.4f | %u/%.4f | %.4f/%.4f | %u/%u/%u | %s |'%(index,label,value['exe_sha256'][:12],case['main_thread_critical_section_ms'],case['request_frame_ms'],case['completion_frame'],case['completion_frame_ms'],f['frame_p99_ms'],f['frame_max_ms'],f['over_16_67_ms'],f['over_33_33_ms'],f['over_50_ms'],'unavailable' if peak is None else '%.2f'%(peak/1048576)))
        evidence.append({'suite':str(args.suite[index].resolve()),'suite_sha256':sha256_file(args.suite[index]),
            'exe_sha256':value['exe_sha256'],'async_cpu_phases':value.get('async_cpu_phases',False),**result})
    lines+=['','![All retained frames](frames.png)','![API phase work](phases.png)','']
    (args.out/'REPORT.md').write_text('\n'.join(lines),encoding='utf-8',newline='\n')
    write(args.out/'recomputed.json',{'scope':__doc__,'matched_pair':args.compare,'evidence':evidence})


if __name__=='__main__':main()
