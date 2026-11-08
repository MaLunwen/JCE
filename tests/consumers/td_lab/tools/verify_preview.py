"""Consecutive-frame witnesses for a switching build card and first basic tower."""
import json
import numpy as np
from PIL import Image
import verify_game as v

def pixels(name):return np.asarray(Image.open(v.OUT/(name+'.png')).convert('RGB')).astype(int)
def main():
    report={'producer':'Same native input recording replayed to each explicit capture frame; no expected image injection.'}
    selection=v.idle(30)+v.click(640,360)+v.idle(30)+v.click(670,365)+v.idle(50)
    frames=[70,78,79,80,81,82,83,85]
    card=[]
    for n in frames:
        name='card-switch-'+str(n);log=v.run(name,selection,extra={'TD_FIXTURE':'initial'},count=n+10)
        if n>=79:assert log.count('action=select')>=2
        card.append(pixels(name)[626:701,500:780])
    delta=[float(np.abs(a-card[0]).mean()) for a in card[1:]]
    assert max(delta)<.05,delta
    report['card_switch']={'frames':frames,'card_roi':[500,626,280,75],'mean_rgb_deltas':delta,'pass':True}
    placing=v.idle(30)+v.click(640,360)+v.idle(30)+v.key(30)+v.idle(50)
    frames=[76,77,78,79,80,81,82,88,100];tower=[];captures=[]
    for n in frames:
        name='basic-first-'+str(n);log=v.run(name,placing,extra={'TD_FIXTURE':'initial'},count=n+10)
        assert 'TD_TOWER cell=60 kind=0' in log
        capture=pixels(name);captures.append(capture);tower.append(capture[280:405,585:695])
    settled=tower[-1];bare=pixels('fixture-initial')[280:405,585:695]
    states=[];seen=False
    for n,image,roi in zip(frames,captures,tower):
        settled_error=float(np.abs(roi-settled).mean());bare_error=float(np.abs(roi-bare).mean())
        funded=np.array_equal(image[20:61,1188:1263],captures[-1][20:61,1188:1263])
        state='before-purchase' if not funded else 'complete' if settled_error<.1 else 'pending' if bare_error<.1 else 'partial'
        states.append({'capture_frame':n,'state':state,'settled_mae':settled_error,'bare_mae':bare_error})
        if funded:assert state!='partial',states
        if seen:assert state=='complete',states
        seen |= state=='complete'
    assert seen
    report['basic_first_frames']={'frames':frames,'tower_roi':[585,280,110,125],'states':states,'pass':True,'policy':'Pending assets may stay absent. First appearance and every later frame must have complete material; no cube or partial bindings.'}
    (v.OUT/'preview-sequence.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2),flush=True)
if __name__=='__main__':main()
