"""check_i18n_dup_values.py — Catch translated-value collisions between sibling
i18n keys.

ImGui derives a widget's ID from its visible label, so two menu items / widgets
that share the same label collide (ImGui asserts / mis-routes input).  This bit
us in Korean: ``menu.file.new`` and ``menu.file.newScene`` were both translated
"새 장면", so the File menu had two identical entries.

Rule (low false-positive by design): for every pair of keys that share the same
immediate parent namespace (everything before the last ``.``) and whose **EN**
values DIFFER — i.e. they are meant to be distinct items — FAIL if any locale
collapses them to the **same** value.  Pairs that are intentionally identical in
EN are ignored (they are not a translation regression), and keys in different
namespaces are never compared (so reused words like "UI"/"On" across unrelated
menus don't trip it).

Auto-discovers ``editor/resources/assets/i18n/*.json``; no per-locale config.
"""
import json
import sys
from collections import defaultdict
from pathlib import Path

I18N_DIR = Path('editor/resources/assets/i18n')
META_PREFIX = '_meta.'

# Ratchet baseline: sibling pairs that already collapse to one value in some
# locale but are NOT true ImGui-label collisions (warning sentences, status
# values, or columns that live in separate tables).  New collisions outside
# this set FAIL — that is the regression guard.  Review + shrink this list as
# the flagged pairs are given distinct translations.
ALLOWLIST = {
    frozenset(('assetBrowser.deleteFolderWarningMulti',
               'assetBrowser.deleteFolderWarningSingle')),
    frozenset(('panel.build_report.col.bundle',
               'panel.build_report.col.dep_bundles')),
    frozenset(('panel.build_report.col.bundle',
               'panel.build_report.col.dup_bundles')),
    frozenset(('panel.build_report.col.dep_bundles',
               'panel.build_report.col.name')),
    frozenset(('panel.build_report.col.dup_bundles',
               'panel.build_report.col.name')),
    frozenset(('profiler.row.cullRatio', 'profiler.row.ocCullRate')),
    frozenset(('profiler.value.ocOff', 'profiler.value.wsInactive')),
    frozenset(('viewer.actions', 'viewer.controls')),
    frozenset(('viewer.materialSettings', 'viewer.materials')),
    frozenset(('viewer.meshes', 'viewer.showGrid')),
}


def parent(key: str) -> str:
    return key.rsplit('.', 1)[0] if '.' in key else ''


def main() -> int:
    locales = {p.stem: json.load(open(p, encoding='utf-8'))
               for p in sorted(I18N_DIR.glob('*.json'))}
    if 'en' not in locales:
        print('check_i18n_dup_values: FAIL — en.json missing', file=sys.stderr)
        return 1
    en = locales['en']

    # Group EN keys (the source of truth for "should differ") by parent ns.
    groups: dict[str, list[str]] = defaultdict(list)
    for k in en:
        if k.startswith(META_PREFIX):
            continue
        groups[parent(k)].append(k)

    violations = []
    for ns, keys in groups.items():
        for i in range(len(keys)):
            for j in range(i + 1, len(keys)):
                a, b = keys[i], keys[j]
                if en.get(a) == en.get(b):
                    continue  # intentionally identical in EN — not a regression
                if frozenset((a, b)) in ALLOWLIST:
                    continue  # reviewed pre-existing pair (see ALLOWLIST note)
                for loc, table in locales.items():
                    if loc == 'en':
                        continue
                    va, vb = table.get(a), table.get(b)
                    if va is not None and va == vb:
                        violations.append((loc, ns, a, b, va))

    if violations:
        print('i18n duplicate-value check: FAILED — keys that differ in EN '
              'collapse to one value in a locale (ImGui ID-collision risk)')
        for loc, ns, a, b, v in violations:
            print(f'  [{loc}] under "{ns or "(root)"}": {a} == {b} == "{v}"')
        print(f'{len(violations)} collision(s). Give them distinct '
              'translations, or disambiguate the widget with jce_editor_i18n_id().')
        return 1

    print('i18n duplicate-value check: OK (no sibling value collisions)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
