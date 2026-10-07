"""Fresh portable Snake bundle witness; synthetic replay, not hardware latency."""
import argparse
import json
from pathlib import Path
import re
import sys
ROOT=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT));sys.path.insert(0,str(ROOT/'tests/consumers/benchmark/tools'));sys.path.insert(0,str(ROOT/'tests/consumers/snake_seven'))
from verify import write_jirc
from tools.perf_bench import run,read_execution_log
from tools.jce_determinism import assert_backend,read_png
from gbuffer_asset_suite import sha256_file,write

def validate(log,scene,image):
    if '] ERROR -' in log or 'no cpp script class' in log or 'failed to instantiate' in log:raise ValueError('actual bundle script error')
    for role in ('manager','input','board','body','food','HUD'):
        if f'SNAKE_PORTABLE {role} Lua started' not in log:raise ValueError('portable role boot missing')
    rows=[tuple(map(int,m)) for m in re.findall(r'SNAKETRACE tick=(\d+) state=(\d+) score=(\d+) len=(\d+) head=(\d+),(\d+)',log)]
    if len(rows)<3 or not any(state==1 and z<10 for tick,state,score,length,x,z in rows):raise ValueError('actual absolute UP gameplay unavailable')
    if any(length<3 or score%10 for _,_,score,length,_,_ in rows):raise ValueError('invalid score or body length')
    body=scene.get('scene',scene);entities=body['entities']
    for e in entities:
        for c in e.get('components',[]):
            if c.get('type')=='Script' and not c.get('scriptPath','').endswith('_portable.lua'):raise ValueError('external driver in actual observed scene')
    state=next(e for e in entities if e['name']=='GameState');transform=next(c for c in state['components'] if c['type']=='Transform')
    if transform['posX']!=3:raise ValueError('actual wall game-over state unavailable')
    w,h,ch,pixels=image
    if w<160 or h<160 or ch<3 or len(pixels)!=w*h*ch:raise ValueError('Game capture incomplete')
    colors=set(tuple(pixels[i:i+3]) for i in range(0,len(pixels),ch))
    if len(colors)<16:raise ValueError('Game image flat; gameplay scene unavailable')
    return {'status':'PORTABLE_SNAKE_ROLES_INPUT_AND_WALL_OBSERVED','trace_rows':len(rows),'scope':'synthetic replay and actual Game capture/state, not all food/self-collision outcomes or full victory'}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('exe','bundle','out'):p.add_argument('--'+n,type=Path,required=True)
    p.add_argument('--backend',choices=('d3d11','d3d12','vulkan','gl'),default='d3d11');p.add_argument('--timeout',type=float,default=180);a=p.parse_args()
    if not a.exe.is_file() or not a.bundle.is_file():p.error('existing inputs required')
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);profile=out/'profile';profile.mkdir();write(profile/'editor-preferences.json',{'startup_mode':1})
    replay=out/'input.jirc';frames=[[] for _ in range(500)]
    frames[50]=[40]
    for i in range(80,90):frames[i]=[82]
    frames[110]=[19];frames[150]=[19];write_jirc(replay,frames)
    inputs={'exe':sha256_file(a.exe),'bundle':sha256_file(a.bundle)}
    env={'JCE_SCENE':str(a.bundle.resolve()),'JCE_BACKEND':a.backend,'JCE_WINDOW_HIDDEN':'1','JCE_MULTI_INSTANCE':'1','JCE_EDITOR_PROFILE_DIR':str(profile),'JCE_FRAME_DT_FIXED':'0.016666666667','JCE_INPUT_REPLAY':str(replay),'JCE_DBG_AUTOPLAY':'20','JCE_DBG_FOCUS_GAME':'1','JCE_KPI_GAME_SHOTS':'2.0|'+str(out/'game.png'),'JCE_KPI_GAME_CAPTURE_SIZE':'1280x720','JCE_EDITOR_OPERATION_METRICS':'1','JCE_DBG_SCENE_OBSERVE_DIR':str(out),'JCE_DBG_SCENE_OBSERVE_AT':'100,130,180,350'}
    result={'status':'INCOMPLETE','environment':env,'input_sha256':inputs,'validator_sha256':sha256_file(Path(__file__))};write(out/'intent.json',result)
    try:
        proc=run(env,out/'engine.log',exe=a.exe.resolve(),frames=500,timeout=a.timeout,samples_path=out/'frames.csv')
        if proc['process']['returncode']!=0 or proc['process']['timed_out']:raise ValueError('abnormal process exit')
        log=read_execution_log(out/'engine.log',proc['process']);actual=assert_backend(log,a.backend)
        result.update(validate(log,json.loads((out/'scene-350.json').read_text()),read_png(out/'game.png')));result.update(process=proc,actual_backend=actual)
        if inputs!={'exe':sha256_file(a.exe),'bundle':sha256_file(a.bundle)}:raise ValueError('inputs changed')
    except Exception as error:result.update(status='FAIL',error=str(error));raise
    finally:result['raw_sha256']={str(v.relative_to(out)):sha256_file(v) for v in out.rglob('*') if v.is_file() and v.name!='result.json'};write(out/'result.json',result)
if __name__=='__main__':main()
