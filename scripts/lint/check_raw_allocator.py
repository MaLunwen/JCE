#!/usr/bin/env python3
"""
check_raw_allocator.py — CI guard rejecting raw malloc/calloc/realloc/free
in engine + editor source trees.

All dynamic allocation MUST go through the canonical wrappers so mimalloc
is the single source of truth and Tracy can attribute lifetimes:

  * Engine  (.c)   → JCE_MALLOC / JCE_CALLOC / JCE_REALLOC / JCE_FREE
                    (engine/include/jce/os/core/jce_alloc.h)
  * Editor  (.cpp) → ED_ALLOC / ED_REALLOC / ED_FREE
                    (editor/src/core/jce_editor_alloc.h)
  * Caller-supplied jce_allocator_t{} interfaces are always allowed.
  * vendored third_party/** is skipped.

Usage:
  python scripts/lint/check_raw_allocator.py
  # exits 0 if clean, 1 with file:line:reason on violation.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

SCAN_TREES: list[tuple[Path, str]] = [
    (REPO_ROOT / "engine" / "src",       "engine"),
    (REPO_ROOT / "editor" / "src",       "editor"),
]

# Files (relative to repo root, forward slashes) intentionally exempted —
# these are the allocator implementations themselves or the crash path
# (which must not depend on any subsystem, including mimalloc).
ALLOW_FILES = {
    "engine/src/os/core/jce_allocator.c",      # mimalloc binding implementation
    "engine/src/os/core/jce_memory.c",         # JCE_* macro impl (if present)
    "engine/src/os/core/jce_crash_handler.c",  # crash path: no allocator deps
    # gpu_particles intentionally retains one raw call site annotated for
    # caller-supplied allocator pairing. See file comment for details.
    "engine/src/renderer/jce_gpu_particles.c",
}

# Raw allocator call patterns. Word-boundary anchored so member fields
# named `.alloc` / `.free` / `->alloc(...)` (caller-supplied interfaces)
# do not trip — those are reached via a `.` or `->` prefix excluded below.
RAW_ALLOC_PATTERNS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"(?<![.\w>])\bmalloc\s*\("),
     "raw malloc — use JCE_MALLOC (engine) / ED_ALLOC (editor)"),
    (re.compile(r"(?<![.\w>])\bcalloc\s*\("),
     "raw calloc — use JCE_CALLOC (engine) / ED_ALLOC + memset (editor)"),
    (re.compile(r"(?<![.\w>])\brealloc\s*\("),
     "raw realloc — use JCE_REALLOC / ED_REALLOC"),
    (re.compile(r"(?<![.\w>])\bfree\s*\("),
     "raw free — use JCE_FREE / ED_FREE"),
]

# C++ new/delete are also banned in editor (engine is C99 — non-issue).
CPP_ALLOC_PATTERNS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"(?<![\w])\bnew\s+[A-Za-z_]"),
     "C++ `new` — use ED_ALLOC + placement new (or container)"),
    (re.compile(r"(?<![\w])\bdelete\s+[A-Za-z_]"),
     "C++ `delete` — use ED_FREE (with manual dtor if needed)"),
]


def rel(p: Path) -> str:
    return p.relative_to(REPO_ROOT).as_posix()


def patterns_for(tree: str) -> list[tuple[re.Pattern[str], str]]:
    if tree == "editor":
        # Editor still has many legitimate C++ new/delete sites in panels
        # using STL containers internally; gating new/delete is a future
        # tightening pass. Today we only enforce the C-style raw bar.
        return RAW_ALLOC_PATTERNS
    return RAW_ALLOC_PATTERNS


def scan_tree(root: Path, tree: str) -> list[tuple[str, int, str, str]]:
    violations: list[tuple[str, int, str, str]] = []
    if not root.is_dir():
        return violations

    pats = patterns_for(tree)

    for r, dirs, files in os.walk(root):
        # Skip vendored third-party trees.
        dirs[:] = [d for d in dirs if d != "third_party"]
        for fn in files:
            if not (fn.endswith(".c") or fn.endswith(".cpp") or fn.endswith(".h") or fn.endswith(".hpp")):
                continue
            # Skip vendored single-header libraries.
            if fn.startswith("stb_") or fn.startswith("miniaudio") or fn.startswith("dr_"):
                continue
            p = Path(r) / fn
            rp = rel(p)
            if rp in ALLOW_FILES:
                continue
            try:
                lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError as e:
                print(f"warn: cannot read {rp}: {e}", file=sys.stderr)
                continue
            in_block_comment = False
            for lineno, line in enumerate(lines, start=1):
                # Per-line opt-out for justified exceptions — e.g. a virtual
                # method named free()/malloc() that overrides an allocator
                # interface (Recast/Detour), not the global allocator.
                # Annotate the line with `raw-alloc-ok: <reason>`.
                if "raw-alloc-ok" in line:
                    continue
                code = line
                # Handle multi-line /* ... */ block comments.
                out_chars: list[str] = []
                i = 0
                while i < len(code):
                    if in_block_comment:
                        end = code.find("*/", i)
                        if end == -1:
                            i = len(code)
                        else:
                            in_block_comment = False
                            i = end + 2
                    else:
                        start = code.find("/*", i)
                        if start == -1:
                            out_chars.append(code[i:])
                            i = len(code)
                        else:
                            out_chars.append(code[i:start])
                            in_block_comment = True
                            i = start + 2
                code = "".join(out_chars)
                # Strip line comments.
                code = code.split("//", 1)[0]
                for pat, reason in pats:
                    if pat.search(code):
                        violations.append((rp, lineno, reason, line.rstrip()))
    return violations


def scan() -> list[tuple[str, int, str, str]]:
    out: list[tuple[str, int, str, str]] = []
    for root, tree in SCAN_TREES:
        out.extend(scan_tree(root, tree))
    return out


def main() -> int:
    v = scan()
    if not v:
        print("raw-allocator check: OK (engine + editor route through JCE_/ED_ wrappers)")
        return 0
    print("raw-allocator check: FAILED — raw malloc/calloc/realloc/free found")
    print("(use JCE_MALLOC etc. in engine .c, ED_ALLOC etc. in editor .cpp)")
    print()
    for rp, lineno, reason, line in v:
        print(f"  {rp}:{lineno}: {reason}")
        print(f"      {line.strip()}")
    print()
    print(f"{len(v)} violation(s).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
