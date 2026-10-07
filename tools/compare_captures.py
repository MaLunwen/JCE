"""Compare real, equally sized captures without alignment or colour correction.

Explicit caller-supplied ROIs and exclusion rectangles keep evidence reviewable.
Outputs RGB MAE, luminance SSIM, foreground IoU and unmodified side-by-side images.
"""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from scipy.ndimage import gaussian_filter

def compare(reference, candidate, output, regions=None, excludes=None):
    ref=Image.open(reference).convert('RGB'); test=Image.open(candidate).convert('RGB')
    if ref.size!=test.size: raise ValueError(f'Capture sizes differ: {ref.size} != {test.size}')
    a=np.asarray(ref,dtype=np.float64); b=np.asarray(test,dtype=np.float64)
    height,width=a.shape[:2]; mask=np.ones((height,width),dtype=bool)
    for x,y,w,h in excludes or []: mask[y:y+h,x:x+w]=False
    weights=np.array([.2126,.7152,.0722]); la=a@weights; lb=b@weights
    ma=gaussian_filter(la,1.5); mb=gaussian_filter(lb,1.5)
    va=gaussian_filter(la*la,1.5)-ma*ma; vb=gaussian_filter(lb*lb,1.5)-mb*mb
    cov=gaussian_filter(la*lb,1.5)-ma*mb
    ssim=((2*ma*mb+6.5025)*(2*cov+58.5225))/((ma*ma+mb*mb+6.5025)*(va+vb+58.5225))
    err=np.abs(a-b); foreground_a=a.max(axis=2)>20; foreground_b=b.max(axis=2)>20
    report={'reference':str(Path(reference).resolve()),'candidate':str(Path(candidate).resolve()),
            'reference_sha256':hashlib.sha256(Path(reference).read_bytes()).hexdigest(),
            'candidate_sha256':hashlib.sha256(Path(candidate).read_bytes()).hexdigest(),
            'size':list(ref.size),'excludes':excludes or [],'regions':{}}
    for name,box in {'full':[0,0,width,height],**(regions or {})}.items():
        x,y,w,h=box; m=mask[y:y+h,x:x+w]
        if not m.any(): raise ValueError('Empty comparison region: '+name)
        ea=err[y:y+h,x:x+w][m]; fa=foreground_a[y:y+h,x:x+w][m]; fb=foreground_b[y:y+h,x:x+w][m]
        union=np.count_nonzero(fa|fb)
        report['regions'][name]={'rect':box,'pixels':int(m.sum()),'rgb_mae':float(ea.mean()),
            'rgb_p95':float(np.quantile(ea,.95)),'luma_ssim':float(ssim[y:y+h,x:x+w][m].mean()),
            'foreground_iou':float(np.count_nonzero(fa&fb)/union) if union else 1.0}
    output=Path(output); output.parent.mkdir(parents=True,exist_ok=True)
    panel=Image.new('RGB',(width*3,height+30),(30,30,30)); draw=ImageDraw.Draw(panel)
    for i,(im,label) in enumerate([(ref,'Reference'),(test,'JCE'),(Image.fromarray(np.clip(err*4,0,255).astype('uint8')),'Absolute RGB error x4')]):
        panel.paste(im,(i*width,30)); draw.text((i*width+8,8),label,fill='white')
    panel.save(output.with_suffix('.png'))
    output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    return report

def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('reference',type=Path); p.add_argument('candidate',type=Path)
    p.add_argument('--out',type=Path,required=True); p.add_argument('--regions',type=Path)
    a=p.parse_args(); cfg=json.loads(a.regions.read_text()) if a.regions else {}
    r=compare(a.reference,a.candidate,a.out,cfg.get('regions'),cfg.get('excludes'))
    print(json.dumps(r['regions'],indent=2))
if __name__=='__main__': main()
