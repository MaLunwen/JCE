"""Capture actual D3D11 runtime/editor output against saved original browser evidence.
No alignment, recolouring, or expected state injection is performed. Random tape
only records original gameplay random inputs; UUID allocation draws are excluded.
"""
import json
import shutil
import sys
import numpy as np
from PIL import Image
import verify_game as v
sys.path.insert(0,str(v.REPO/'tools'))
from compare_captures import compare

REGIONS={'board':[190,150,900,520],'hud':[1180,10,100,225],'actors':[230,395,290,215]}
EXCLUDES=[[0,688,320,32]] # Clearly visible browser test-probe controls only.

def main():
    suite={}
    for name in ['initial','select','basic','advanced','rocket','ghost','ufo','combat','low-money']:
        extra={'TD_FIXTURE':name}
        if name=='combat':extra['TD_RANDOM_TAPE']=str(v.PROJECT/'build/reference/combat.random.txt')
        v.run('fixture-'+name,extra=extra,count=900)
        suite[name]=compare(v.PROJECT/'build/reference/captures'/(name+'.jpg'),v.OUT/('fixture-'+name+'.png'),v.OUT/('compare-'+name),REGIONS,EXCLUDES)
        print(name,suite[name]['regions']['full'],flush=True)
    v.run('menu',v.idle(120),count=150)
    v.run('credits',v.idle(40)+v.click(675,125)+v.idle(80),count=180)
    cross=np.asarray(Image.open(v.OUT/'credits.png').convert('RGB')).astype(int)[45:90,909:935]
    assert int(((cross[:,:,2]>cross[:,:,0]+30)&(cross[:,:,1]>cross[:,:,0]+20)).sum())>5, 'Credits close cross is covered by another UI sibling'
    for name,candidate in [('menu-final','menu'),('credits-final','credits')]:
        suite[name]=compare(v.PROJECT/'build/reference/captures'/(name+'.jpg'),v.OUT/(candidate+'.png'),v.OUT/('compare-'+name),{'menu':[340,20,600,560]},EXCLUDES)
        print(name,suite[name]['regions']['full'],flush=True)
    v.run('no-effects-direct',extra={'TD_FIXTURE':'initial','JCE_NO_POSTFX':'1'},count=900)
    a=np.asarray(Image.open(v.OUT/'fixture-initial.png').convert('RGB')).astype(int)
    b=np.asarray(Image.open(v.OUT/'no-effects-direct.png').convert('RGB')).astype(int)
    difference=int(np.abs(a-b).max());mean=float(np.abs(a-b).mean());print('EMPTY FX MAX/MEAN RGB',difference,mean,flush=True);assert mean<.25
    suite['empty_postfx']={'max_rgb_difference':difference,'mean_rgb_difference':mean,'pass':mean<.25,'threshold_mean_rgb':.25,'note':'Residual edge rasterization differences are measured, not pixel identity.'}
    parity={}
    for name in ['initial','combat']:
        # The frozen fixture reaches the same simulated frame in both hosts.
        v.editor(name)
        for i in range(2):
            capture=v.OUT/f'editor-{name}-{i}.png';shutil.copyfile(v.OUT/f'editor-game-{i}.png',capture)
            r=compare(v.OUT/('fixture-'+name+'.png'),capture,v.OUT/f'editor-parity-{name}-{i}',REGIONS)
            parity[name+str(i)]=r
            print('EDITOR PARITY',name,i,r['regions']['full'],flush=True)
    suite['editor_parity']=parity
    (v.OUT/'visual-suite.json').write_text(json.dumps(suite,indent=2)+'\n',encoding='utf-8')
    print('Actual captures and comparisons written; measured differences remain explicit.')
if __name__=='__main__':main()
