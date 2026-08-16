#!/usr/bin/env python3
"""One loaded terrain per path, owned by the scene's terrain cache.

The same terrain asset used to be read from disk FIVE times, each into a
privately-owned JceTerrain: the editor panel, the scene renderer, the runtime
at physics spawn, the pick pass, and foliage borrowing the renderer's.  The
duplicated memory was the mild part.  They were five copies that could
DISAGREE -- sculpt a valley and the renderer showed it while the collider the
player walked on and the mesh the mouse picked against were still the version
on disk at level load.  Nothing could fix it, because no owner knew the others
existed.

jce_terrain_cache.{h,c} is now the single owner and the scene holds it.  This
gate keeps it that way, because a sixth private load would restore exactly the
old behaviour and would look completely reasonable in review.

Two rules:

  1. Only the cache (and the terrain module itself) may LOAD a terrain
     (jce_terrain_load_file / jce_terrain_load_from_pak).  Consumers call
     jce_scene_terrain_cache() + jce_terrain_cache_acquire().

  2. Only the cache may FREE one (jce_terrain_free).  Every consumer holds a
     BORROWED pointer, so a free anywhere else is a use-after-free for all the
     others -- the failure this refactor's whole ownership model exists to make
     impossible.

The editor's authoring panel is exempt from rule 1 by allowlist: it owns a
genuinely separate MUTABLE working copy with its own undo stack, which is not
the shared read-only asset.  It is still bound by rule 2's spirit -- it frees
only what it loaded itself.

An explicit opt-out comment on the line or the line above allows a deliberate
exception, so an exemption is something someone wrote down rather than a diff
nobody noticed.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

LOAD_ALLOWED = {
    "engine/src/middleware/scene/jce_terrain_cache.c",
    "engine/src/middleware/scene/jce_terrain.c",
    "engine/include/jce/middleware/scene/jce_terrain.h",
    # The authoring copy: mutable, undo-stacked, deliberately not the shared
    # read-only asset.  See the module header.
    "editor/src/panels/jce_panel_terrain.cpp",
}

FREE_ALLOWED = {
    "engine/src/middleware/scene/jce_terrain_cache.c",
    "engine/src/middleware/scene/jce_terrain.c",
    "engine/include/jce/middleware/scene/jce_terrain.h",
    "editor/src/panels/jce_panel_terrain.cpp",
}

OPT_OUT = "jce-terrain-owner-exempt"
OPT_OUT_LOOKBACK = 3

LOAD_RE = re.compile(r"\bjce_terrain_load_(file|from_pak)\s*\(")
FREE_RE = re.compile(r"\bjce_terrain_free\s*\(")

SCAN_DIRS = ["engine/src", "engine/include", "editor/src", "caged_kingdom/src"]
SUFFIXES = {".c", ".h", ".cpp", ".hpp"}


def scan():
    failures = []
    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            rel = path.relative_to(ROOT).as_posix()
            try:
                lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError:
                continue
            for i, line in enumerate(lines):
                # Look back a few lines, not one: a real justification is a
                # block comment, and the token naturally lands on its first
                # line rather than the one nearest the code.
                window = lines[max(0, i - OPT_OUT_LOOKBACK):i + 1]
                if any(OPT_OUT in w for w in window):
                    continue
                if LOAD_RE.search(line) and rel not in LOAD_ALLOWED:
                    failures.append(
                        f"{rel}:{i + 1}: loads a terrain outside the scene's cache.\n"
                        f"    {line.strip()}\n"
                        f"    Use jce_terrain_cache_acquire(jce_scene_terrain_cache(scene), ...)."
                    )
                if FREE_RE.search(line) and rel not in FREE_ALLOWED:
                    failures.append(
                        f"{rel}:{i + 1}: frees a BORROWED terrain.\n"
                        f"    {line.strip()}\n"
                        f"    The scene's terrain cache owns it; freeing here dangles "
                        f"every other consumer."
                    )
    return failures


def main():
    failures = scan()
    if failures:
        print("check_terrain_single_owner: FAILED\n")
        for f in failures:
            print("  " + f + "\n")
        print(
            f"  If an exception is genuinely correct, write '{OPT_OUT}' on the line\n"
            f"  or the line above it, with a comment saying why."
        )
        return 1
    print("check_terrain_single_owner: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
