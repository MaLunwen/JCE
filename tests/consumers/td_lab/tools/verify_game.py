"""Run real SDK artifacts, replay input and preserve logs/captures on failures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

PROJECT = Path(__file__).resolve().parents[1]
REPO = PROJECT.parents[2]
sys.path.insert(0,str(REPO/'tools'))
from jce_determinism import DETERMINISM, assert_backend
OUT = Path(os.environ.get('TD_VERIFY_OUT', str(PROJECT/'build/verification')))
GAME = Path(os.environ.get('TD_VERIFY_GAME', str(REPO/'dist/apps/td_lab/windows-x86_64-dist/TowerDefense.exe')))
EDITOR = Path(os.environ.get('TD_VERIFY_EDITOR', str(REPO/'dist/editor/windows-x86_64-dist/JCE_Editor.exe')))

def idle(n):
    return [((),0,0,0)]*n

def key(code):
    return [((code,),0,0,0)]*3+idle(5)

def click(x,y):
    return [((),x,y,0)]*3+[((),x,y,1)]*3+[((),x,y,0)]*5

def write_replay(path,frames):
    with path.open('wb') as file:
        file.write(b'JIRC'+struct.pack('<III',2,2336,0))
        for sample in frames:
            keys,x,y,buttons=sample[:4]
            dx,dy,wheel=sample[4:7] if len(sample)>4 else (0,0,0)
            frame = bytearray(2336)
            struct.pack_into('<II',frame,0,2,512)
            for code in keys:
                offset = 8+(code//64)*8
                old = struct.unpack_from('<Q',frame,offset)[0]
                struct.pack_into('<Q',frame,offset,old | (1 << (code%64)))
            struct.pack_into('<5fI',frame,520,x,y,dx,dy,wheel,buttons)
            touches=sample[7] if len(sample)>7 else []
            struct.pack_into('<I',frame,544,len(touches))
            for index,(ident,tx,ty,pressure) in enumerate(touches):
                struct.pack_into('<Q3f',frame,552+index*24,ident,tx,ty,pressure)
            file.write(frame)

def run(name, frames=None, extra=None, count=240, exe=None, cwd=None):
    exe = Path(exe) if exe is not None else GAME
    OUT.mkdir(parents=True,exist_ok=True)
    env = dict(os.environ)
    for name_to_clear in ('TD_RANDOM_TAPE','TD_SELF_TEST','TD_AUTOSTART','TD_AUTODEFENSE','TD_FIXTURE','TD_EXTENDED','TD_DIAG','TD_TIMING','TD_TIMING_START','TD_LEGACY_FRAME_MODE','TD_STRESS','JCE_NO_POSTFX','JCE_INPUT_REPLAY','JCE_CAPTURE_PATH','JCE_KPI_GAME_SHOTS','JCE_KPI_AUTOPLAY'):
        env.pop(name_to_clear,None)
    env.update(DETERMINISM)
    env.update(JCE_BACKEND='d3d11',JCE_MULTI_INSTANCE='1',JCE_MAX_FRAMES=str(count),
               JCE_CAPTURE_FRAME=str(count-10),JCE_CAPTURE_PATH=str(OUT/(name+'.png')),
               TD_TRACE='1',TD_SEED='7')
    if frames is not None:
        path = OUT/(name+'.jirc')
        write_replay(path,frames+idle(max(0,count-len(frames))))
        env['JCE_INPUT_REPLAY'] = str(path)
    if extra:
        env.update(extra)
    start = time.perf_counter()
    result = subprocess.run([str(exe)],cwd=cwd or exe.parent,env=env,capture_output=True,
                            text=True,encoding='utf-8',errors='replace',timeout=180)
    log = result.stdout+result.stderr
    identity = {'executable':str(exe.resolve()),'sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),
                'exit':result.returncode,'frames':count,'synthetic_replay':frames is not None}
    (OUT/(name+'-artifact.json')).write_text(json.dumps(identity,indent=2)+'\n',encoding='utf-8')
    (OUT/(name+'.log')).write_text(log,encoding='utf-8',newline='\n')
    print(name,'exit',result.returncode,'wall_seconds',round(time.perf_counter()-start,3),flush=True)
    for line in log.splitlines():
        if 'TD_TEST' in line or 'TD_BOOT' in line or 'action=destroy' in line or ' error:' in line:
            print(line,flush=True)
    if result.returncode:
        raise RuntimeError(f'{name} exited {result.returncode}; see {OUT/name}.log')
    assert_backend(log,'d3d11')
    failures = ('cgltf_validate failed','cannot load models/','error: Tower Defense',
                'scene pool is incomplete','actor prefab failed to spawn','disabled for this script instance')
    if any(marker in log for marker in failures):
        raise RuntimeError(f'Render/script failure; see {OUT/name}.log')
    if 'TD_SELF_TEST' not in (extra or {}) and 'TD_BOOT' not in log:
        raise RuntimeError('The shared scene controller did not start')
    return log

def editor(fixture=None):
    for index in range(2):
        profile=OUT/('editor-profile-'+str(index))
        profile.mkdir(parents=True,exist_ok=True)
        (profile/'editor-session.json').write_text(json.dumps({'last_project':str(PROJECT),
            'last_scene_path':str(PROJECT/'resources/scenes/td_lab.scene.json')}),encoding='utf-8')
        options={'JCE_EDITOR_PROFILE_DIR':str(profile),'JCE_KPI_AUTOPLAY':'1',
                'JCE_DBG_FOCUS_GAME':'1','JCE_KPI_GAME_CAPTURE_SIZE':'1280x720',
                'JCE_KPI_GAME_SHOTS':'4.00|'+str(OUT/f'editor-game-{index}.png')}
        options.update({'TD_FIXTURE':fixture} if fixture else {'TD_AUTODEFENSE':'1'})
        if fixture=='combat': options['TD_RANDOM_TAPE']=str(PROJECT/'build/reference/combat.random.txt')
        run('editor-play-'+str(index),extra=options,count=420,
                exe=EDITOR)
    print('EDITOR USED ISOLATED PROFILES',flush=True)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--editor',action='store_true')
    ap.add_argument('--quick',action='store_true')
    args = ap.parse_args()
    log = run('rules',extra={'TD_SELF_TEST':'1'},count=10)
    assert 'TD_TEST PASS' in log
    run('menu',idle(120),count=150)
    run('initial',extra={'TD_AUTOSTART':'1'},count=180)
    log = run('combat',extra={'TD_AUTODEFENSE':'1'},count=1100)
    assert re.search(r'kills=[1-9]',log) and re.search(r'hits=[1-9]',log)
    if not args.quick:
        workflow=idle(40)+click(595,125)+idle(20)+click(210,36)+idle(30)
        workflow+=click(640,360)+key(30)+key(8)+key(24)+key(26)+idle(80)
        log=run('original-input',workflow,count=len(workflow)+30)
        assert 'action=place menu=0' in log and 'resources=554' in log
        assert log.count('action=spawn')==2 and 'action=wave' in log
        assert 'wave=1 towers=1' in log
        log=run('key-release',idle(30)+[((8,),640,360,0)]*60+idle(20),
                extra={'TD_AUTOSTART':'1'},count=120)
        assert log.count('action=spawn')==1,'E must fire once on release'
        log=run('menu-return',workflow+click(640,36)+idle(15)+click(70,36)+idle(20),count=len(workflow)+80)
        assert 'action=menu menu=2' in log and 'action=menu menu=1' in log
        log=run('orbit-input',idle(30)+[((),640,360,1,20,-10,0)]*3+idle(5)
                +[((),640,360,4,5,3,0)]*3+idle(5)+[((),640,360,0,0,0,1)]
                +key(79),extra={'TD_AUTOSTART':'1'},count=100)
        assert 'action=camera' in log and 'targetX=0.000000 targetY=0.000000 targetZ=0.000000' not in log.split('action=destroy')[-1]
        relocated = OUT/'relocated'/'独立目录'
        relocated.mkdir(parents=True,exist_ok=True)
        assert not any(relocated.iterdir()), 'Relocation evidence needs an empty directory'
        shutil.copy2(GAME,relocated/GAME.name)
        log = run('relocated',extra={'TD_AUTODEFENSE':'1'},count=350,exe=relocated/GAME.name)
        assert 'TD_BOOT' in log
    if args.editor:
        editor()
    identity = {'exe':str(GAME),'sha256':hashlib.sha256(GAME.read_bytes()).hexdigest(),
                'reference_commit':'784129a685627a18f3d3f8fa594208000d9313e8',
                'renderer':'d3d11','fixed_dt':DETERMINISM['JCE_FRAME_DT_FIXED']}
    (OUT/'identity.json').write_text(json.dumps(identity,indent=2)+'\n',encoding='utf-8')
    print('APPLICATION CHECKS PASS; visual fidelity requires separate inspected comparisons')

if __name__ == '__main__':
    main()
