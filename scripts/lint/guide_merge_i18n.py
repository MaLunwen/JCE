#!/usr/bin/env python3
"""Merge staged user-guide i18n fragments into the shipped locale files.

Reads every <name>.i18n.json in the staging dir and inserts all four
locale blocks into editor/resources/assets/i18n/<locale>.json.  Existing
keys are overwritten (re-runs are idempotent).  Run guide_validate.py
first; run tools/sort_i18n.py after.

Usage: python scripts/lint/guide_merge_i18n.py [staging_dir]
"""
import collections
import io
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STAGING = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "_guide_staging")
I18N = os.path.join(ROOT, "editor", "resources", "assets", "i18n")
LOCALES = ["zh_cn", "en", "ja", "ko"]


def main():
    merged = {loc: {} for loc in LOCALES}
    frags = sorted(f for f in os.listdir(STAGING) if f.endswith(".i18n.json"))
    if not frags:
        print("no fragments in", STAGING)
        return 1
    for fn in frags:
        with io.open(os.path.join(STAGING, fn), encoding="utf-8") as f:
            frag = json.load(f)
        for loc in LOCALES:
            merged[loc].update(frag.get(loc, {}))

    for loc in LOCALES:
        path = os.path.join(I18N, loc + ".json")
        with io.open(path, encoding="utf-8") as f:
            data = json.load(f, object_pairs_hook=collections.OrderedDict)
        before = len(data)
        data.update(merged[loc])
        with io.open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, ensure_ascii=False, indent=4)
            f.write("\n")
        print(f"{loc}: {before} -> {len(data)} (+{len(data) - before})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
