#!/usr/bin/env python3
"""Validate staged user-guide chapter TUs against their i18n fragments.

Checks, per chapter pair (jce_guide_ch_<name>.cpp + <name>.i18n.json):
  1. every guide.* key referenced by the .cpp exists in ALL four locale
     blocks of the fragment (keys outside the chapter prefix must already
     exist in the shipped i18n files);
  2. the four locale blocks carry IDENTICAL key sets;
  3. every JCE_HK_* / JCE_PANEL_* identifier exists in the real headers;
  4. every MENU_PATH segment and OPEN_PANEL title key exists in the
     shipped zh_cn.json;
  5. extern symbol name matches g_jce_guide_ch_<name>;
  6. no key collisions between chapters.

Usage: python scripts/lint/guide_validate.py [staging_dir]
Exit 0 = clean.
"""
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STAGING = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "_guide_staging")
I18N = os.path.join(ROOT, "editor", "resources", "assets", "i18n")
LOCALES = ["zh_cn", "en", "ja", "ko"]


def read(path):
    with io.open(path, encoding="utf-8") as f:
        return f.read()


def main():
    errors = []
    warnings = []

    hotkeys = set(re.findall(r"\bJCE_HK_[A-Z0-9_]+\b",
                             read(os.path.join(ROOT, "editor/src/core/jce_hotkeys.h"))))
    panels = set(re.findall(r"\bJCE_PANEL_[A-Z0-9_]+\b",
                            read(os.path.join(ROOT, "editor/src/ui/jce_editor_panels.h"))))
    shipped = json.loads(read(os.path.join(I18N, "zh_cn.json")))

    cpps = sorted(f for f in os.listdir(STAGING)
                  if f.startswith("jce_guide_ch_") and f.endswith(".cpp"))
    if not cpps:
        print("no staged chapters found in", STAGING)
        return 1

    seen_keys = {}
    for cpp_name in cpps:
        name = cpp_name[len("jce_guide_ch_"):-len(".cpp")]
        cpp = read(os.path.join(STAGING, cpp_name))
        json_path = os.path.join(STAGING, name + ".i18n.json")
        if not os.path.exists(json_path):
            errors.append(f"{name}: missing {name}.i18n.json")
            continue
        frag = json.loads(read(json_path))

        for loc in LOCALES:
            if loc not in frag:
                errors.append(f"{name}: locale block '{loc}' missing")
        locsets = {loc: set(frag.get(loc, {})) for loc in LOCALES}
        base = locsets["zh_cn"]
        for loc in LOCALES[1:]:
            if locsets[loc] != base:
                diff = locsets[loc] ^ base
                errors.append(f"{name}: key set mismatch zh_cn vs {loc}: {sorted(diff)[:6]}")

        sym = f"g_jce_guide_ch_{name}"
        if sym not in cpp:
            errors.append(f"{name}: extern symbol {sym} not found in {cpp_name}")

        for hk in set(re.findall(r"\bJCE_HK_[A-Z0-9_]+\b", cpp)):
            if hk not in hotkeys:
                errors.append(f"{name}: unknown hotkey id {hk}")
        for pn in set(re.findall(r"\bJCE_PANEL_[A-Z0-9_]+\b", cpp)):
            if pn not in panels:
                errors.append(f"{name}: unknown panel id {pn}")

        # String literals referenced by the table code.
        lits = set(re.findall(r'"((?:[^"\\]|\\.)*)"', cpp))
        guide_keys = {s for s in lits if s.startswith("guide.")}
        for k in guide_keys:
            if k not in base:
                errors.append(f"{name}: cpp references {k} but fragment lacks it")
        for k in base:
            if k not in guide_keys and not k.endswith(".title"):
                warnings.append(f"{name}: fragment key {k} unused by cpp")
            if k in seen_keys:
                errors.append(f"{name}: key {k} collides with chapter {seen_keys[k]}")
            seen_keys[k] = name

        # MENU_PATH segments + OPEN_PANEL title keys must be shipped keys.
        for m in re.finditer(r"JCE_GB_MENU_PATH\s*,[^,]*,[^,]*,\s*\"([^\"]+)\"", cpp):
            for seg in m.group(1).split(">"):
                if seg not in shipped:
                    errors.append(f"{name}: MENU_PATH segment '{seg}' not in shipped zh_cn.json")
        for m in re.finditer(r"JCE_GB_OPEN_PANEL\s*,\s*\"([^\"]+)\"", cpp):
            if m.group(1) not in shipped:
                errors.append(f"{name}: OPEN_PANEL title key '{m.group(1)}' not in shipped zh_cn.json")

    for w in warnings:
        print("[warn]", w)
    for e in errors:
        print("[FAIL]", e)
    print(f"chapters={len(cpps)} errors={len(errors)} warnings={len(warnings)}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
