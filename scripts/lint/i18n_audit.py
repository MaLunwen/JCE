import json, re, sys
from pathlib import Path
root = Path('editor/src')
keys = set()
pat = re.compile(r'jce_editor_i18n(?:_or|_id)?\(\s*"([^"]+)"')
for p in root.rglob('*'):
    if p.suffix in ('.cpp','.h','.hpp','.c'):
        try: t = p.read_text(encoding='utf-8', errors='ignore')
        except: continue
        for m in pat.findall(t): keys.add(m)
en = json.load(open('editor/resources/assets/i18n/en.json', encoding='utf-8'))
zh = json.load(open('editor/resources/assets/i18n/zh_cn.json', encoding='utf-8'))
print('used:', len(keys), 'en:', len(en), 'zh:', len(zh))
miss_en = sorted(keys - set(en.keys()))
miss_zh = sorted(keys - set(zh.keys()))
extra_en = sorted(set(en.keys()) - set(zh.keys()))
extra_zh = sorted(set(zh.keys()) - set(en.keys()))
print('missing EN:', len(miss_en))
for k in miss_en: print('  EN-MISS', k)
print('missing ZH:', len(miss_zh))
for k in miss_zh: print('  ZH-MISS', k)
print('en-only (zh missing):', len(extra_en))
for k in extra_en[:80]: print('  ZH-NEED', k)
if len(extra_en)>80: print(' ...', len(extra_en)-80, 'more')
print('zh-only (en missing):', len(extra_zh))
for k in extra_zh[:80]: print('  EN-NEED', k)
if len(extra_zh)>80: print(' ...', len(extra_zh)-80, 'more')
