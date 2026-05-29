#!/usr/bin/env python3
"""
check_engine_native_io.py — CI guard rejecting raw native I/O in engine,
client (caged_kingdom) and editor source trees.

The JCE codebase MUST go through canonical wrappers for cross-platform
consistency:
  * file I/O           → SDL_IOStream / PhysFS / jce_filesystem / jce_path
  * threading / time   → SDL3 / jce_thread / jce_atomic
  * sockets / network  → (when added) SDL_net
  * Win32 / POSIX APIs → only inside `engine/src/os/platform/` shims

In addition, client + editor code is held to a stricter bar: it must
not pull in non-portable C++ stdlib path/IO/threading headers
(`<filesystem>`, `<fstream>`, `<thread>`, `<mutex>`, `<atomic>`,
`<chrono>`, `<future>`, `<condition_variable>`) — the engine wrappers
are the only sanctioned path.

Usage:
  python scripts/lint/check_engine_native_io.py
  # exits 0 if clean, 1 with file:line:reason on violation.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Each scan tree gets its own pattern set: engine has the C / native bar,
# client + editor add the C++-stdlib portability bar on top.
SCAN_TREES: list[tuple[Path, str]] = [
    (REPO_ROOT / "engine" / "src",       "engine"),
    (REPO_ROOT / "caged_kingdom" / "src", "client"),
    (REPO_ROOT / "editor" / "src",       "editor"),
]

# Files (relative to repo root, forward slashes) that are LEGITIMATE
# platform shims. They wrap native APIs behind the canonical jce_* layer.
ALLOW_FILES = {
    "engine/src/application/jce_engine.c",          # bgfx native window handle
    "engine/src/os/core/jce_log.c",                 # OutputDebugString hook
    "engine/src/os/core/jce_sysinfo.c",             # CPU/RAM probe
    "engine/src/os/core/jce_crash_handler.c",       # SetUnhandledExceptionFilter
    "engine/src/os/platform/jce_single_instance.c", # native mutex / lock file
    "engine/src/os/platform/jce_window_modal_loop.c", # Win32 modal-loop hook
}

# Native / Win32 / POSIX bar — applied to ALL trees.
NATIVE_PATTERNS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"\bfopen\s*\("),       "stdio fopen — use jce_fs_host_* / SDL_IOFromFile"),
    (re.compile(r"\bfreopen\s*\("),     "stdio freopen — use SDL_IOFromFile"),
    (re.compile(r"\bfread\s*\("),       "stdio fread — use SDL_ReadIO"),
    (re.compile(r"\bfwrite\s*\("),      "stdio fwrite — use SDL_WriteIO"),
    (re.compile(r"\bfclose\s*\("),      "stdio fclose — use SDL_CloseIO"),
    (re.compile(r"\bCreateFile[AW]\b"), "Win32 CreateFile — use SDL_IOFromFile"),
    (re.compile(r"#\s*include\s*<windows\.h>"),  "raw <windows.h> outside platform shim"),
    (re.compile(r"#\s*include\s*<unistd\.h>"),   "raw <unistd.h> outside platform shim"),
    (re.compile(r"#\s*include\s*<sys/stat\.h>"), "raw <sys/stat.h> outside platform shim"),
    (re.compile(r"#\s*include\s*<sys/socket\.h>"), "raw <sys/socket.h> — use SDL_net"),
    (re.compile(r"#\s*include\s*<winsock2?\.h>"),  "raw <winsock.h> — use SDL_net"),
    (re.compile(r"#\s*include\s*<pthread\.h>"),    "raw <pthread.h> — use jce_thread / SDL_thread"),
]

# Portable-but-banned-in-client/editor C++ stdlib bar.
# Engine code may still touch these where appropriate (e.g. third-party
# bindings) so the patterns are scoped to client + editor trees.
PORTABILITY_PATTERNS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"#\s*include\s*<filesystem>"),
     "<filesystem> — use jce_fs_host_* + jce_path_*"),
    (re.compile(r"#\s*include\s*<fstream>"),
     "<fstream> — use jce_fs_host_read_all/write_all (or ed_read_file/ed_write_file)"),
    (re.compile(r"#\s*include\s*<thread>"),
     "<thread> — use jce_thread"),
    (re.compile(r"#\s*include\s*<mutex>"),
     "<mutex> — use jce_thread mutex API"),
    (re.compile(r"#\s*include\s*<atomic>"),
     "<atomic> — use jce_atomic_*"),
    (re.compile(r"#\s*include\s*<chrono>"),
     "<chrono> — use engine timer wrappers"),
    (re.compile(r"#\s*include\s*<future>"),
     "<future> — use jce_thread / task system"),
    (re.compile(r"#\s*include\s*<condition_variable>"),
     "<condition_variable> — use jce_thread cond var API"),
    (re.compile(r"\bstd::filesystem\b"),
     "std::filesystem — use jce_fs_host_* + jce_path_*"),
    (re.compile(r"\bstd::ifstream\b"),
     "std::ifstream — use ed_read_file / jce_fs_host_read_all"),
    (re.compile(r"\bstd::ofstream\b"),
     "std::ofstream — use ed_write_file / jce_fs_host_write_all"),
    (re.compile(r"\bstd::thread\b"),
     "std::thread — use jce_thread"),
    (re.compile(r"\bstd::mutex\b"),
     "std::mutex — use jce_thread mutex API"),
    (re.compile(r"\bstd::async\b"),
     "std::async — use jce_thread / task system"),
]


# Raw platform-detection macros — banned in client (caged_kingdom) where
# game code should be 100% platform-agnostic.  The engine routes platform
# detection through JCE_PLATFORM_* macros from <jce/os/core/jce_platform.h>.
CLIENT_PLATFORM_PATTERNS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"#if(?:def|\s+defined\s*\()?\s*(_WIN32|_WIN64|__APPLE__|__linux__|__ANDROID__|__EMSCRIPTEN__)\b"),
     "raw platform macro — use JCE_PLATFORM_* (or call engine API that wraps it)"),
]


def rel(p: Path) -> str:
    return p.relative_to(REPO_ROOT).as_posix()


def patterns_for(tree: str) -> list[tuple[re.Pattern[str], str]]:
    if tree == "engine":
        return NATIVE_PATTERNS
    if tree == "client":
        return NATIVE_PATTERNS + PORTABILITY_PATTERNS + CLIENT_PLATFORM_PATTERNS
    return NATIVE_PATTERNS + PORTABILITY_PATTERNS


def scan_tree(root: Path, tree: str) -> list[tuple[str, int, str, str]]:
    violations: list[tuple[str, int, str, str]] = []
    if not root.is_dir():
        # Tree may be optional (e.g. caged_kingdom in some clones).
        return violations

    pats = patterns_for(tree)

    for r, dirs, files in os.walk(root):
        # Skip vendored third-party trees — not our code.
        dirs[:] = [d for d in dirs if d != "third_party"]
        for fn in files:
            if not (fn.endswith(".c") or fn.endswith(".cpp") or fn.endswith(".h")):
                continue
            # Skip vendored single-header libraries (stb_*, etc.).
            if fn.startswith("stb_") or fn.startswith("miniaudio") or fn.startswith("dr_"):
                continue
            p = Path(r) / fn
            rp = rel(p)
            if rp in ALLOW_FILES:
                continue
            # All files under engine/src/os/platform/ are sanctioned
            # platform shims (this is the entire purpose of that dir).
            if rp.startswith("engine/src/os/platform/"):
                continue
            try:
                lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError as e:
                print(f"warn: cannot read {rp}: {e}", file=sys.stderr)
                continue
            for lineno, line in enumerate(lines, start=1):
                # Strip line comments cheaply to reduce false positives.
                code = line.split("//", 1)[0]
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
        print("native-io check: OK (engine + client + editor are clean)")
        return 0
    print("native-io check: FAILED — raw native I/O / non-portable stdlib found")
    print("(use SDL_IOStream, jce_filesystem, jce_path, jce_thread, etc.)")
    print()
    for rp, lineno, reason, line in v:
        print(f"  {rp}:{lineno}: {reason}")
        print(f"      {line.strip()}")
    print()
    print(f"{len(v)} violation(s).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
