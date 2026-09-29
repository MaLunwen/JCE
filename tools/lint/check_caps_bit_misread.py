#!/usr/bin/env python3
"""
check_caps_bit_misread.py — a clear capability bit is only an answer if the
backend sets that bit at all.

THE BUG THIS EXISTS FOR.  Three features read

    caps->formats[FMT] & BGFX_CAPS_FORMAT_TEXTURE_VERTEX

as a plain boolean.  bgfx's OpenGL renderer never sets that flag -- for any
format, on any hardware; it appears zero times in renderer_gl.cpp against two
sites in renderer_d3d11.cpp and one each in renderer_vk.cpp and
renderer_d3d12.cpp.  So on every OpenGL machine the expression was false
regardless of the GPU, and three features were silently and permanently off:

  * FFT water displacement   (warned, blaming the hardware)
  * dynamic water ripples    (silent)
  * GPU crowd instancing     (silent; the comment blamed "low tiers")

Measured 2026-09-03 with a one-shot probe, four backends, same machine:

    OpenGL 4.6   state(RGBA32F)=UNANSWERED  state(R32F)=UNANSWERED
    D3D11        state(RGBA32F)=YES         state(R32F)=YES
    D3D12        state(RGBA32F)=YES         state(R32F)=YES
    Vulkan       state(RGBA32F)=YES         state(R32F)=YES

The three answering backends are the positive control: the field works, and
OpenGL's silence is not a "no".

WHAT IS CHECKED.  BGFX_CAPS_FORMAT_TEXTURE_VERTEX may be named only inside
jce_renderer_caps.c, which implements jce_gpu_vertex_fetch_state() /
jce_gpu_vertex_fetch_usable().  Every other read is reported, because a raw
read cannot distinguish "no" from "never asked".

This is deliberately narrow.  It gates ONE flag -- the one with a measured
population gap -- rather than every BGFX_CAPS_*, because the other flags have
not been shown to have the same hole and a gate that fires on correct code
gets switched off.  Widen it when another flag is measured to behave this way,
and put the measurement in this docstring when you do.

Usage:
    python tools/lint/check_caps_bit_misread.py
    python tools/lint/check_caps_bit_misread.py --list
Exit 0 clean, 1 when a raw read exists outside the helper.
"""

from __future__ import annotations

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

SCAN_PREFIXES = ("engine/", "editor/", "tools/", "scripting/")
SOURCE_SUFFIXES = (".c", ".cpp", ".h", ".hpp", ".inc")

FLAG = "BGFX_CAPS_FORMAT_TEXTURE_VERTEX"

# The one translation unit allowed to read it: it is the helper's body.
OWNER = "engine/src/renderer/jce_renderer_caps.c"


def strip_comments(text: str) -> str:
    """Blank /* */ and // regions, preserving line structure.

    Not optional, and its own first run proved it: the gate reported three
    hits on a clean tree, and all three were the /* */ comments that EXPLAIN
    this defect -- including the one in the public header describing the
    helper.  Stripping only // left exactly the prose a reader needs most
    looking like the bug.  (Nine sibling checkers carry their own copy of
    this; a lint script is deliberately standalone here.)
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(c if c == chr(10) else " "
                               for c in text[i:end]))
            i = end
        elif text.startswith("//", i):
            end = text.find(chr(10), i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def tracked() -> list:
    out = _lgf.working_tree()
    return [f for f in out
            if f.startswith(SCAN_PREFIXES) and f.endswith(SOURCE_SUFFIXES)]


def main() -> int:
    files = tracked()
    if len(files) < 200:
        print("check_caps_bit_misread: FAIL - only %d tracked source file(s); "
              "the tree shape changed and this gate no longer sees the code."
              % len(files), file=sys.stderr)
        return 1

    owner_reads = 0
    findings = []
    for rel in files:
        try:
            text = (REPO_ROOT / rel).read_text(encoding="utf-8",
                                               errors="replace")
        except OSError:
            continue
        if FLAG not in text:
            continue
        raw = text.splitlines()
        code_lines = strip_comments(text).splitlines()
        for i, line in enumerate(code_lines, start=1):
            if FLAG not in line:
                continue          # comments may name it; that is the point
            if rel == OWNER:
                owner_reads += 1
                continue
            findings.append((rel, i, raw[i - 1].strip()[:100]))

    if owner_reads == 0:
        print("check_caps_bit_misread: FAIL - %s no longer reads %s, so this "
              "gate is guarding a helper that does not exist.  If the helper "
              "moved, move OWNER with it."
              % (OWNER, FLAG), file=sys.stderr)
        return 1

    if "--list" in sys.argv:
        print("%s reads inside the helper: %d" % (FLAG, owner_reads))
        for rel, line, text in findings:
            print("%-58s %s" % ("%s:%d" % (rel, line), text))

    if findings:
        for rel, line, text in findings:
            print("  %s:%d reads %s directly.  A clear bit is an answer only "
                  "if the backend sets that bit at all, and bgfx's OpenGL "
                  "renderer never sets this one -- so a raw read is false on "
                  "every GL machine regardless of hardware.  Use "
                  "jce_gpu_vertex_fetch_usable() (or _state() when the three "
                  "cases differ)." % (rel, line, FLAG), file=sys.stderr)
        print("check_caps_bit_misread: FAILED", file=sys.stderr)
        return 1

    print("check_caps_bit_misread: OK (%s is read only in %s, behind "
          "jce_gpu_vertex_fetch_state/_usable)" % (FLAG, OWNER))
    return 0


if __name__ == "__main__":
    sys.exit(main())
