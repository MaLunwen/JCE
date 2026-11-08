"""Real-clock, capture-free known native protocols; raw samples are retained."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from datetime import datetime, timezone
from uuid import uuid4
import verify_game as v
sys.path.insert(0,str(v.REPO/'tools'))
from process_sampler import run_process
from jce_determinism import DETERMINISM, assert_backend
from native_performance_evidence import PROTOCOLS, RAW_NAMES, capture_frame_budget, clock_points, clock_errors, identity, parse_kpi, protocol_named, validate_native_performance


def now():return datetime.now(timezone.utc).isoformat()


def write_json(path,value):
    path.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8',newline='\n')


def observe_competition():
    import psutil
    forbidden={'towerdefense.exe','jce_editor.exe','cl.exe','link.exe','ninja.exe','cmake.exe',
               'clang.exe','clang++.exe','wasm-ld.exe','make.exe','gmake.exe','emcc.exe',
               'qrenderdoc.exe','renderdoccmd.exe'}
    forbidden.update((v.GAME.name.lower(),v.EDITOR.name.lower()))
    scripts=('benchmark_suite.py','benchmark_scene.py','perf_bench.py','verify_performance.py',
             'verify_game.py','verify_visual.py','verify_interaction_render.py','verify_preview.py',
             'verify_reference_inputs.py','verify_delivery.py','verify_reference_shader_boundary.js',
             'verify_web_acceptance.js','verify_web_performance.js','verify_web_render_quality.js',
             'tools/lint/','tools/audit/','scripts/jce.py lint','scripts/jce.py test')
    observed={'observed_utc':now(),'processes':[],'competing_processes':[]}
    for process in psutil.process_iter(['name','cmdline']):
        arguments=process.info['cmdline'];name=process.info['name']
        command=' '.join(arguments or []).replace('\\','/').lower()
        row={'pid':process.pid,'name':name,'cmdline':arguments}
        observed['processes'].append(row)
        competing_script=(name or '').lower().startswith(('python','node','bun','deno')) and any(token in command for token in scripts)
        if process.pid!=os.getpid() and ((name or '').lower() in forbidden or competing_script):
            observed['competing_processes'].append(row)
    return observed


def guarded_process(exe,env,folder,timeout):
    # Keep both original inventories even when the named guard rejects a run.
    guard={'scope':'Known named native/build/QA processes before and after each child. Ordinary desktop/background and inaccessible process details remain visible/unavailable; this is not proof of full-machine GPU idleness.',
           'before':observe_competition(),'after':None}
    write_json(folder/'guard.json',guard)
    if guard['before']['competing_processes']:raise RuntimeError('Timing isolation unavailable before run; see '+str(folder/'guard.json'))
    before=identity(exe);started=now()
    result=run_process([str(exe)],env=env,cwd=exe.parent,timeout=timeout,output_dir=folder,marker_path=folder/'frames.csv')
    finished=now();guard['after']=observe_competition();write_json(folder/'guard.json',guard)
    if guard['after']['competing_processes']:raise RuntimeError('Timing isolation unavailable after run; see '+str(folder/'guard.json'))
    after=identity(exe)
    if before['sha256']!=after['sha256']:raise RuntimeError('Executable changed during native run')
    raw={key:identity(folder/filename) for key,filename in RAW_NAMES.items()}
    return result,{'raw_dir':str(folder),'raw_evidence':raw,'started_utc':started,'finished_utc':finished,'exe_sha256':after['sha256']}


def measurement_environment(folder,frames):
    environment={key:value for key,value in os.environ.items() if not key.startswith(('JCE_','TD_'))}
    environment.update(DETERMINISM);environment.pop('JCE_FRAME_DT_FIXED',None)
    environment.update(JCE_BACKEND='d3d11',JCE_MULTI_INSTANCE='1',JCE_MAX_FRAMES=str(frames),
                       JCE_KPI_FRAME_LOG=str(folder/'frames.csv'),TD_TRACE='1',TD_SEED='7',
                       JCE_TRACE='0',JCE_TRACE_STREAM='',JCE_TRACE_EXPORT='')
    return environment


def main():
    import psutil
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--protocol',choices=tuple(PROTOCOLS),default='standard')
    parser.add_argument('--resume',type=Path)
    args=parser.parse_args();protocol=protocol_named(args.protocol)
    v.OUT.mkdir(parents=True,exist_ok=True)
    if (v.OUT/'performance-suite.json').exists():raise ValueError('Use a fresh TD_VERIFY_OUT; historical complete reports are never overwritten')
    out=(v.OUT/'performance'/('native-'+args.protocol+'-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S')+'-'+uuid4().hex[:8])).resolve()
    out.mkdir(parents=True,exist_ok=False)
    artifacts={'native':hashlib.sha256(v.GAME.read_bytes()).hexdigest(),'editor':hashlib.sha256(v.EDITOR.read_bytes()).hexdigest()}
    maximum=capture_frame_budget(protocol)
    warmup_method=(f"{protocol['warmup_frames']} warmup" if args.protocol=='standard' else
                   f"{protocol['startup_discard_frames']} startup frames discarded; at least {protocol['warmup_min_ms']} ms of continuous realtime warmup with a {protocol['warmup_max_frames']}-frame budget")
    report={'schema':1,'protocol':protocol,'status':'NATIVE_TIMING_PROTOCOL_PARTIAL','complete':False,'errors':[],
            'method':f"{warmup_method} + {protocol['measured_frames']} measured frames, {protocol['repeats']} repeats per host/mode; hidden D3D11, max_frames={maximum}; no screenshot encoding, realtime dt; asynchronous CPU/GPU stats are not exactly frame-aligned. The resolution label is not an applied or observed render extent; the capture-free Editor follows its Game panel policy, so hosts remain separate workloads, without equal-FBO or throughput claims. Publication retains the bounded full process capture, including frames after its dynamic measured window; the frame budget does not enlarge trace capacity. Startup and elapsed warmup are excluded from measured summaries. Linear percentile interpolation matches the prior numpy default; raw CSV spikes are retained.",
            'process_inventory':observe_competition(),
            'hardware':{'logical_cpu_count':psutil.cpu_count(),'physical_cpu_count':psutil.cpu_count(logical=False),'ram_bytes':psutil.virtual_memory().total},
            'conditions':{'resolution':[1280,720],
                          'resolution_scope':'protocol_label_only_unverified_render_surface',
                          'actual_window_size':None,'actual_game_fbo_size':None,'same_render_surface_comparison':False,
                          'vsync_requested':1,'fixed_dt':False,'stream_sync':True,'visible_window':False,'contended':False,
                          'guard':'Per-run original before/after process inventories; named native/build/QA rejection. Executor separately schedules browser/GPU exclusivity. Ordinary desktop/background processes remain.'},
            'runs':[]}
    for kind in ('cpu','gpu'):
        path=v.OUT/(kind+'-hardware.json')
        if path.exists():report['hardware'][kind]=json.loads(path.read_text(encoding='utf-8-sig'))
    if args.resume:
        prior=json.loads(args.resume.read_text(encoding='utf-8-sig'))
        if prior.get('protocol')!=protocol:raise ValueError('Resume protocol differs; no prior report/raw files will be overwritten')
        validate_native_performance(prior,artifacts,allow_partial=True)
        report['runs']=prior['runs'];report['resumed_from']=identity(args.resume)
    write_json(out/'method.json',report)
    for host,exe in [('player',v.GAME),('editor',v.EDITOR)]:
        for mode,options in [('menu',{}),('combat',{'TD_AUTODEFENSE':'1'}),('stress-200',{'TD_STRESS':'200'})]:
            for repeat in range(protocol['repeats']):
                if any(row['host']==host and row['mode']==mode and row['repeat']==repeat for row in report['runs']):continue
                folder=out/f'{host}-{mode}-{repeat}';folder.mkdir()
                env=measurement_environment(folder,maximum);env.update(options)
                if host=='editor':
                    profile=folder/'profile';profile.mkdir()
                    write_json(profile/'editor-session.json',{'last_project':str(v.PROJECT),'last_scene_path':str(v.PROJECT/'resources/scenes/td_lab.scene.json')})
                    env.update(JCE_EDITOR_PROFILE_DIR=str(profile),JCE_KPI_AUTOPLAY='1',JCE_DBG_FOCUS_GAME='1',JCE_KPI_GAME_CAPTURE_SIZE='1280x720')
                result,binding=guarded_process(exe,env,folder,300 if args.protocol=='publication' else 120)
                log=result['stdout']+result['stderr'];assert result['returncode']==0 and not result['timed_out']
                assert_backend(log,'d3d11');assert 'TD_BOOT' in log
                assert 'actor prefab failed to spawn' not in log and 'camera proj:' not in log
                if mode=='stress-200':assert 'spawned=200' in log
                if mode=='combat':assert re.search(r'shots=[1-9]',log)
                stats=parse_kpi(folder/'frames.csv',protocol)
                record={'host':host,'mode':mode,'repeat':repeat,**binding,'kpi':stats,'fps':1000/stats['frame_wall']['mean_ms'],
                        'process':{key:value for key,value in result.items() if key not in ('stdout','stderr','samples')}}
                report['runs'].append(record)
                write_json(folder/'measurement.json',record);write_json(out/'partial.json',report)
                rss=result['peak_rss_bytes'];rss='UNAVAILABLE' if rss is None else round(rss/1048576,1)
                print(host,mode,repeat,'fps',round(record['fps'],2),'p95_ms',round(stats['frame_wall']['p95_ms'],3),'rss_mb',rss,flush=True)
    folder=out/'live-clock';folder.mkdir()
    env=measurement_environment(folder,700);env.update(TD_TIMING='1',TD_TIMING_START='1')
    result,binding=guarded_process(v.GAME,env,folder,60)
    log=result['stdout']+result['stderr'];assert result['returncode']==0 and not result['timed_out'];assert_backend(log,'d3d11')
    points=clock_points(log);slope_error,progress_error=clock_errors(points)
    report['live_clock']={**binding,'process':{key:value for key,value in result.items() if key not in ('stdout','stderr','samples')},'points':points,'elapsed_clock_error_s':slope_error,'simulation_step_progress_error':progress_error,
                          'policy':'60 Hz simulation, max 15 steps / 0.25 seconds accepted per update; excess simulation time discarded; host suspension is excluded by the runtime','pass':True}
    print('LIVE CLOCK PASS',slope_error,progress_error,flush=True)
    report.update(complete=True,status='COMPLETE_NATIVE_TIMING_PROTOCOL_OBSERVED')
    validate_native_performance(report,artifacts)
    write_json(out/'complete.json',report);write_json(v.OUT/'performance-suite.json',report)
    print('NATIVE PROTOCOL COMPLETE',protocol['id'],len(report['runs']),out,flush=True)


if __name__=='__main__':main()
