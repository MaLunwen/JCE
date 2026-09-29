"""Project-owned bounded director. HTTP remains exclusively in jce_llm."""
import argparse
import json
import os
from pathlib import Path
import sys
import tempfile
REPO=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(REPO/'tools'/'llm'))
import jce_llm

FIELDS=('width','height','mines','red','green','blue')
SYSTEM=('Return one JSON object, no markdown, with exactly six integer keys: '
        'width (5..30), height (5..16), mines (1..width*height-9), '
        'red, green, blue (each 0..255). These are custom-board and background '
        'parameters for a classic Minesweeper scene. No code, URLs or asset paths.')

def validate(value):
    if not isinstance(value,dict) or set(value)!=set(FIELDS):
        raise ValueError('Expected exactly: '+', '.join(FIELDS))
    if any(type(value[k]) is not int for k in FIELDS):
        raise ValueError('Every field must be an integer (not a boolean)')
    w,h,m=(value[k] for k in FIELDS[:3])
    if not (5<=w<=30 and 5<=h<=16 and 1<=m<=w*h-9):
        raise ValueError('Board bounds or first-click safe-area capacity violated')
    if any(not 0<=value[k]<=255 for k in FIELDS[3:]):
        raise ValueError('RGB outside 0..255')
    return value

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    jce_llm.add_cli_args(ap)
    ap.add_argument('--brief-file',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    cfg=jce_llm.config_from_args(args)
    brief=args.brief_file.read_text(encoding='utf-8')
    if len(brief)>12000: raise ValueError('Brief exceeds 12000 characters')
    if not args.send:
        print(jce_llm.describe(cfg,SYSTEM,brief));return 0
    reply=jce_llm.send(cfg,SYSTEM,brief,timeout=80)
    data=validate(json.loads(reply))
    args.out.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.NamedTemporaryFile('w',encoding='utf-8',dir=args.out.parent,delete=False) as f:
        json.dump(data,f);f.write('\n');name=f.name
    try: os.replace(name,args.out)
    finally:
        if os.path.exists(name):os.unlink(name)
    print('Validated AI proposal from %s / %s'%(cfg.provider,cfg.model))
    return 0
if __name__=='__main__':
    try: raise SystemExit(main())
    except (ValueError,OSError,jce_llm.JceLlmError) as exc:
        print('ai_director: '+str(exc),file=sys.stderr);raise SystemExit(1)
