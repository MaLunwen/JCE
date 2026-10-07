"""Sequential fresh-host statistics service witness; not a gameplay or speed test."""
import argparse
import json
from pathlib import Path
import sys
ROOT=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT));sys.path.insert(0,str(ROOT/'tests/consumers/benchmark/tools'))
from tools.perf_bench import run,read_execution_log
from tools.jce_determinism import assert_backend
from gbuffer_asset_suite import sha256_file,write

def validate_phase(log,process,phase,key,value,backend):
    if process['process']['returncode']!=0 or process['process']['timed_out']:
        raise ValueError('abnormal host exit')
    if '] ERROR -' in log or 'statistics host unavailable' in log:
        raise ValueError('actual service error')
    marker=f'RUNTIME_SERVICE_PROBE {phase} PASS key={key} value={value}'
    if log.count(marker)!=1:raise ValueError('exact actual service marker unavailable')
    actual=assert_backend(log,backend)
    if actual!=process['backend_actual']:raise ValueError('process backend differs')
    return actual

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('exe','writer-bundle','reader-bundle','out'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--backend',choices=('d3d11','d3d12','vulkan','gl'),default='d3d11');p.add_argument('--key',default='service_probe');p.add_argument('--value',type=int,default=123456);p.add_argument('--timeout',type=float,default=120)
    a=p.parse_args()
    if not all(v.is_file() for v in (a.exe,a.writer_bundle,a.reader_bundle)) or not 10<=a.timeout<=600:p.error('existing inputs and bounded timeout required')
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);store=out/'statistics';store.mkdir()
    inputs={'exe':sha256_file(a.exe),'writer':sha256_file(a.writer_bundle),'reader':sha256_file(a.reader_bundle)}
    result={'status':'INCOMPLETE','scope':'two fresh editor processes; actual set/flush then get, not gameplay victory, stock process exit or formal performance','input_sha256':inputs,'validator_sha256':sha256_file(Path(__file__)),'phases':[]}
    write(out/'intent.json',result)
    try:
        for phase,bundle in [('writer',a.writer_bundle),('reader',a.reader_bundle)]:
            trial=out/phase;trial.mkdir();profile=trial/'profile';profile.mkdir();write(profile/'editor-preferences.json',{'startup_mode':1})
            env={'JCE_SCENE':str(bundle.resolve()),'JCE_BACKEND':a.backend,'JCE_WINDOW_HIDDEN':'1','JCE_MULTI_INSTANCE':'1','JCE_EDITOR_PROFILE_DIR':str(profile),'JCE_RUNTIME_STATS_DIR':str(store),'JCE_DBG_AUTOPLAY':'20','JCE_DBG_FOCUS_GAME':'1'}
            write(trial/'environment.json',env)
            proc=run(env,trial/'engine.log',exe=a.exe.resolve(),frames=200,timeout=a.timeout,samples_path=trial/'frames.csv')
            log=read_execution_log(trial/'engine.log',proc['process']);actual=validate_phase(log,proc,phase,a.key,a.value,a.backend)
            files={str(v.relative_to(store)):sha256_file(v) for v in store.rglob('*') if v.is_file() and v.stat().st_size>0}
            if not files:raise ValueError('actual nonempty persistent store unavailable')
            result['phases'].append({'phase':phase,'process':proc,'actual_backend':actual,'store_sha256':files})
        if inputs!={'exe':sha256_file(a.exe),'writer':sha256_file(a.writer_bundle),'reader':sha256_file(a.reader_bundle)}:raise ValueError('input identity changed')
        if result['phases'][0]['store_sha256']!=result['phases'][1]['store_sha256']:raise ValueError('reader unexpectedly changed persistent store')
        result['status']='PUBLIC_STATISTICS_CROSS_PROCESS_OBSERVED'
    except Exception as error:
        result.update(status='FAIL',error=str(error));raise
    finally:
        result['raw_sha256']={str(v.relative_to(out)):sha256_file(v) for v in out.rglob('*') if v.is_file() and v.name!='result.json'}
        write(out/'result.json',result)
if __name__=='__main__':main()
