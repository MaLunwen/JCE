#!/usr/bin/env python3
"""Public loader phase observations; OS cache uncontrolled, no cold-load claim.

CPU upload submission wall time is not GPU completion. The measured critical
section excludes cleanup, is not the whole frame, and includes synchronous work.
Process IO counters have OS-defined semantics and do not certify physical disk IO.
"""
import argparse
import json
import os
import shutil
from pathlib import Path
import re
import struct
import wave
import numpy as np
from PIL import Image

from stream_route_suite import original_glb, scenes
from gbuffer_asset_suite import write, sha256_file
from postfx_fixture import fixture
from tools.benchmark_suite import source_identity
from tools.perf_bench import run, ROOT
from tools.process_sampler import run_process
from tools.jce_determinism import DETERMINISM

PHASES = ('read_ns','open_ns','decompress_verify_ns','decode_ns','submit_ns')
FIELDS = ('frame kind ok read_bytes packed_entry_bytes payload_bytes '+
          ' '.join(PHASES)+' total_ns').split()


def generate_inputs(directory, *, side=2048, seconds=60):
    directory.mkdir()
    model = directory/'set_0.glb'
    model.write_bytes(original_glb(0))
    y,x = np.indices((side,side),dtype=np.uint32)
    pixels = np.stack(((x*17+y*7)%256,(x*3+y*19)%256,(x*13+y*11)%256),axis=-1).astype(np.uint8)
    texture = directory/'texture.png'
    Image.fromarray(pixels).save(texture,compress_level=6)
    sound = directory/'audio.wav'
    with wave.open(str(sound),'wb') as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(48000)
        # Original deterministic bounded square wave; no playback is requested.
        block = b''.join(struct.pack('<hh',600 if i%100<50 else -600,
            600 if i%100<50 else -600) for i in range(48000))
        for _ in range(seconds):output.writeframesraw(block)
    return dict(glb=model,texture=texture,audio=sound)


def validate_marker(log, kind, size, *, payload_size=None, success=True):
    pattern = r'benchmark-load: '+ ' '.join(key+r'=(\w+)' for key in FIELDS)
    matches = re.findall(pattern,log)
    if len(matches)!=1:
        raise ValueError('missing or duplicate loader phase marker')
    row = dict(zip(FIELDS,matches[0]))
    for key in FIELDS:
        if key!='kind':row[key]=int(row[key])
    if row['frame']!=20 or row['kind']!=kind or row['ok']!=int(success) \
            or row['read_bytes']!=size or sum(row[p] for p in PHASES)!=row['total_ns']:
        raise ValueError('loader phase identity/byte accounting/sum failed')
    if success:
        if row['total_ns']<=0 or row['payload_bytes']<=0:
            raise ValueError('loader work unavailable')
        if kind!='bundle' and (row['payload_bytes']!=size or row['packed_entry_bytes'] \
                or row['open_ns'] or row['decompress_verify_ns']):
            raise ValueError('standalone phases incorrectly reported as bundle work')
        if payload_size is not None and row['payload_bytes']!=payload_size:
            raise ValueError('bundle payload differs from supplied original GLB')
    return row


def io_observation(samples):
    out={'scope':'sampled process OS IO counters; includes other engine IO; not physical disk bytes',
         'samples':len(samples)}
    for key in ('read_bytes','write_bytes'):
        values=[s[key] for s in samples if s.get(key) is not None]
        out[key+'_last']=values[-1] if values else None
        out[key+'_sampled_delta']=values[-1]-values[0] if len(values)>1 else None
    return out



def prepared_bundle(output, payload_size):
    catalog=json.loads((output/'bundle_catalog.json').read_text(encoding='utf-8'))
    report=json.loads((output/'build_report.json').read_text(encoding='utf-8'))
    bundles=catalog.get('bundles')
    if not isinstance(bundles,dict) or catalog.get('contract',{}).get('name')!='jce.bundle.catalog':
        raise ValueError('actual bundle catalog missing')
    matches=[]
    for bundle in report.get('bundles',[]):
        for entry in bundle.get('entries',[]):
            if entry.get('path')=='set_0.glb':
                record=bundles.get(bundle.get('name'),{})
                filename=record.get('file')
                if not filename or filename!=bundle.get('file') or entry.get('size_bytes')!=payload_size:
                    raise ValueError('bundle catalog/report entry mismatch')
                path=(output/filename).resolve()
                if not path.is_relative_to(output.resolve()) or path.suffix!='.jbundle' or not path.is_file():
                    raise ValueError('invalid produced bundle path')
                if record.get('size')!=path.stat().st_size or bundle.get('size_bytes')!=path.stat().st_size:
                    raise ValueError('produced bundle size differs from catalog')
                matches.append((path,entry['path']))
    if len(matches)!=1:
        raise ValueError('original GLB bundle entry unavailable or ambiguous')
    return matches[0]


def prepare_bundle(args, model):
    directory=args.out/'prepare'
    directory.mkdir()
    project=directory/'project'
    project.mkdir()
    shutil.copyfile(model,project/'set_0.glb')
    write(project/'fixture.scene.json',scenes()[1][0])
    output=directory/'bundles'
    env={key:value for key,value in os.environ.items() if not key.startswith('JCE_')}
    controls=dict(DETERMINISM,JCE_BACKEND=args.backend,JCE_MAX_FRAMES='1',
        JCE_LOG_FILE=str(directory/'engine.log'),JCE_BENCH_LOAD_PREPARE_ROOT=str(project),
        JCE_BENCH_LOAD_PREPARE_OUT=str(output))
    env.update(controls)
    process=run_process([str(args.exe.resolve())],env=env,cwd=str(ROOT),timeout=180,
        output_dir=directory/'process',interval=.02)
    log=(directory/'engine.log').read_text(encoding='utf-8')
    if process['returncode']!=0 or process['timed_out'] or '] ERROR -' in log \
            or log.count('benchmark-load-prepare: ok=1 result=0')!=1:
        raise ValueError('SDK public bundle preparation failed')
    bundle,entry=prepared_bundle(output,model.stat().st_size)
    return bundle,entry,{'process':process,'environment':controls,
        'scope':'untimed preparation via public SDK bundle pack; excluded from loader phases',
        'raw_sha256':{str(p.relative_to(directory)):sha256_file(p) for p in directory.rglob('*') if p.is_file()}}


def main(async_mode=False):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--backend',choices=('d3d11','d3d12','vulkan','gl'),required=True)
    parser.add_argument('--bundle',type=Path,help='Actual SDK-produced .jbundle, not renamed raw JPAK')
    parser.add_argument('--bundle-entry',default='set_0.glb')
    parser.add_argument('--frames',type=int,default=2000 if async_mode else 200,
        help='explicit diagnostic deadline; not a performance target')
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    if not 200<=args.frames<=100000:
        raise ValueError('frames must be200..100000')
    args.out=args.out.resolve()
    relative=args.out.relative_to(ROOT.resolve())
    if not relative.parts or relative.parts[0] not in ('build','reports'):
        raise ValueError('generated output must remain in build or reports')
    args.out.mkdir(parents=True,exist_ok=False)
    exe_hash=sha256_file(args.exe)
    inputs=generate_inputs(args.out/'inputs')
    preparation=None
    if args.bundle:
        inputs['bundle']=args.bundle.resolve()
    else:
        inputs['bundle'],args.bundle_entry,preparation=prepare_bundle(args,inputs['glb'])
    inputs_hash={kind:sha256_file(path) for kind,path in inputs.items()}
    if sha256_file(args.exe)!=exe_hash:
        raise ValueError('binary changed during preparation')
    result={'status':'INCOMPLETE','source':source_identity(),'exe':str(args.exe.resolve()),
        'exe_sha256':exe_hash,'inputs':{k:{'path':str(p),'sha256':inputs_hash[k]} for k,p in inputs.items()},
        'cache':'uncontrolled; sequential new-process repeats, not guaranteed warm/cold',
        'scope':__doc__,'cases':{},'preparation':preparation,'async_cpu_phases':async_mode,'frame_limit':args.frames}
    try:
        for kind,path in inputs.items():
            for trial in range(3):
                directory=args.out/('%s-%u'%(kind,trial))
                directory.mkdir()
                scene=fixture(empty=True)
                write(directory/'scene.json',scene)
                env={'JCE_BACKEND':args.backend,'JCE_SCENE':str(directory/'scene.json'),
                    'JCE_BENCH_LOAD_KIND':kind,'JCE_BENCH_LOAD_FILE':str(path),
                    'JCE_BENCH_LOAD_ENTRY':args.bundle_entry,'JCE_GPU_TIER':'high'}
                if async_mode:env['JCE_BENCH_LOAD_ASYNC']='1'
                process=run(env,directory/'engine.log',frames=args.frames,exe=args.exe.resolve(),
                    samples_path=directory/'frames.csv',sample_interval=.02)
                log=(directory/'engine.log').read_text(encoding='utf-8')
                if '] ERROR -' in log:raise ValueError('public loader logged error')
                marker=validate_marker(log,kind,path.stat().st_size,
                    payload_size=inputs['glb'].stat().st_size if kind=='bundle' else None)
                async_evidence=None
                if async_mode:
                    from load_async_suite import validate_async
                    async_evidence=validate_async(log,marker,args.frames)
                if sha256_file(args.exe)!=exe_hash or any(sha256_file(p)!=inputs_hash[k] for k,p in inputs.items()):
                    raise ValueError('loader input or binary changed')
                from tools.benchmark_suite import read_frames
                observed_frames=read_frames(directory/'frames.csv',warmup=0,sample_count=None)
                if len(observed_frames['frame_ms'])!=args.frames:
                    raise ValueError('actual frame count differs from deadline')
                files={str(p.relative_to(directory)):sha256_file(p) for p in directory.rglob('*') if p.is_file()}
                result['cases'][directory.name]={'phase':marker,'environment':env,'process':process,
                    'io_observation':io_observation(process['process_samples']),'raw_sha256':files,
                    'main_thread_critical_section_ms':(marker['submit_ns'] if async_mode else marker['total_ns'])/1e6,
                    'async_evidence':async_evidence,'actual_frames':len(observed_frames['frame_ms']),
                    'actual_elapsed_s':process['process']['elapsed_s'],
                    'gpu_completion':'unavailable; submit API wall time only'}
                write(args.out/'suite.json',result)
        result['status']='LOAD_PHASE_OBSERVATION_ONLY'
    except Exception as error:
        result.update(status='FAIL',error=str(error))
        raise
    finally:write(args.out/'suite.json',result)


if __name__=='__main__':main()
