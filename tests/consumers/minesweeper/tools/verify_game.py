"""Real executable replay and capture; no engine or editor business hooks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
PROJECT=Path(__file__).resolve().parents[1]
REPO=PROJECT.parents[2]
sys.path.insert(0,str(REPO/'tools'))
from jce_determinism import DETERMINISM, assert_backend
GAME=None
EDITOR=None
OUT=None

def write_jirc(path,frames):
    with path.open('wb') as f:
        f.write(b'JIRC'+struct.pack('<III',2,2336,0))
        for keys,x,y,buttons in frames:
            frame=bytearray(2336);struct.pack_into('<II',frame,0,2,512)
            for key in keys:
                at=8+(key//64)*8
                struct.pack_into('<Q',frame,at,struct.unpack_from('<Q',frame,at)[0]|(1<<(key%64)))
            struct.pack_into('<5fI',frame,520,x,y,0,0,0,buttons)
            f.write(frame)

def idle(n=30):return [((),0,0,0)]*n

def click(x,y,button=1):return [((),x,y,0)]*3+[((),x,y,1<<(button-1))]*3+[((),x,y,0)]*5

def key(code):return [((code,),0,0,0)]*3+idle(5)

def run(name,frames=None,extra=None,max_frames=None,timeout=120,exe=None):
    exe=Path(exe) if exe is not None else GAME
    OUT.mkdir(parents=True,exist_ok=True)
    env={k:v for k,v in os.environ.items() if not k.startswith('JCE_')}
    env.update(DETERMINISM)
    env.pop('MINES_SELF_TEST',None)
    count=max_frames or max(120,len(frames or [])+30)
    env.update(JCE_BACKEND='d3d11',JCE_MULTI_INSTANCE='1',JCE_MAX_FRAMES=str(count),MINES_TRACE='1',MINES_SEED='7',MINES_SAVE_DIR=str(OUT/'save'))
    if frames is not None:
        path=OUT/(name+'.jirc');write_jirc(path,frames+idle(count-len(frames)))
        env['JCE_INPUT_REPLAY']=str(path)
    env['JCE_CAPTURE_FRAME']=str(count-10);env['JCE_CAPTURE_PATH']=str(OUT/(name+'.png'))
    if extra:env.update(extra)
    started=time.perf_counter()
    before=after=None
    before_error=after_error=None
    proc=None
    failure=None
    failure_stage=None
    stdout=stderr=None
    status='CHILD_RETURNED'
    try:
        before=hashlib.sha256(exe.read_bytes()).hexdigest()
    except OSError as exc:
        before_error=str(exc);failure=exc;failure_stage='hash_before'
        status='ARTIFACT_READ_FAILED_BEFORE'
    if failure is None:
        try:
            proc=subprocess.run([str(exe)],cwd=exe.parent,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=timeout)
            stdout,stderr=proc.stdout,proc.stderr
        except subprocess.TimeoutExpired as exc:
            stdout,stderr=exc.stdout,exc.stderr
            failure=exc;failure_stage='child';status='TIMEOUT'
        except OSError as exc:
            failure=exc;failure_stage='launch';status='LAUNCH_FAILED'
    try:
        after=hashlib.sha256(exe.read_bytes()).hexdigest()
    except OSError as exc:
        after_error=str(exc)
        if failure is None:
            failure=exc;failure_stage='hash_after';status='ARTIFACT_READ_FAILED_AFTER'
    # TimeoutExpired may retain bytes even for a text-mode child. Keep all
    # available output under the same UTF-8 replacement policy as normal runs.
    def output_text(value):
        if isinstance(value,bytes):return value.decode('utf-8',errors='replace')
        return value or ''
    log=output_text(stdout)+output_text(stderr)
    if failure is not None:
        log+='\n[verify-game] status='+status+' stage='+failure_stage+' error='+str(failure)+'\n'
    log_path=OUT/(name+'.log')
    log_path.write_text(log,encoding='utf-8',newline='\n')
    receipt={'executable':str(exe),'sha256_before':before,'sha256_after':after,
             'hash_before_status':'AVAILABLE' if before is not None else 'UNAVAILABLE',
             'hash_after_status':'AVAILABLE' if after is not None else 'UNAVAILABLE',
             'hash_before_error':before_error,'hash_after_error':after_error,
             'identity_stable':before is not None and after is not None and before==after,
             'exit':proc.returncode if proc is not None else None,'status':status,
             'failure_stage':failure_stage,
             'failure_type':type(failure).__name__ if failure is not None else None,
             'failure':str(failure) if failure is not None else None,
             'failure_errno':getattr(failure,'errno',None),
             'failure_winerror':getattr(failure,'winerror',None),
             'timeout_s':timeout if status=='TIMEOUT' else None,
             'elapsed_s':time.perf_counter()-started,'frames':count,
             'synthetic_replay':frames is not None,
             'stdout_available':stdout is not None,'stderr_available':stderr is not None,
             'log_sha256':hashlib.sha256(log_path.read_bytes()).hexdigest()}
    (OUT/(name+'-artifact.json')).write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
    if failure is not None:raise failure
    assert before==after,'Executable changed during verification'
    print(name,'exit',proc.returncode,flush=True)
    for line in log.splitlines():
        if 'MINES_' in line or 'Lua error' in line:print(line,flush=True)
    assert proc.returncode==0,(name,proc.returncode)
    assert_backend(log,'d3d11')
    return log


def extended():
    base=idle(80)+key(30)+idle(30)+click(456,218)
    log=run('integrated-reveal',base+idle(30))
    layout=re.findall(r'layout=([0-8*]+)',log)[-1]
    def cell(i,b=1):return click(433+(i%9)*46+23,195+(i//9)*46+23,b)
    loss=run('loss',base+cell(8,3)+cell(layout.index('*'))+idle(60))
    assert 'state=3' in loss
    # A fresh save namespace makes this a write test on every invocation.
    import tempfile
    save=Path(tempfile.mkdtemp(prefix='mines-best-',dir=OUT))
    win=run('win',base+sum((cell(i) for i,c in enumerate(layout) if c!='*'),[])+idle(60),extra={'MINES_SAVE_DIR':str(save)})
    assert 'state=2' in win and 'MINES_BEST' in win and 'persisted=1' in win
    run('best-readback',idle(80)+key(30)+idle(30),extra={'MINES_SAVE_DIR':str(save)})
    presets=run('presets',idle(80)+key(31)+idle(20)+key(32)+idle(20)+key(17)+idle(30))
    assert 'width=16 height=16 mines=40' in presets and 'width=30 height=16 mines=99' in presets
    flags=run('flags',idle(80)+key(30)+idle(30)+cell(0,3)+cell(0)+cell(0,3)+cell(0)+idle(30))
    assert flags.count('action=flag index=0')==2 and flags.count('action=reveal index=0')==1
    print('EXTENDED GAME PASS')

def editor():
    profile=OUT/'editor-profile'
    profile.mkdir(exist_ok=False)
    (profile/'editor-session.json').write_text(json.dumps({
        'last_project':str(PROJECT),
        'last_scene_path':str(PROJECT/'resources/scenes/minesweeper.scene.json')}),encoding='utf-8')
    log=run('editor-play',idle(90)+key(30)+idle(100),exe=EDITOR,
        extra={'JCE_EDITOR_PROFILE_DIR':str(profile),'JCE_KPI_AUTOPLAY':'1',
               'JCE_DBG_FOCUS_GAME':'1','JCE_KPI_GAME_CAPTURE_SIZE':'1280x720',
               'JCE_KPI_GAME_SHOTS':'2.00|'+str(OUT/'editor-game.png')},max_frames=280)
    assert 'MINES_BOOT' in log and 'action=start' in log
    print('EDITOR PLAY PASS; isolated profile, synthetic replay')

def runtime_ai():
    frames=idle(80)+key(30)+idle(30)+click(100,210)+idle(1600)+key(41)+idle(20)+click(640,480)+idle(20)+click(500,535)+idle(40)
    log=run('runtime-ai',frames,extra={'MINES_AI_PYTHON':sys.executable,'MINES_AI_TOOL':str(PROJECT/'tools/ai_director.py'),'JCE_LLM_PROVIDER':'ollama','JCE_LLM_BASE_URL':'http://127.0.0.1:11435','JCE_LLM_MODEL':'qwen3.5:0.8b','NO_PROXY':'127.0.0.1,localhost,::1'},timeout=90)
    assert 'MINES_AI_APPLY' in log and 'challenge=10x10/12' in log and 'width=10 height=10 mines=12' in log
    print('REAL RUNTIME AI PASS')

def main():
    global GAME,EDITOR,OUT
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe',default=os.environ.get('MINES_VERIFY_GAME'),
                    help='Explicit current game executable; no build-path guessing')
    ap.add_argument('--editor-exe',default=os.environ.get('MINES_VERIFY_EDITOR'))
    ap.add_argument('--out',default=os.environ.get('MINES_VERIFY_OUT'),
                    help='Fresh verification directory')
    ap.add_argument('--extended',action='store_true')
    ap.add_argument('--editor',action='store_true')
    ap.add_argument('--ai',action='store_true')
    args=ap.parse_args()
    if not args.exe or not Path(args.exe).is_file():
        ap.error('--exe (or MINES_VERIFY_GAME) must name an existing executable')
    if args.editor and (not args.editor_exe or not Path(args.editor_exe).is_file()):
        ap.error('--editor-exe (or MINES_VERIFY_EDITOR) is required with --editor')
    if not args.out:
        ap.error('--out (or MINES_VERIFY_OUT) must select a fresh directory')
    GAME=Path(args.exe).resolve()
    EDITOR=Path(args.editor_exe).resolve() if args.editor_exe else None
    OUT=Path(args.out).resolve()
    if OUT.exists() and (not OUT.is_dir() or any(OUT.iterdir())):
        ap.error('Verification output must be absent or empty; preserve earlier evidence')
    OUT.mkdir(parents=True,exist_ok=True)
    rules=run('integrated-rules',extra={'MINES_SELF_TEST':'1'})
    assert 'MINES_TEST PASS' in rules
    menu=run('integrated-menu',idle(130))
    assert 'MINES_BOOT' in menu and 'MINES_DIRECTOR applied' in menu
    frames=idle(80)+key(30)+idle(30)+click(456,218)+idle(30)
    log=run('integrated-reveal',frames)
    assert 'action=reveal' in log and 'state=1' in log
    if args.extended:extended()
    if args.editor:editor()
    if args.ai:runtime_ai()
    return 0
if __name__=='__main__':raise SystemExit(main())
