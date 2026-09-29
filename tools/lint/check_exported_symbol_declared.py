#!/usr/bin/env python3
"""
check_exported_symbol_declared.py — JCE_API on a symbol no public header
declares.

WHAT JCE_API ACTUALLY IS HERE, because the obvious reading is wrong and this
gate's first docstring got it wrong.  jce_defs.h expands it to
__declspec(dllexport) only under `#if defined(JCE_SHARED)`, and JCE_SHARED is
defined by NO CMakeLists.txt or .cmake in this repository -- checked, not
assumed.  So JCE_API expands to NOTHING in every configuration this tree
builds.  It is not an export in practice; it is the tree's MARKER for "this
is public API surface", and check_editor_consumption counts 3,555 of them.

WHY IT STILL MATTERS.  A JCE_API on a symbol that no public header declares
mislabels an internal function as public, inflates that count, and sits
outside contracts/abi-snapshot.txt -- which is generated from engine/include
only.  So the marker says "contracted public API" about something no
contract covers and no SDK holder can call.  If JCE_SHARED is ever switched
on, the same declarations become real exports with no declaration to import
them from.

WHAT THIS CHECKS.  Every `JCE_API ... name(` in a header under engine/src or
editor/src must also appear as a JCE_API declaration under engine/include.

Usage:
    python tools/lint/check_exported_symbol_declared.py
    python tools/lint/check_exported_symbol_declared.py --list
Exit 0 clean, 1 on any private-only JCE_API symbol.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

# Enumerate the WORKING TREE, not tracked-only: `git ls-files` hides a
# brand-new file until it is committed, so a defect introduced in a new file
# passes this checker on the commit that introduces it.  Measured: an internal
# function marked JCE_API sailed through a full run and failed on the next one,
# one commit late.  See tools/lint/lint_git_files.py.
import importlib.util as _ilu
_spec = _ilu.spec_from_file_location(
    "lint_git_files", str(Path(__file__).resolve().parent / "lint_git_files.py"))
_lgf = _ilu.module_from_spec(_spec)
_spec.loader.exec_module(_lgf)


LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]
EXEMPT = LINT_DIR / "exported_symbol_exempt.txt"

PUBLIC_PREFIX = "engine/include/"
PRIVATE_PREFIXES = ("engine/src/", "editor/src/")
HEADER_SUFFIXES = (".h", ".hpp")

# `JCE_API <return type, possibly with JCE_CALL and pointers> name(`
DECL = re.compile(r"JCE_API[^;{)]*?\b(\w+)\s*\(")


def headers() -> tuple:
    listing = _lgf.working_tree()
    pub, priv = [], []
    for rel in listing:
        if not rel.endswith(HEADER_SUFFIXES):
            continue
        if rel.startswith(PUBLIC_PREFIX):
            pub.append(rel)
        elif rel.startswith(PRIVATE_PREFIXES):
            priv.append(rel)
    return pub, priv


def symbols(paths) -> dict:
    """name -> the headers declaring it with JCE_API."""
    out: dict = {}
    for rel in paths:
        text = (REPO_ROOT / rel).read_text(encoding="utf-8", errors="replace")
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//[^\n]*", "", text)
        for name in DECL.findall(text):
            out.setdefault(name, set()).add(rel)
    return out


def read_exemptions() -> dict:
    out: dict = {}
    if not EXEMPT.is_file():
        return out
    for raw in EXEMPT.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        name, _, reason = line.partition(" ")
        out[name.strip()] = reason.strip()
    return out


def main() -> int:
    pub_paths, priv_paths = headers()
    if len(pub_paths) < 100:
        print("check_exported_symbol_declared: FAIL - found only %d public "
              "header(s); the tree shape changed and this extractor no longer "
              "sees them" % len(pub_paths), file=sys.stderr)
        return 1

    public = symbols(pub_paths)
    private = symbols(priv_paths)
    exempt = read_exemptions()

    offenders = sorted(n for n in private if n not in public)

    if "--list" in sys.argv:
        for name in offenders:
            state = "EXEMPT" if name in exempt else "PRIVATE-ONLY EXPORT"
            print("%-40s %-22s %s"
                  % (name, state, ", ".join(sorted(private[name]))))

    ok = True
    for name in offenders:
        if name in exempt:
            if not exempt[name]:
                ok = False
                print("  %s: exempted with no reason.  An exemption without a "
                      "stated reason is indistinguishable from an oversight."
                      % name, file=sys.stderr)
            continue
        ok = False
        print("  %s: JCE_API in %s, and no public header under %s declares "
              "it.  JCE_API is this tree's marker for public API surface, so "
              "this labels an internal function as public: it is counted as "
              "public by check_editor_consumption, it is outside "
              "contracts/abi-snapshot.txt (generated from public headers "
              "only), and no SDK holder can call it.  Drop the JCE_API, move "
              "the declaration to a public header, or exempt it with a "
              "reason."
              % (name, ", ".join(sorted(private[name])), PUBLIC_PREFIX),
              file=sys.stderr)

    dead = [n for n in exempt if n not in offenders]
    for name in dead:
        ok = False
        print("  %s: exempted, but it is no longer a private-only export.  "
              "Drop the exemption." % name, file=sys.stderr)

    if not ok:
        print("check_exported_symbol_declared: FAIL", file=sys.stderr)
        return 1

    if offenders:
        print("check_exported_symbol_declared: OK (%d JCE_API symbol(s) in "
              "public headers; %d private-only, each exempted with a reason)"
              % (len(public), len(offenders)))
    else:
        print("check_exported_symbol_declared: OK (%d JCE_API symbol(s), "
              "every one declared in a public header)" % len(public))
    return 0


if __name__ == "__main__":
    sys.exit(main())
