#!/usr/bin/env python3
"""
check_public_api_purity.py — Guard the JCE public C API (`<jce/...>`).

The public umbrella headers under `engine/include/jce/` form JCE's ABI.
They MUST be self-contained C99 (or C++ via extern "C") and MUST NOT
expose any third-party type, macro, or include. If a single SDL/bgfx/
flecs/ImGui/Bullet/etc. symbol leaks into a public header, every FFI
binding generator breaks and every consumer is forced to install the
third-party include path.

This linter enforces the rule:

  Files under `engine/include/jce/**.h(pp)?` MUST NOT contain
  `#include` lines that reference banned third-party headers.

Engine implementation files (`engine/src/**`) MAY include third-party
headers (that's where the integration happens). Editor/client are out
of scope here — they should never include `engine/include/jce/` internal
details either, but that's enforced by other linters.

Usage:
  python scripts/lint/check_public_api_purity.py
  # exits 0 if clean, 1 with file:line:reason on violation.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PUBLIC_API_ROOT = REPO_ROOT / "engine" / "include" / "jce"

# Banned include substrings. Pattern matches any `#include <...>` or
# `#include "..."` whose path contains the substring.
# Order is informational; first match wins.
BANNED_INCLUDES: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r'#include\s*[<"]SDL3?/'),         "SDL header leaked into public API"),
    (re.compile(r'#include\s*[<"]SDL\.h[">]'),     "SDL header leaked into public API"),
    (re.compile(r'#include\s*[<"]bgfx/'),          "bgfx header leaked into public API"),
    (re.compile(r'#include\s*[<"]bx/'),            "bx (bgfx helper) leaked into public API"),
    (re.compile(r'#include\s*[<"]flecs\.h[">]'),   "flecs leaked into public API (ECS is private)"),
    (re.compile(r'#include\s*[<"]imgui'),          "ImGui leaked into public API (editor-only dep)"),
    (re.compile(r'#include\s*[<"]RmlUi/'),         "RmlUI leaked into public API (private)"),
    (re.compile(r'#include\s*[<"]btBulletDynamicsCommon\.h[">]'),
     "Bullet leaked into public API"),
    (re.compile(r'#include\s*[<"]LinearMath/'),    "Bullet LinearMath leaked into public API"),
    (re.compile(r'#include\s*[<"]box2d/'),         "box2d leaked into public API"),
    (re.compile(r'#include\s*[<"]ozz/'),           "ozz-animation leaked into public API"),
    (re.compile(r'#include\s*[<"]miniaudio\.h[">]'), "miniaudio leaked into public API"),
    (re.compile(r'#include\s*[<"]Recast/'),        "Recast leaked into public API"),
    (re.compile(r'#include\s*[<"]Detour/'),        "Detour leaked into public API"),
    (re.compile(r'#include\s*[<"]behaviortree_cpp/'),
     "behaviortree.CPP leaked into public API"),
    (re.compile(r'#include\s*[<"]enkiTS'),         "enkiTS leaked into public API"),
    (re.compile(r'#include\s*[<"]TaskScheduler'),  "enkiTS TaskScheduler leaked into public API"),
    (re.compile(r'#include\s*[<"]mimalloc'),       "mimalloc leaked into public API"),
    (re.compile(r'#include\s*[<"]physfs\.h[">]'),  "PhysFS leaked into public API"),
    (re.compile(r'#include\s*[<"]tracy'),          "Tracy leaked into public API (must be compile-gated)"),
    (re.compile(r'#include\s*[<"]Tracy'),          "Tracy leaked into public API (must be compile-gated)"),
    (re.compile(r'#include\s*[<"]cJSON'),          "cJSON leaked into public API"),
    (re.compile(r'#include\s*[<"]zstd'),           "zstd leaked into public API"),
    (re.compile(r'#include\s*[<"]xxhash'),         "xxHash leaked into public API"),
    (re.compile(r'#include\s*[<"]dav1d'),          "dav1d leaked into public API"),
    (re.compile(r'#include\s*[<"]vpx'),            "libvpx leaked into public API"),
    (re.compile(r'#include\s*[<"]enet/'),          "enet leaked into public API"),
    (re.compile(r'#include\s*[<"]google/protobuf'),
     "protobuf leaked into public API"),
    # Banned C++ stdlib (public C API must be C99-callable):
    (re.compile(r'#include\s*<[a-z_]+>'),
     "C++ stdlib include in public API (header must be C99)"),
]

# Allow-list: specific public headers that are intentionally C++ (e.g.
# the editor-only `<jce/tools/...>` headers may pull <imgui.h>). Keep
# minimal — every entry is an admission that part of the surface is not
# truly C-callable.
ALLOW_FILES: set[str] = {
    # Currently none — all `<jce/...>` must remain pure C99.
}


def rel(p: Path) -> str:
    return p.relative_to(REPO_ROOT).as_posix()


def is_header(name: str) -> bool:
    return name.endswith((".h", ".hpp", ".hh", ".hxx"))


def scan() -> list[tuple[str, int, str, str]]:
    violations: list[tuple[str, int, str, str]] = []
    if not PUBLIC_API_ROOT.is_dir():
        print(f"warn: public API root not found: {PUBLIC_API_ROOT}", file=sys.stderr)
        return violations

    for r, dirs, files in os.walk(PUBLIC_API_ROOT):
        for fn in files:
            if not is_header(fn):
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
                # Strip /* ... */ block comments (may span multiple lines).
                out_chars: list[str] = []
                i = 0
                while i < len(line):
                    if in_block_comment:
                        end = line.find("*/", i)
                        if end == -1:
                            i = len(line)
                        else:
                            in_block_comment = False
                            i = end + 2
                    else:
                        start = line.find("/*", i)
                        if start == -1:
                            out_chars.append(line[i:])
                            i = len(line)
                        else:
                            out_chars.append(line[i:start])
                            in_block_comment = True
                            i = start + 2
                code = "".join(out_chars).split("//", 1)[0]
                for pat, reason in BANNED_INCLUDES:
                    if pat.search(code):
                        violations.append((rp, lineno, reason, line.rstrip()))
                        break
    return violations


def main() -> int:
    v = scan()
    if not v:
        print("public-api-purity check: OK (no third-party leakage in <jce/...> headers)")
        return 0
    print("public-api-purity check: FAILED — third-party / non-C99 includes in public API")
    print("(public headers under engine/include/jce/ must be self-contained C99)")
    print()
    for rp, lineno, reason, line in v:
        print(f"  {rp}:{lineno}: {reason}")
        print(f"      {line.strip()}")
    print()
    print(f"{len(v)} violation(s).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
