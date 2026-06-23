"""i18n_audit.py — Validate i18n key coverage across all locales.

Auto-discovers every ``editor/resources/assets/i18n/*.json`` file so a
new translation needs no script change: drop the JSON into the folder
and the audit picks it up.

Rules:
    * ``en.json`` is the source of truth — every key used in the editor
      C/C++ sources must exist in EN.
    * EVERY shipped locale (zh_cn, ko, ja, de, …) must be a 1:1 match of
      EN — full coverage. Missing keys FAIL (no silent EN-fallback drift);
      extra keys not present in EN FAIL as typo/orphan candidates. A new
      EN key is therefore a release gate until all locales carry it.
    * The reserved ``_meta.*`` namespace (e.g. ``_meta.nativeName``) is
      ignored by the coverage checks — it carries metadata, not strings
      used by jce_editor_i18n() lookups.
"""
import json, re, sys
from pathlib import Path

ROOT = Path('editor/src')
I18N_DIR = Path('editor/resources/assets/i18n')
META_PREFIX = '_meta.'

def is_meta(k: str) -> bool:
    return k.startswith(META_PREFIX)

# Collect keys actually used by the editor source.
keys = set()
pat = re.compile(r'jce_editor_i18n(?:_or|_id)?\(\s*"([^"]+)"')
for p in ROOT.rglob('*'):
    if p.suffix in ('.cpp','.h','.hpp','.c'):
        try: t = p.read_text(encoding='utf-8', errors='ignore')
        except: continue
        for m in pat.findall(t): keys.add(m)

# Discover all locale files automatically.
locales = {p.stem: json.load(open(p, encoding='utf-8'))
           for p in sorted(I18N_DIR.glob('*.json'))}
if 'en' not in locales:
    print('FAIL: en.json missing', file=sys.stderr)
    sys.exit(1)

en = {k: v for k, v in locales['en'].items() if not is_meta(k)}
en_keys = set(en.keys())

print('locales:', ', '.join(sorted(locales.keys())))
print('used:', len(keys), 'en:', len(en))

# EN must cover every used key.
miss_en = sorted(keys - en_keys)
print('missing EN:', len(miss_en))
for k in miss_en: print('  EN-MISS', k)

failed = bool(miss_en)

# zh_cn must match EN exactly (full coverage is a release requirement).
if 'zh_cn' in locales:
    zh = {k: v for k, v in locales['zh_cn'].items() if not is_meta(k)}
    zh_keys = set(zh.keys())
    miss_zh = sorted(keys - zh_keys)
    extra_en = sorted(en_keys - zh_keys)
    extra_zh = sorted(zh_keys - en_keys)
    print('missing ZH:', len(miss_zh))
    for k in miss_zh: print('  ZH-MISS', k)
    print('en-only (zh missing):', len(extra_en))
    for k in extra_en[:80]: print('  ZH-NEED', k)
    if len(extra_en) > 80: print(' ...', len(extra_en) - 80, 'more')
    print('zh-only (en missing):', len(extra_zh))
    for k in extra_zh[:80]: print('  EN-NEED', k)
    if len(extra_zh) > 80: print(' ...', len(extra_zh) - 80, 'more')
    if miss_zh or extra_en or extra_zh: failed = True

# Every shipped locale must be a 1:1 match of EN: missing keys FAIL
# (no silent EN-fallback drift), orphan/typo keys FAIL.
for code, table in sorted(locales.items()):
    if code in ('en', 'zh_cn'): continue
    data = {k: v for k, v in table.items() if not is_meta(k)}
    data_keys = set(data.keys())
    orphans = sorted(data_keys - en_keys)
    missing = sorted(en_keys - data_keys)
    covered = len(data_keys & en_keys)
    print(f'{code}-extra (typo/orphan, FAIL):', len(orphans))
    for k in orphans: print(f'  {code.upper()}-ORPHAN', k)
    print(f'{code}-missing (FAIL):', len(missing))
    for k in missing[:80]: print(f'  {code.upper()}-MISS', k)
    if len(missing) > 80: print(' ...', len(missing) - 80, 'more')
    pct = 100.0 * covered / max(1, len(en))
    print(f'{code} coverage: {covered}/{len(en)} ({pct:.1f}%)')
    if orphans or missing: failed = True

sys.exit(1 if failed else 0)
