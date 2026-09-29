"""Exercise actual JCE CLI requests over four loopback protocols."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from ai_director import validate
PROJECT=Path(__file__).resolve().parents[1]
REPO=PROJECT.parents[1]
PROVIDERS={'openai':'/v1','anthropic':'','gemini':'/v1beta','ollama':''}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image',type=Path)
    ap.add_argument('--base',default='http://127.0.0.1:11435')
    ap.add_argument('--model',default='qwen3.5:0.8b')
    args=ap.parse_args()
    out=PROJECT/'build'/'ai';out.mkdir(parents=True,exist_ok=True)
    good=dict(width=12,height=12,mines=18,red=24,green=45,blue=64)
    assert validate(good)==good
    invalid=[dict(good,width=True),dict(good,mines=144),dict(good,red=256),dict(good,width=12.5),dict(good,code='print(1)'),{},[]]
    for value in invalid:
        try:validate(value)
        except ValueError:pass
        else:raise AssertionError('Invalid proposal accepted: '+repr(value))
    rows=[]
    env=dict(os.environ,JCE_LLM_API_KEY='local')
    env['NO_PROXY']=','.join(filter(None,[env.get('NO_PROXY',env.get('no_proxy','')),'127.0.0.1,localhost,::1']))
    for provider,suffix in PROVIDERS.items():
        flags=['--provider',provider,'--base-url',args.base+suffix,'--model',args.model,'--max-tokens','256','--send']
        jobs=[('text',[str(REPO/'tools/llm/jce_llm.py'),*flags,'--user','What is 2 + 2? Reply with the number only.'])]
        if args.image:jobs.append(('vision',[str(REPO/'tools/llm/jce_llm.py'),*flags,'--image',str(args.image),'--user','Describe the two shapes and their colours from left to right. One short sentence.']))
        proposal=out/(provider+'-proposal.json')
        jobs.append(('parameters',[str(PROJECT/'tools/ai_director.py'),*flags,'--brief-file',str(PROJECT/'ai/director-brief.txt'),'--out',str(proposal)]))
        for kind,argv in jobs:
            started=time.monotonic()
            try:
                run=subprocess.run([sys.executable,'-B',*argv],env=env,cwd=REPO,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=140)
                text=run.stdout.strip();ok=run.returncode==0 and bool(text)
                if kind=='text':ok=ok and text.strip('.!')=='4'
                if kind=='vision':ok=ok and all(word in text.lower() for word in ('red','blue','square','circle'))
                if kind=='parameters':ok=ok and validate(json.loads(proposal.read_text()))==good
                row=dict(provider=provider,kind=kind,passed=ok,exit=run.returncode,answer=text,error=run.stderr.strip(),seconds=round(time.monotonic()-started,2))
            except (subprocess.TimeoutExpired,ValueError,OSError) as exc:row=dict(provider=provider,kind=kind,passed=False,error=str(exc))
            rows.append(row);print(provider,kind,'PASS' if row['passed'] else 'FAIL',flush=True)
            (out/'protocol-results.json').write_text(json.dumps({'model':args.model,'backend':'one local Ollama model, four adapter protocols; no cloud-provider models','validator_negative_cases':len(invalid),'results':rows},indent=2),encoding='utf-8')
    return 0 if all(r['passed'] for r in rows) else 1
if __name__=='__main__':raise SystemExit(main())
