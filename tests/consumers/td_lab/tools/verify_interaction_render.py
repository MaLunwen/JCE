"""Actual camera images and normal Editor event routing; synthetic input is labelled."""
import json, re
import numpy as np
from PIL import Image
import verify_game as v
from compare_captures import compare

def main():
    report={'input_producer':'Native JIRC; Editor normalized synthetic events through normal event processor. Physical mouse not certified.'}
    drag=v.idle(30)+[((),640,360,1),((),700,330,1,60,-30,0),((),700,330,0)]+v.idle(30)
    log=v.run('rendered-orbit',drag,extra={'TD_FIXTURE':'initial'},count=100)
    r=compare(v.PROJECT/'build/reference/captures/control-touch-rotate.jpg',v.OUT/'rendered-orbit.png',v.OUT/'rendered-orbit-reference',{'board':[190,150,900,520]},[[0,0,500,45],[0,680,600,40]])
    a=np.asarray(Image.open(v.OUT/'fixture-initial.png').convert('RGB')).astype(int)
    b=np.asarray(Image.open(v.OUT/'rendered-orbit.png').convert('RGB')).astype(int)
    delta=float(np.abs(a[150:670,190:1090]-b[150:670,190:1090]).mean());assert delta>1, 'Runtime camera changed only its variables, not its image'
    assert r['regions']['board']['luma_ssim']>.9,r
    report['rendered_orbit']={'vs_initial_board_mae':delta,'vs_original':r['regions'],'pass':True}
    profile=v.OUT/'editor-event-profile';profile.mkdir(exist_ok=True)
    (profile/'editor-session.json').write_text(json.dumps({'last_project':str(v.PROJECT),'last_scene_path':str(v.PROJECT/'resources/scenes/td_lab.scene.json')}),encoding='utf-8')
    rows=[(30,'M',640,360,0,0),(40,'B',640,360,1,1),(43,'B',640,360,1,0),
          (100,'M',535,660,0,0),(105,'B',535,660,1,1),(108,'B',535,660,1,0),
          (150,'M',640,360,0,0),(155,'B',640,360,1,1),(158,'M',700,330,60,-30),(161,'B',700,330,1,0)]
    events=v.OUT/'editor-game-events.txt'
    events.write_text(''.join(f'{f} {k} {x/1280:.9f} {y/720:.9f} {a/1280 if k=="M" else a:.9f} {b/720 if k=="M" else b:.9f}\n' for f,k,x,y,a,b in rows),encoding='utf-8')
    log=v.run('editor-events',extra={'TD_FIXTURE':'initial','JCE_KPI_GAME_EVENTS':str(events),'JCE_EDITOR_PROFILE_DIR':str(profile),'JCE_KPI_AUTOPLAY':'1','JCE_DBG_FOCUS_GAME':'1','JCE_KPI_GAME_CAPTURE_SIZE':'1280x720','JCE_KPI_GAME_SHOTS':'4.00|'+str(v.OUT/'editor-events-game.png')},count=600,exe=v.EDITOR)
    assert 'Game event fixture frame=40 type=B owner=1 hovered=1' in log
    assert 'action=select menu=0 selected=60' in log
    assert 'action=place menu=0' in log and 'resources=554' in log
    assert 'towers=1' in log and 'action=camera' in log
    report['editor_events']={'selection':60,'tower_kind':0,'resource_cost':1,'orbit':True,'render_size':[1280,720],'pass':True}
    sequence=v.idle(30)+v.click(640,360)+v.idle(35)+v.click(535,660)+v.idle(12)
    log=v.run('player-events',sequence+drag[30:],extra={'TD_FIXTURE':'initial'},count=220)
    assert 'TD_TOWER cell=60 kind=0' in log
    r=compare(v.OUT/'player-events.png',v.OUT/'editor-events-game.png',v.OUT/'editor-events-parity',{'board':[190,150,900,520]})
    assert r['regions']['full']['luma_ssim']>.99,r
    report['editor_render_parity']=r['regions']
    (v.OUT/'interaction-render.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2),flush=True)
if __name__=='__main__':main()
