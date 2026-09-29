#!/usr/bin/env python3
"""
check_shader_reserved_identifiers.py — a shader local whose name ends in '_'
becomes a RESERVED identifier on OpenGL, and only on OpenGL.

THE MECHANISM, measured on 2026-09-01 rather than guessed.  bgfx's shaderc
routes the `glsl` profile through glsl-optimizer, which renames every local to
`<name>_<id>`.  In fs_sky_glsl.bin that produced:

    budget      -> budget_24        (fine)
    li_         -> li__37           (NOT fine)

GLSL reserves every identifier containing a double underscore, so NVIDIA's
compiler rejected the whole shader:

    bgfx fatal: Failed to compile shader. 0(159) :
                error C7528: OpenGL reserves names containing '__'

The sky was therefore broken on OpenGL and nowhere else: D3D11, Vulkan and
Metal never go through glsl-optimizer, so the same source compiles cleanly
there and no test, no gate and no build warning said a word.  It took actually
launching the editor with JCE_BACKEND=opengl to see it.

WHY A SOURCE-LEVEL RULE.  The failure is only observable in a built OpenGL
blob on a driver strict enough to enforce the reservation.  The cause is one
character in the source, so that is where this checks.  Two rules:

  1. No identifier in shader CODE may end in '_'  -- that is what becomes '__'
     after the rename.
  2. No identifier may already contain '__'       -- reserved outright.

Comments are stripped first.  Four of the six trailing-underscore hits in this
tree at the time of writing were prose with a wildcard ("the fog_* fields",
"emits fs_pbr_fwdplus_<backend>.bin"); a gate that flagged those would have
been switched off within a day, and the two real ones would have gone with it.

Usage:
    python tools/lint/check_shader_reserved_identifiers.py
    python tools/lint/check_shader_reserved_identifiers.py --list
Exit 0 clean, 1 on any offending identifier.
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


REPO_ROOT = Path(__file__).resolve().parents[2]

SHADER_SUFFIXES = (".sc", ".sh")
# User projects author their own shaders and are out of scope for the general
# engine gates (owner decision 2026-08-27); tests carry deliberate fixtures.
SKIP_PREFIXES = ("examples/caged_kingdom/", "tests/", "space/")

TRAILING = re.compile(r"\b([A-Za-z][A-Za-z0-9_]*_)(?![A-Za-z0-9_])")
DOUBLED = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*__[A-Za-z0-9_]*)\b")

# bgfx's own vocabulary, spelled by its headers rather than by us.  Nothing in
# it ends in '_' today; the set exists so that adding a bgfx-mandated name here
# stays a deliberate, visible act instead of a quiet loosening of the regex.
ALLOWED: set = set()


def strip_comments(text: str) -> str:
    """Blank out /* */ and // regions, preserving line count and offsets."""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:end]))
            i = end
        elif text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def shader_files() -> list:
    listing = _lgf.working_tree()
    return sorted(rel for rel in listing
                  if rel.endswith(SHADER_SUFFIXES)
                  and not rel.startswith(SKIP_PREFIXES))


def offenders(rel: str):
    """-> [(line_no, identifier, why)]"""
    text = (REPO_ROOT / rel).read_text(encoding="utf-8", errors="replace")
    code = strip_comments(text)
    hits = []
    for line_no, line in enumerate(code.splitlines(), 1):
        for m in DOUBLED.finditer(line):
            if m.group(1) not in ALLOWED:
                hits.append((line_no, m.group(1), "already contains '__'"))
        for m in TRAILING.finditer(line):
            name = m.group(1)
            if name in ALLOWED or "__" in name:
                continue
            hits.append((line_no, name,
                         "ends in '_', so glsl-optimizer renames it to "
                         "'%s_<id>' -- a reserved '__' name" % name))
    return hits


def main() -> int:
    files = shader_files()
    if len(files) < 50:
        print("check_shader_reserved_identifiers: FAIL - found only %d shader "
              "file(s); the tree shape changed and this extractor no longer "
              "sees them" % len(files), file=sys.stderr)
        return 1

    found = [(rel, hits) for rel in files if (hits := offenders(rel))]

    if "--list" in sys.argv:
        for rel, hits in found:
            for line_no, name, why in hits:
                print("%s:%d  %-24s %s" % (rel, line_no, name, why))

    if not found:
        print("check_shader_reserved_identifiers: OK (%d shader file(s), no "
              "identifier ends in '_' or contains '__')" % len(files))
        return 0

    for rel, hits in found:
        for line_no, name, why in hits:
            print("  %s:%d: `%s` %s.\n"
                  "      This breaks on OpenGL ONLY -- D3D11, Vulkan and Metal "
                  "never go through glsl-optimizer, so the same source compiles "
                  "cleanly there and the shader fails at RUNTIME on GL with "
                  "\"error C7528: OpenGL reserves names containing '__'\".  "
                  "Drop the trailing underscore."
                  % (rel, line_no, name, why), file=sys.stderr)
    print("check_shader_reserved_identifiers: FAILED", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
