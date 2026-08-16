#!/usr/bin/env python3
"""check_material_texture_sampler.py

A material texture must be created WRAP + mipped.

The engine has two RGBA8 upload entry points and they are not
interchangeable:

  jce_texture_from_rgba()     CLAMP, mip-0 only.  Right for surfaces sampled at
                              roughly 1:1, and for surfaces where wrapping would
                              fetch an unrelated neighbour: glyph atlases, 1-D
                              lookup tables, video frames, UI thumbnails.

  jce_texture_from_rgba_ex()  Explicit sampler mode + a full box-filtered mip
                              chain.  Required for anything used as a MATERIAL.

Using the first for a material is silent and slow to diagnose.  Terrain
multiplies its UV by the layer tile scale, so a clamped sampler returns the edge
texel for every tile past the first -- the ground renders as horizontal streaks,
which reads as a broken texture ASSET rather than as a sampler flag.  The
missing mip chain then aliases whatever detail survives.  That is how it shipped:
the editor's scene-texture cache called the ~1:1 entry point for every material
in every scene, and the terrain shader was the first surface whose UVs left
[0,1] far enough to make it visible.

The rule is an allow-list rather than a pattern match, so a NEW call site fails
by default and has to be justified deliberately.  Adding an entry here is a
claim that the surface is sampled ~1:1 and never tiled.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Call sites where CLAMP + mip-0 is the CORRECT choice, with the reason.
# Keyed by repo-relative path; the value documents why 1:1/clamp holds.
ALLOWED = {
    "editor/src/panels/jce_panel_assets_thumb.cpp":
        "asset-browser thumbnail, drawn ~1:1 in ImGui",
    "editor/src/panels/jce_panel_file_viewer.cpp":
        "image preview tab, fit-to-window blit",
    "editor/src/panels/jce_panel_sprite_editor.cpp":
        "sprite editor canvas, drawn at explicit zoom",
    "editor/src/viewers/jce_fv_material.cpp":
        "material preview swatch",
    "editor/src/viewers/jce_fv_video.cpp":
        "decoded video frame, full-quad",
    "engine/src/middleware/scene/jce_scene_video.c":
        "VideoPlayer output frame, full-quad",
    "engine/src/renderer/jce_ies_profile.c":
        "IES angular LUT -- 1-D, and WRAP would fold the angle around",
    "engine/src/renderer/jce_text.c":
        "glyph atlas -- mips would bleed neighbouring characters together",
    "engine/src/renderer/jce_texture.c":
        "the implementation itself",
}

CALL = re.compile(r"\bjce_texture_from_rgba\s*\(")

SEARCH_DIRS = ("engine/src", "editor/src", "caged_kingdom", "space", "tools")
EXTS = (".c", ".cpp", ".cc", ".h", ".hpp")

# Copied SDK trees and build output are artifacts of a source file that this
# script already checks at its origin.  Scanning them reports the same public
# HEADER DECLARATION as a call site, which it is not.
SKIP_PARTS = ("/build/", "/dist/", "/sdk-current/", "/sdk-eval/", "/.git/")


def main() -> int:
    violations = []
    for d in SEARCH_DIRS:
        base = ROOT / d
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix not in EXTS or not path.is_file():
                continue
            rel = path.relative_to(ROOT).as_posix()
            if rel in ALLOWED:
                continue
            if any(p in "/" + rel for p in SKIP_PARTS):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for i, line in enumerate(text.splitlines(), 1):
                stripped = line.lstrip()
                # Skip comments and the _ex declaration/uses: the regex matches
                # a prefix of jce_texture_from_rgba_ex, so exclude it first.
                if stripped.startswith(("*", "//", "/*")):
                    continue
                if not CALL.search(line):
                    continue
                if re.search(r"\bjce_texture_from_rgba_ex\s*\(", line):
                    continue
                violations.append((rel, i, line.strip()))

    if violations:
        print("check_material_texture_sampler: FAIL")
        print()
        print("jce_texture_from_rgba() is CLAMP + mip-0 only. A material texture")
        print("created with it streaks the edge texel across every tile past the")
        print("first and aliases under minification.")
        print()
        print("Use jce_texture_from_rgba_ex(..., JCE_TEX_WRAP) for materials, or")
        print("add the file to ALLOWED in this script with the reason its surface")
        print("is genuinely sampled ~1:1.")
        print()
        for rel, line_no, src in violations:
            print(f"  {rel}:{line_no}: {src}")
        return 1

    print(f"[OK] check_material_texture_sampler: "
          f"{len(ALLOWED)} allowed ~1:1 call site(s), no material misuse")
    return 0


if __name__ == "__main__":
    sys.exit(main())
