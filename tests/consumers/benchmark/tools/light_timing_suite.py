#!/usr/bin/env python3
"""True 0/10/100 light-component timing; separate correctness captures required."""
import argparse
from pathlib import Path
import re

from light_suite import scene, write, sha256_file
from tools.benchmark_suite import source_identity, read_frames, frame_summary
from tools.perf_bench import run
from tools.jce_determinism import assert_backend

WARMUP, MEASURED = 120, 600


def timing_scene(kind, count):
    if count not in (0,10,100):raise ValueError('unsupported actual component count')
    result=scene(kind,range(count))
    light_type='PointLight' if kind=='point' else 'SpotLight'
    filtered=[]
    for entity in result['entities']:
        types={component['type'] for component in entity['components']}
        if 'DirectionalLight' in types:continue
        if light_type in types and int(entity['name'][5:])>=count:continue
        filtered.append(entity)
    result['entities']=filtered
    return result


def validate_inventory(log, kind, count):
    matches=re.findall(r'benchmark-light-timing: frame=(\d+) kind=(\w+) requested=(\d+) point=(\d+) spot=(\d+) directional=(\d+) area=(\d+) positive=(\d+)',log)
    expected=('100',kind,str(count),str(count if kind=='point' else 0),
        str(count if kind=='spot' else 0),'0','0',str(count))
    if matches!=[expected]:raise ValueError('actual light inventory differs or was measured repeatedly')
    if '] ERROR -' in log or 'JCE_FORWARDPLUS set -> r.forwardplus ON' not in log:
        raise ValueError('renderer error or Forward+ authority absent')
    return dict(point=count if kind=='point' else 0,spot=count if kind=='spot' else 0,
        directional=0,area=0,positive=count,observed_frame=100)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--backend',choices=('d3d11','d3d12','vulkan','gl'),default='d3d11')
    parser.add_argument('--repeats',type=int,choices=(3,7),default=3)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    args.out=args.out.resolve();args.out.mkdir(parents=True,exist_ok=False)
    identity=sha256_file(args.exe)
    result=dict(status='INCOMPLETE',source=source_identity(),exe=str(args.exe.resolve()),
        exe_sha256=identity,backend=args.backend,repeats=args.repeats,warmup=WARMUP,
        measured_frames=MEASURED,cases={},scope=__doc__,
        fixture_generator_sha256={name:sha256_file(Path(__file__).parent/name)
            for name in ('light_suite.py','light_timing_suite.py')},
        limitations=['Frame100 inventory is outside measured120..719 interval.',
            'Actual light component population is not actual shader evaluation count.',
            'Correctness requires separate archived pixel suite; this timing run does not capture PNGs.'])
    try:
        for kind in ('point','spot'):
            for count in (0,10,100):
                for repeat in range(args.repeats):
                    directory=args.out/('%s-%u-%u'%(kind,count,repeat));directory.mkdir()
                    write(directory/'scene.json',timing_scene(kind,count))
                    fixture_hash=sha256_file(directory/'scene.json')
                    env=dict(JCE_BACKEND=args.backend,JCE_FORWARDPLUS='1',JCE_GPU_TIER='high',
                        JCE_SCENE=str(directory/'scene.json'),JCE_BENCH_ASSET_STILL='1',
                        JCE_BENCH_LIGHT_TIMING_KIND=kind,JCE_BENCH_LIGHT_TIMING_COUNT=str(count),
                        JCE_WINDOW_HIDDEN='1')
                    process=run(env,directory/'engine.log',frames=WARMUP+MEASURED,
                        exe=args.exe.resolve(),samples_path=directory/'frames.csv')
                    log=(directory/'engine.log').read_text(encoding='utf-8')
                    assert_backend(log,args.backend)
                    inventory=validate_inventory(log,kind,count)
                    if sha256_file(args.exe)!=identity or sha256_file(directory/'scene.json')!=fixture_hash:
                        raise ValueError('binary or fixture changed during timing')
                    all_frames=read_frames(directory/'frames.csv',warmup=0,sample_count=None)
                    if len(all_frames['frame_ms'])!=WARMUP+MEASURED:
                        raise ValueError('actual complete frame count differs')
                    measured=read_frames(directory/'frames.csv',warmup=WARMUP,sample_count=MEASURED)
                    result['cases'][directory.name]=dict(environment=env,process=process,
                        inventory=inventory,fixture_sha256=fixture_hash,frame=frame_summary(measured),
                        all_frame_count=len(all_frames['frame_ms']),
                        raw_sha256={str(p.relative_to(directory)):sha256_file(p) for p in directory.rglob('*') if p.is_file()})
                    write(args.out/'suite.json',result)
        result['status']='LIGHT_COMPONENT_TIMING_OBSERVED'
    except Exception as error:
        result.update(status='FAIL',error=str(error));raise
    finally:write(args.out/'suite.json',result)


if __name__=='__main__':main()
