#!/usr/bin/env python3
"""
check_editor_consumer_purity.py — Keep the editor a pure engine consumer.

The editor (`editor/src/**`) is an L7 consumer of the engine, exactly like
a game built on top of JCE. To prove the engine's public API is complete
enough to build a real application ("dogfooding"), the editor MUST reach
the engine only through `<jce/...>` umbrella headers — never by pulling in
an engine-internal third-party library directly.

Concretely: a panel that calls `bgfx_get_stats()` or `SDL_GetTicks()` is a
dogfooding hole — it means the engine failed to expose that capability, and
the editor papered over the gap by linking the backend itself. This linter
fails such cases so the gap gets closed with a real public API instead.

Sanctioned exceptions:
  * ImGui — the editor's own UI toolkit, reachable via the
    `jce_tools_imgui` INTERFACE target (`<jce/tools/jce_imgui.hpp>` or
    `<imgui...>`). The engine deliberately does not wrap ImGui.
  * C/C++ standard library — the editor is C++17.

Usage:
  python scripts/lint/check_editor_consumer_purity.py
  # exits 0 if clean, 1 with file:line:reason on violation.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
EDITOR_SRC_ROOT = REPO_ROOT / "editor" / "src"

# Engine-internal third-party headers the editor must never include
# directly. Each of these has an engine public API the editor should use
# instead (e.g. bgfx -> <jce/renderer/...>, SDL -> <jce/os/platform/...>,
# flecs -> <jce/middleware/scene/...>, cJSON -> <jce/os/core/jce_json.h>).
BANNED_INCLUDES: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r'#include\s*[<"]SDL3?/'),         "SDL in editor (use <jce/os/platform/...>)"),
    (re.compile(r'#include\s*[<"]SDL\.h[">]'),     "SDL in editor (use <jce/os/platform/...>)"),
    (re.compile(r'#include\s*[<"]bgfx/'),          "bgfx in editor (use <jce/renderer/...>)"),
    (re.compile(r'#include\s*[<"]bx/'),            "bx in editor (use <jce/renderer/...>)"),
    (re.compile(r'#include\s*[<"]flecs\.h[">]'),   "flecs in editor (use <jce/middleware/scene/...>)"),
    (re.compile(r'#include\s*[<"]cjson/', re.I),   "cJSON in editor (use <jce/os/core/jce_json.h>)"),
    (re.compile(r'#include\s*[<"]cJSON\.h[">]'),   "cJSON in editor (use <jce/os/core/jce_json.h>)"),
    (re.compile(r'#include\s*[<"]RmlUi/'),         "RmlUI in editor (engine HUD dep, not editor)"),
    (re.compile(r'#include\s*[<"]btBulletDynamicsCommon\.h[">]'),
     "Bullet in editor (use <jce/middleware/physics/...>)"),
    (re.compile(r'#include\s*[<"]LinearMath/'),    "Bullet LinearMath in editor (use <jce/...>)"),
    (re.compile(r'#include\s*[<"]box2d/'),         "box2d in editor (use <jce/middleware/physics/...>)"),
    (re.compile(r'#include\s*[<"]ozz/'),           "ozz-animation in editor (use <jce/middleware/animation/...>)"),
    (re.compile(r'#include\s*[<"]Recast/'),        "Recast in editor (use <jce/middleware/ai/...>)"),
    (re.compile(r'#include\s*[<"]Detour/'),        "Detour in editor (use <jce/middleware/ai/...>)"),
    (re.compile(r'#include\s*[<"]behaviortree_cpp/'),
     "behaviortree.CPP in editor (use <jce/middleware/ai/...>)"),
    (re.compile(r'#include\s*[<"]enkiTS'),         "enkiTS in editor (use <jce/os/core/jce_jobs.h>)"),
    (re.compile(r'#include\s*[<"]TaskScheduler'),  "enkiTS in editor (use <jce/os/core/jce_jobs.h>)"),
    (re.compile(r'#include\s*[<"]mimalloc'),       "mimalloc in editor (use <jce/os/core/jce_alloc.h>)"),
    (re.compile(r'#include\s*[<"]physfs\.h[">]'),  "PhysFS in editor (use <jce/os/core/jce_filesystem.h>)"),
    (re.compile(r'#include\s*[<"]miniaudio\.h[">]'), "miniaudio in editor (use <jce/middleware/audio/...>)"),
    (re.compile(r'#include\s*[<"]zstd'),           "zstd in editor (use engine archive/compress API)"),
    (re.compile(r'#include\s*[<"]xxhash', re.I),   "xxHash in editor (use engine hash API)"),
    (re.compile(r'#include\s*[<"]dav1d'),          "dav1d in editor (use <jce/middleware/video/...>)"),
    (re.compile(r'#include\s*[<"]vpx'),            "libvpx in editor (use <jce/middleware/video/...>)"),
    (re.compile(r'#include\s*[<"]enet/'),          "enet in editor (use <jce/middleware/net/...>)"),
    (re.compile(r'#include\s*[<"]google/protobuf'), "protobuf in editor (use <jce/middleware/net/...>)"),
    (re.compile(r'#include\s*[<"]tracy', re.I),    "Tracy in editor (use <jce/os/core/jce_profiler.h>)"),
]

# Files intentionally exempt (every entry is a known dogfooding debt).
ALLOW_FILES: set[str] = {
    # Currently none.
}


def rel(p: Path) -> str:
    return p.relative_to(REPO_ROOT).as_posix()


def is_source(name: str) -> bool:
    return name.endswith((".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh", ".hxx"))


def scan() -> list[tuple[str, int, str, str]]:
    violations: list[tuple[str, int, str, str]] = []
    if not EDITOR_SRC_ROOT.is_dir():
        print(f"warn: editor src root not found: {EDITOR_SRC_ROOT}", file=sys.stderr)
        return violations

    for r, dirs, files in os.walk(EDITOR_SRC_ROOT):
        for fn in files:
            if not is_source(fn):
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
        print("editor-consumer-purity check: OK (editor reaches the engine only via <jce/...>)")
        return 0
    print("editor-consumer-purity check: FAILED — editor includes an engine-internal third-party header")
    print("(editor/src must consume the engine through <jce/...>; close the gap with a public API)")
    print()
    for rp, lineno, reason, line in v:
        print(f"  {rp}:{lineno}: {reason}")
        print(f"      {line.strip()}")
    print()
    print(f"{len(v)} violation(s).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
