"""Real executable replay and capture; no engine or editor business hooks."""
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
PROJECT=Path(__file__).resolve().parents[1]
REPO=PROJECT.parents[1]
sys.path.insert(0,str(REPO/'tools'))
from jce_determinism import DETERMINISM, assert_backend
GAME=REPO/'dist/games/Minesweeper-release-x86_64/Minesweeper.exe'
OUT=PROJECT/'build/verification'

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

def run(name,frames=None,extra=None,max_frames=None,timeout=120):
    OUT.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ);env.update(DETERMINISM)
    for k in ('JCE_INPUT_REPLAY','JCE_CAPTURE_PATH','JCE_KPI_GAME_SHOTS','MINES_SELF_TEST'):env.pop(k,None)
    count=max_frames or max(120,len(frames or [])+30)
    env.update(JCE_BACKEND='d3d11',JCE_MULTI_INSTANCE='1',JCE_MAX_FRAMES=str(count),MINES_TRACE='1',MINES_SEED='7',MINES_SAVE_DIR=str(OUT/'save'))
    if frames is not None:
        path=OUT/(name+'.jirc');write_jirc(path,frames+idle(count-len(frames)))
        env['JCE_INPUT_REPLAY']=str(path)
    env['JCE_CAPTURE_FRAME']=str(count-10);env['JCE_CAPTURE_PATH']=str(OUT/(name+'.png'))
    if extra:env.update(extra)
    proc=subprocess.run([str(GAME)],cwd=GAME.parent,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=timeout)
    log=proc.stdout+proc.stderr;(OUT/(name+'.log')).write_text(log,encoding='utf-8')
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
    global GAME
    session=Path.home()/'.jce/editor-session.json'
    before=session.read_bytes() if session.exists() else None
    original_game=GAME
    try:
        data=json.loads(before) if before else {}
        data.update(last_project=str(PROJECT),last_scene_path=str(PROJECT/'resources/scenes/minesweeper.scene.json'))
        session.parent.mkdir(parents=True,exist_ok=True);session.write_text(json.dumps(data),encoding='utf-8')
        GAME=REPO/'build/desktop/windows-x64/release/jce_editor.exe'
        log=run('editor-play',idle(90)+key(30)+idle(100),extra={'JCE_KPI_AUTOPLAY':'1','JCE_DBG_FOCUS_GAME':'1','JCE_KPI_GAME_SHOTS':'2.00|'+str(OUT/'editor-game.png')},max_frames=280)
        assert 'MINES_BOOT' in log and 'action=start' in log
    finally:
        GAME=original_game
        if before is None:
            if session.exists():session.unlink()
        else:session.write_bytes(before)
        assert (session.read_bytes() if session.exists() else None)==before
    print('EDITOR PLAY PASS; session restored byte-for-byte')

def runtime_ai():
    frames=idle(80)+key(30)+idle(30)+click(100,210)+idle(1600)+key(41)+idle(20)+click(640,480)+idle(20)+click(500,535)+idle(40)
    log=run('runtime-ai',frames,extra={'MINES_AI_PYTHON':sys.executable,'MINES_AI_TOOL':str(PROJECT/'tools/ai_director.py'),'JCE_LLM_PROVIDER':'ollama','JCE_LLM_BASE_URL':'http://127.0.0.1:11435','JCE_LLM_MODEL':'qwen3.5:0.8b','NO_PROXY':'127.0.0.1,localhost,::1'},timeout=90)
    assert 'MINES_AI_APPLY' in log and 'challenge=10x10/12' in log and 'width=10 height=10 mines=12' in log
    print('REAL RUNTIME AI PASS')

def main():
    rules=run('integrated-rules',extra={'MINES_SELF_TEST':'1'})
    assert 'MINES_TEST PASS' in rules
    menu=run('integrated-menu',idle(130))
    assert 'MINES_BOOT' in menu and 'MINES_DIRECTOR applied' in menu
    frames=idle(80)+key(30)+idle(30)+click(456,218)+idle(30)
    log=run('integrated-reveal',frames)
    assert 'action=reveal' in log and 'state=1' in log
    if '--extended' in sys.argv:extended()
    if '--editor' in sys.argv:editor()
    if '--ai' in sys.argv:runtime_ai()
    return 0
if __name__=='__main__':raise SystemExit(main())
