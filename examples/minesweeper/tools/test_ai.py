"""Offline regression checks for Gemini wire names and bounded proposals."""
import sys
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[3]/'tools/llm'))
import jce_llm
from ai_director import validate
cfg=jce_llm.Config(provider='gemini',base_url='http://127.0.0.1:11435/v1beta',model='local',api_key='sentinel')
url,headers,body=jce_llm.build_request(cfg,'system','user',images=[('image/png','AA==')])
assert body['systemInstruction']['parts'][0]['text']=='system'
assert body['contents'][0]['parts'][1]['inlineData']['mimeType']=='image/png'
assert 'system_instruction' not in body
valid=dict(width=12,height=12,mines=18,red=24,green=45,blue=64)
assert validate(valid)==valid
for value in [dict(valid,width=True),dict(valid,mines=144),dict(valid,red=256),dict(valid,width=12.5),dict(valid,code='print(1)'),{},[]]:
    try:validate(value)
    except ValueError:pass
    else:raise AssertionError(value)
print('AI REGRESSION PASS: canonical Gemini fields; seven invalid proposals rejected')
