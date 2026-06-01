#!/usr/bin/env python3
"""
check_platform_macros.py — Reject raw platform-detection macros in
first-party engine + editor source.

JCE routes ALL platform detection through the JCE_PLATFORM_* family
defined in <jce/os/core/jce_defs.h>. Raw compiler/OS macros such as
_WIN32, __APPLE__, __linux__, __ANDROID__ and __EMSCRIPTEN__ MUST NOT
appear in first-party translation units; they fragment behaviour across
toolchains and bypass the single canonical mapping.

The ONLY sanctioned places for raw platform macros are:
  * engine/include/jce/os/core/jce_defs.h — the canonical mapping itself
    (not scanned here; this linter only walks src trees).
  * engine/src/os/platform/**            — the platform-shim layer.
  * vendored third-party code            — third_party/ dirs, stb_*, etc.
  * a short ALLOW_FILES list of genuine compiler/STL shims.

Mapping cheat-sheet (raw → JCE):
  _WIN32          → JCE_PLATFORM_WINDOWS
  __APPLE__       → JCE_PLATFORM_APPLE (then JCE_PLATFORM_IOS / _MACOS)
  __ANDROID__     → JCE_PLATFORM_ANDROID
  __linux__       → JCE_PLATFORM_LINUX
  __EMSCRIPTEN__  → JCE_PLATFORM_WEB

Usage:
  python scripts/lint/check_platform_macros.py
  # exits 0 if clean, 1 with file:line:reason on violation.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

SCAN_TREES = [
    REPO_ROOT / "engine" / "src",
    REPO_ROOT / "editor" / "src",
    REPO_ROOT / "caged_kingdom" / "src",
]

# Genuine compiler / STL shims and sanctioned native shims that MUST
# speak the raw dialect.  These mirror check_engine_native_io.py's
# ALLOW_FILES: a file that branches on platform purely to call native
# APIs (backtrace, OutputDebugString, sysconf, native window handles) is
# the platform-shim layer by another name, even when it lives in os/core
# rather than os/platform.  Everything else MUST use JCE_PLATFORM_*.
ALLOW_FILES = {
    "engine/src/sdk_shims/jce_msvc_stl_shims.cpp",  # MSVC STL ABI shim
    "engine/src/application/jce_engine.c",          # bgfx native window handle
    "engine/src/os/core/jce_log.c",                 # OutputDebugString / android log
    "engine/src/os/core/jce_sysinfo.c",             # CPU/RAM native probe
    "engine/src/os/core/jce_crash_handler.c",       # signals / backtrace / SEH
}

# A preprocessor conditional line ...
COND_RE = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b")
# ... that references any of these raw platform macros is a violation.
BANNED_RE = re.compile(
    r"\b(_WIN32|_WIN64|__APPLE__|__linux__|__ANDROID__|__EMSCRIPTEN__)\b"
)

MAPPING = {
    "_WIN32": "JCE_PLATFORM_WINDOWS",
    "_WIN64": "JCE_PLATFORM_WINDOWS",
    "__APPLE__": "JCE_PLATFORM_APPLE",
    "__linux__": "JCE_PLATFORM_LINUX",
    "__ANDROID__": "JCE_PLATFORM_ANDROID",
    "__EMSCRIPTEN__": "JCE_PLATFORM_WEB",
}


def rel(p: Path) -> str:
    return p.relative_to(REPO_ROOT).as_posix()


def scan_tree(root: Path) -> list[tuple[str, int, str, str]]:
    violations: list[tuple[str, int, str, str]] = []
    if not root.is_dir():
        return violations

    for r, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d != "third_party"]
        for fn in files:
            if not (fn.endswith(".c") or fn.endswith(".cpp")
                    or fn.endswith(".h") or fn.endswith(".hpp")):
                continue
            if fn.startswith("stb_") or fn.startswith("miniaudio") \
                    or fn.startswith("dr_"):
                continue
            p = Path(r) / fn
            rp = rel(p)
            if rp in ALLOW_FILES:
                continue
            if rp.startswith("engine/src/os/platform/"):
                continue
            try:
                lines = p.read_text(encoding="utf-8",
                                    errors="replace").splitlines()
            except OSError as e:
                print(f"warn: cannot read {rp}: {e}", file=sys.stderr)
                continue
            for lineno, line in enumerate(lines, start=1):
                if not COND_RE.match(line):
                    continue
                m = BANNED_RE.search(line)
                if m:
                    raw = m.group(1)
                    repl = MAPPING.get(raw, "JCE_PLATFORM_*")
                    reason = f"raw platform macro {raw} — use {repl}"
                    violations.append((rp, lineno, reason, line.rstrip()))
    return violations


def main() -> int:
    v: list[tuple[str, int, str, str]] = []
    for root in SCAN_TREES:
        v.extend(scan_tree(root))
    if not v:
        print("platform-macro check: OK (engine + editor + client are clean)")
        return 0
    print("platform-macro check: FAILED — raw platform macros found")
    print("(route platform detection through JCE_PLATFORM_* in jce_defs.h)")
    print()
    for rp, lineno, reason, line in v:
        print(f"  {rp}:{lineno}: {reason}")
        print(f"      {line.strip()}")
    print()
    print(f"{len(v)} violation(s).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
