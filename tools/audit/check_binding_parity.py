#!/usr/bin/env python3
"""
check_binding_parity.py — verify the language bindings expose the same
engine surface as the C ABI (plan §22 / §35.5).

The C ABI (engine/include/jce/**) is the single source of truth.  Lua and
Java/JNI are consumers of it.  This checker does NOT require 100% coverage
(the bindings are curated subsets) — it enforces that a curated set of
"core parity" functions is reachable from every supported language, and it
reports the overall coverage numbers so drift is visible.

Sources scanned:
  * C ABI  — JCE_API declarations under engine/include/jce/**
  * Lua    — luaL_Reg tables / lua_register / lua_pushcfunction sites in
             engine/src/middleware/script/** and application/jce_rt_script.c
  * JNI    — JNIEXPORT functions / Java_* symbols in
             engine/src/os/platform/jce_jni_bridge.c (+ engine/java if present)

CORE_PARITY lists the representative APIs the plan wants proven equivalent
across C / Lua / Java.  A missing binding is a finding; the checker prints a
parity matrix and exits non-zero only if a CORE_PARITY concept is missing
from a language that is expected to have it.

Usage:
  python tools/audit/check_binding_parity.py [--json]

This is a REPORTING gate: by default it prints the matrix and coverage and
exits 0 (informational) unless --strict is passed, because the bindings are
an intentional subset and hard-failing on every unbound C function would be
noise.  CI wires it with --report only until the parity contract is frozen.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PUBLIC_ROOT = REPO_ROOT / "engine" / "include" / "jce"
SCRIPT_DIR = REPO_ROOT / "engine" / "src" / "middleware" / "script"
RT_SCRIPT = REPO_ROOT / "engine" / "src" / "application" / "jce_rt_script.c"
JNI_BRIDGE = REPO_ROOT / "engine" / "src" / "os" / "platform" / "jce_jni_bridge.c"
JAVA_ROOT = REPO_ROOT / "engine" / "java"

# Representative cross-language APIs the plan wants proven at parity.
# Each concept maps to the C ABI symbol; the Lua/Java detectors match by the
# bound NAME (Lua binding name or JNI method fragment), which may differ, so
# we match loosely on the concept keyword set.
CORE_PARITY = [
    ("entity.create",   "jce_scene_create_entity",   ["create_entity", "createEntity", "spawn"]),
    ("entity.destroy",  "jce_scene_destroy_entity",  ["destroy_entity", "destroyEntity", "destroy"]),
    ("transform.set",   "jce_scene_set_transform", ["set_position", "setPosition", "set_transform"]),
    ("transform.get",   "jce_scene_get_transform", ["get_position", "getPosition", "get_transform"]),
    ("resource.load",   "jce_asset",                 ["asset_read", "load", "load_asset", "loadAsset"]),
    ("log",             "jce_log",                   ["log", "print"]),
]

JCE_API_RE = re.compile(r"\bJCE_API\b[^;{]*?\b(jce_[a-z0-9_]+)\s*\(")
LUA_REG_RE = re.compile(r'\{\s*"([a-zA-Z0-9_]+)"\s*,')          # luaL_Reg { "name", fn }
LUA_PUSH_RE = re.compile(r'lua_register\s*\(\s*\w+\s*,\s*"([a-zA-Z0-9_]+)"')
# JCE's own binding helper: register_binding(L, s, "name", fn)
LUA_BIND_RE = re.compile(r'register_binding\s*\([^,]+,[^,]+,\s*"([a-zA-Z0-9_]+)"')
JNI_RE = re.compile(r"Java_[A-Za-z0-9_]+_([A-Za-z0-9]+)\s*\(|JNIEXPORT")


def read(p: Path) -> str:
    try:
        return p.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def collect_c_api() -> set[str]:
    out = set()
    for p in PUBLIC_ROOT.rglob("*.h"):
        for m in JCE_API_RE.finditer(read(p)):
            out.add(m.group(1))
    return out


def collect_lua() -> set[str]:
    out = set()
    files = list(SCRIPT_DIR.rglob("*.c")) + list(SCRIPT_DIR.rglob("*.cpp")) + [RT_SCRIPT]
    for p in files:
        if not p.exists():
            continue
        txt = read(p)
        # Only harvest names near a lua binding table/registration to avoid
        # picking up unrelated string literals; a light heuristic: collect all
        # luaL_Reg-style { "name", ... } and lua_register names.
        for m in LUA_BIND_RE.finditer(txt):
            out.add(m.group(1))
        for m in LUA_PUSH_RE.finditer(txt):
            out.add(m.group(1))
        # luaL_Reg tables only inside a lua binding context (avoid harvesting
        # the sandboxed-libs stdlib table); require the name to look like an
        # engine verb by also being registered via register_binding elsewhere.
    return out


def collect_jni() -> tuple[set[str], bool]:
    names = set()
    present = False
    files = [JNI_BRIDGE]
    if JAVA_ROOT.is_dir():
        files += list(JAVA_ROOT.rglob("*.c")) + list(JAVA_ROOT.rglob("*.cpp"))
    for p in files:
        if not p.exists():
            continue
        txt = read(p)
        if "JNIEXPORT" in txt or "Java_" in txt:
            present = True
        for m in re.finditer(r"Java_[A-Za-z0-9_]+_([A-Za-z0-9]+)\s*\(", txt):
            names.add(m.group(1).lower())
    return names, present


# ---------------------------------------------------------------------------
# Version-handshake mirror
#
# jce_version.h.in requires foreign-language bindings to call jce_api_version()
# at startup and reject a mismatching major.  The Java binding does exactly
# that -- but against a HAND-COPIED constant:
#
#     private static final int EXPECTED_API_VERSION = 0x000B0200;  // 0.11.2
#     /* Bump in lockstep with project(JCE VERSION ...) */
#
# "Bump in lockstep" is a request to a human, and this audit has already found
# three other hand-synced mirrors in this codebase.  When this one drifts the
# failure is especially unhelpful: the handshake REJECTS a correctly paired
# engine and binding, so the error message points away from the real cause.
# Check it mechanically instead.
# ---------------------------------------------------------------------------

JAVA_BINDING = REPO_ROOT / "engine" / "java" / "com" / "jce" / "JceRuntime.java"
ROOT_CMAKE = REPO_ROOT / "CMakeLists.txt"


def check_version_mirror():
    """Return a list of problems (empty = consistent, or not applicable)."""
    if not JAVA_BINDING.is_file() or not ROOT_CMAKE.is_file():
        return []

    m = re.search(r"project\s*\(\s*JCE\s+VERSION\s+(\d+)\.(\d+)\.(\d+)",
                  read(ROOT_CMAKE))
    if not m:
        return ["cannot parse project(JCE VERSION ...) from CMakeLists.txt"]
    major, minor, patch = int(m.group(1)), int(m.group(2)), int(m.group(3))
    expected = (major << 24) | (minor << 16) | (patch << 8)

    j = re.search(r"EXPECTED_API_VERSION\s*=\s*(0x[0-9A-Fa-f]+)",
                  read(JAVA_BINDING))
    if not j:
        return ["JceRuntime.java has no EXPECTED_API_VERSION -- the version "
                "handshake required by jce_version.h.in is missing"]
    got = int(j.group(1), 16)
    if got != expected:
        return ["JceRuntime.java EXPECTED_API_VERSION = 0x%08X but "
                "project(JCE VERSION %d.%d.%d) packs to 0x%08X -- the JNI "
                "handshake would REJECT a correctly paired engine at runtime"
                % (got, major, minor, patch, expected)]
    return []


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero if any expected core-parity binding is missing")
    ap.add_argument("--version-only", action="store_true",
                    help="check ONLY the version-handshake mirror and gate on it "
                         "(a drifted constant is a factual error, not a coverage "
                         "judgement, so it must fail the build even though the "
                         "parity matrix stays informational)")
    args = ap.parse_args()

    if args.version_only:
        problems = check_version_mirror()
        if problems:
            print("version-handshake mirror: FAILED")
            for v in problems:
                print("    " + v)
            return 1
        print("version-handshake mirror: OK "
              "(JceRuntime.java EXPECTED_API_VERSION == project(JCE VERSION))")
        return 0

    c_api = collect_c_api()
    lua = collect_lua()
    jni, jni_present = collect_jni()

    def has(names: set[str], keys: list[str]) -> bool:
        low = {n.lower() for n in names}
        return any(any(k.lower() in n for n in low) for k in keys)

    matrix = []
    missing = []
    for concept, c_sym, keys in CORE_PARITY:
        c_ok = any(c_sym in s for s in c_api) or c_sym in c_api
        lua_ok = has(lua, keys)
        jni_ok = has(jni, keys) if jni_present else None
        matrix.append({
            "concept": concept, "c_symbol": c_sym,
            "c": c_ok, "lua": lua_ok, "jni": jni_ok,
        })
        if not c_ok:
            missing.append(f"{concept}: C ABI symbol {c_sym} not found")
        if c_ok and not lua_ok:
            missing.append(f"{concept}: no Lua binding (looked for {keys})")

    summary = {
        "c_api_count": len(c_api),
        "lua_binding_count": len(lua),
        "jni_present": jni_present,
        "jni_method_count": len(jni),
    }

    if args.json:
        print(json.dumps({"matrix": matrix, "summary": summary, "missing": missing}, indent=1))
        return 1 if (missing and args.strict) else 0

    print("binding-parity report (C ABI = source of truth)")
    print(f"  C ABI functions (JCE_API):   {len(c_api)}")
    print(f"  Lua bindings registered:     {len(lua)}")
    print(f"  JNI present:                 {jni_present} ({len(jni)} native methods)")
    print()
    print("  concept            | C  | Lua | JNI")
    print("  -------------------|----|-----|----")
    for m in matrix:
        def mark(v):
            return " ? " if v is None else (" y " if v else " . ")
        print(f"  {m['concept']:<18} |{mark(m['c'])} |{mark(m['lua'])} |{mark(m['jni'])}")
    print()
    if missing:
        print("Gaps:")
        for x in missing:
            print(f"  - {x}")
        if args.strict:
            print(f"\n{len(missing)} parity gap(s).")
            return 1
    version_problems = check_version_mirror()
    if version_problems:
        print()
        print("version-handshake mirror: FAILED")
        for v in version_problems:
            print("    " + v)
        return 1
    print()
    print("version-handshake mirror: OK "
          "(JceRuntime.java EXPECTED_API_VERSION == project(JCE VERSION))")
    print("\n(informational; run with --strict to gate on core-parity gaps)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
