#!/usr/bin/env python3
"""R12 base environment timing; effects lacking actual activation proof stay unavailable."""
import argparse
import json
import math
import re
import shutil
import statistics
from pathlib import Path
from static_asset_suite import fixture, glb_census
from check_asset_pixels import pack_fixture
from postfx_fixture import fixture as effect_fixture
from postfx_suite import validate_log
from tools.benchmark_suite import source_identity, read_frames, frame_summary, sha256_file
from tools.perf_bench import run, read_execution_log
from tools.jce_determinism import assert_backend
from stream_route_metrics import process_metrics

EFFECTS = ('taa', 'ssao', 'ssr', 'bloom', 'fog')


def validate_base(log, census, backend, effect='base'):
    assert_backend(log, backend)
    views=validate_log(log,effect)
    rows=re.findall(r'stock-model: frame=120 vertices=(\d+) primitives=(\d+)',log)
    if rows != [(str(census['vertices']),str(census['primitives']))]:
        raise ValueError('actual fixed environment topology not ready before timing')
    return {'topology':rows,'views':views}


def validate_world(log, asset, scale, position, pitch):
    if re.findall(r'stock-world: entities=(\d+) meshes=(\d+) cameras=(\d+) lights=(\d+) valid=(\d+)',log)!=[('3','1','1','1','1')]:
        raise ValueError('actual fixed scene membership differs')
    def numbers(prefix, expected):
        rows=re.findall(re.escape(prefix)+r'([^\r\n]+)',log)
        if len(rows)!=1:raise ValueError('one actual '+prefix+' witness required')
        fields=re.findall(r'-?\d+\.\d+',rows[0].split(' at ')[0].split(' path=')[0])
        values=list(map(float,fields))
        if len(values)!=len(expected) or any(not math.isfinite(v) or abs(v-e)>0.00002 for v,e in zip(values,expected)):
            raise ValueError('actual camera/mesh does not match authored fixture')
    meshes=re.findall(r'stock-mesh: scale=([^ ]+) path=([^\r\n]+)',log)
    if len(meshes)!=1 or meshes[0][1].split(' at ')[0]!=asset:
        raise ValueError('actual selected mesh path differs')
    numbers('stock-mesh: scale=',[scale]*3)
    numbers('stock-authored-camera: position=',[*position,40,.01,100])
    radians=math.radians(pitch)
    numbers('stock-camera: position=',[*position,0,math.sin(radians),-math.cos(radians),40,.01,100])
    return {'entities':3,'asset':asset,'position':position,'pitch':pitch}


def readiness_warmup(log):
    rows=re.findall(r'stock-ready: raw_frame=(\d+) elapsed_ms=(\d+)',log)
    if len(rows)!=1:
        raise ValueError('one actual readiness event required')
    frame,elapsed=map(int,rows[0])
    if frame<1 or frame>100000 or elapsed>=60000:
        raise ValueError('readiness exceeded bounded startup')
    # raw_frame is one-based after app_draw; the ready draw is logical frame1.
    return frame-1+120


def effect_deltas(runs):
    medians={}
    for effect, records in runs.items():
        if len(records)!=7:
            raise ValueError('seven complete repeats required for every effect delta')
        values=[record['summary']['frame_median_ms'] for record in records]
        if any(not math.isfinite(value) or value<=0 for value in values):
            raise ValueError('invalid measured frame timing')
        medians[effect]=statistics.median(values)
    if 'base' not in medians:
        raise ValueError('base timing required')
    return {effect:{'median_of_run_frame_medians_ms':value,
                    'delta_from_base_ms':value-medians['base']} for effect,value in medians.items()}


def write(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n', encoding='utf-8', newline='\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('exe', 'asset', 'packer', 'out'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--backend', choices=('d3d11','d3d12','vulkan','gl'), required=True)
    parser.add_argument('--camera', type=float, nargs=3, required=True)
    parser.add_argument('--pitch', type=float, required=True)
    parser.add_argument('--scale', type=float, default=1)
    parser.add_argument('--effect',choices=('all',*EFFECTS),default='all')
    args = parser.parse_args()
    if not all(math.isfinite(value) for value in (*args.camera,args.pitch,args.scale)) or args.scale<=0:
        parser.error('finite camera/pitch and positive scale required')
    args.out=args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    hashes={name:sha256_file(getattr(args,name)) for name in ('exe','asset','packer')}
    census=glb_census(args.asset.read_bytes())
    runtime=args.out/'runtime'
    cooked=runtime/'resources/_cooked'
    cooked.mkdir(parents=True)
    relocated=runtime/args.exe.name
    shutil.copyfile(args.exe,relocated)
    shutil.copyfile(args.asset,cooked/args.asset.name)
    asset_stage=args.out/'asset-input'
    asset_stage.mkdir()
    packed_asset=asset_stage/args.asset.name
    shutil.copyfile(args.asset,packed_asset)
    pak=pack_fixture(args.packer,packed_asset,args.out)
    hashes['pak']=sha256_file(pak)
    cases=('base',*(EFFECTS if args.effect=='all' else (args.effect,)))
    result={'status':'INCOMPLETE','source':source_identity(),'identity':hashes,'census':census,
            'backend':args.backend,'runs':{effect:[] for effect in cases},
            'scope':'Same fixed environment; seven runs per effect, warmup120/sample600, actual public view submissions and model topology; no pixel/readback observer during timing'}
    try:
        for effect in cases:
            for index in range(7):
                directory=args.out/('%s-%02d'%(effect,index))
                directory.mkdir()
                scene=fixture(args.asset.name,args.scale,args.camera,args.pitch,True)
                scene['rendering']=effect_fixture(effect)['rendering']
                scene_name='r12-fixed-'+hashes['asset'][:12]+'-'+effect+'.scene.json'
                write(cooked/scene_name,scene)
                write(directory/'scene.json',scene)
                env={'JCE_BACKEND':args.backend,'JCE_GPU_TIER':'high',
                     'JCE_STARTUP_SCENE':scene_name,'JCE_BENCH_POST_EFFECT':effect,
                     'JCE_BENCH_POST_MODEL':args.asset.name,
                     'JCE_BENCH_ASSET_PAK':str(pak),
                     'JCE_GPU_SCENE':'0','JCE_HIZ_OCCLUSION':'0'}
                process=run(env,directory/'engine.log',frames=100000,timeout=210,max_seconds=180,exe=relocated,
                            samples_path=directory/'frames.csv',sample_interval=.02)
                log=read_execution_log(directory/'engine.log',process)
                witness=validate_base(log,census,args.backend,effect)
                witness['world']=validate_world(log,args.asset.name,args.scale,args.camera,args.pitch)
                summary=frame_summary(read_frames(directory/'frames.csv',warmup=readiness_warmup(log),sample_count=600))
                resources=process_metrics(process)
                for name in ('exe','asset','packer'):
                    if sha256_file(getattr(args,name))!=hashes[name]:
                        raise ValueError('input artifact changed')
                if sha256_file(relocated)!=hashes['exe'] or sha256_file(cooked/args.asset.name)!=hashes['asset']:
                    raise ValueError('relocated artifact changed')
                if sha256_file(pak)!=hashes['pak']:
                    raise ValueError('packed fixture changed')
                result['runs'][effect].append({'process':process,'environment':env,'witness':witness,'sample_warmup_raw':readiness_warmup(log),
                    'summary':summary,'resources':resources,
                    'files':{name:sha256_file(directory/name) for name in ('scene.json','engine.log','frames.csv')}})
                write(args.out/'suite.json',result)
        result['deltas']=effect_deltas(result['runs'])
        result['status']='FIXED_ENVIRONMENT_EFFECT_TIMING_PASS'
    except Exception as error:
        result.update(status='FAIL',error=str(error))
        raise
    finally:
        write(args.out/'suite.json',result)


if __name__=='__main__':
    main()
