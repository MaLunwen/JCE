"""Launch the example with a local provider; optional AI never blocks gameplay."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
PROJECT=Path(__file__).resolve().parents[1]
REPO=PROJECT.parents[1]
BASES={'openai':'http://127.0.0.1:11435/v1','anthropic':'http://127.0.0.1:11435',
       'gemini':'http://127.0.0.1:11435/v1beta','ollama':'http://127.0.0.1:11435'}
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--provider',choices=BASES,default='ollama')
ap.add_argument('--model',default='qwen3.5:0.8b')
ap.add_argument('--base-url',help='Override the selected protocol endpoint')
a=ap.parse_args()
env=dict(os.environ)
env['NO_PROXY']=','.join(filter(None,[env.get('NO_PROXY',env.get('no_proxy','')),'127.0.0.1,localhost,::1']))
env.update(MINES_AI_PYTHON=sys.executable,MINES_AI_TOOL=str(PROJECT/'tools/ai_director.py'),
           JCE_LLM_PROVIDER=a.provider,JCE_LLM_BASE_URL=a.base_url or BASES[a.provider],
           JCE_LLM_MODEL=a.model,JCE_LLM_MAX_TOKENS='512')
if a.provider!='ollama':env.setdefault('JCE_LLM_API_KEY','local')
exe=REPO/'dist/games/Minesweeper-release-x86_64/Minesweeper.exe'
if not exe.is_file():raise SystemExit('Build this project through scripts/jce.py first')
raise SystemExit(subprocess.call([str(exe)],cwd=exe.parent,env=env))
