#!/usr/bin/env python3
"""
check_culling_mask_single_source.py — the colour pass and the depth/velocity
prepass must cull on the same value.

THE FAILURE THIS GUARDS.  The camera culling mask (JceCameraComponent
.culling_mask, Unity's Camera.cullingMask) is applied in two places:

  * the colour pass          engine/src/middleware/scene/jce_sr_draw.c
  * the depth/velocity pass  engine/src/middleware/scene/jce_sr_cull.c

The prepass feeds SSAO, SSR and the TAA motion vectors.  When it disagrees
with the colour cull, the affected objects are drawn and TAA-jittered with no
motion vector, and the whole set shimmers every frame -- the exact failure the
`cull_aspect` comment in jce_sr_draw.c records from the last time these two
passes disagreed, that time over the viewport aspect ratio.

The two passes do not share a signature: sr_draw_depth_prepass() has no
JceSceneRenderConfig parameter.  So the mask is copied ONCE per frame into
sr->frame_culling_mask at the top of jce_scene_renderer_render, and both
passes read that.  This gate holds that arrangement in place:

  * JceSceneRenderConfig::camera_culling_mask may be read in exactly one
    place, the assignment to sr->frame_culling_mask;
  * both pass files must read sr->frame_culling_mask.

Reading cfg->camera_culling_mask directly in a pass is how the two drift
apart, and it looks completely reasonable in review.

Usage:
    python tools/lint/check_culling_mask_single_source.py
    python tools/lint/check_culling_mask_single_source.py --list
Exit 0 when the arrangement holds, 1 otherwise.
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

FIELD = "camera_culling_mask"
FRAME = "frame_culling_mask"

# The one translation unit allowed to read the config field.
OWNER = "engine/src/middleware/scene/jce_scene_renderer.c"

# Both passes must read the per-frame copy.
PASSES = (
    "engine/src/middleware/scene/jce_sr_draw.c",
    "engine/src/middleware/scene/jce_sr_cull.c",
)

SCAN_PREFIXES = ("engine/src/", "editor/src/", "tools/", "scripting/")
SOURCE_SUFFIXES = (".c", ".cpp", ".h", ".hpp", ".inc")

# A FIELD ACCESS, not a substring.  The first cut matched the bare name and
# reported three hits on a clean tree -- all of them the Inspector's
# draw_camera_culling_mask() function name.  A gate red on arrival teaches
# nothing, and its two negative controls were meaningless until this was fixed.
ACCESS = re.compile(r"(?:->|\.)\s*" + FIELD + r"\b")

# Hosts legitimately WRITE the config field; only reads are constrained.
WRITE = re.compile(r"(?:->|\.)\s*" + FIELD + r"\s*=(?!=)")


def strip_comments(text: str) -> str:
    """Blank /* */ and // regions, preserving line structure.

    Every comment in this area names both identifiers on purpose -- they
    explain the arrangement -- so a scan that did not strip them would report
    its own documentation.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(c if c == chr(10) else " " for c in text[i:end]))
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
        print("check_culling_mask_single_source: FAIL - only %d tracked "
              "source file(s); the tree shape changed and this gate no longer "
              "sees the code." % len(files), file=sys.stderr)
        return 1

    stray = []
    owner_reads = 0
    for rel in files:
        try:
            code = strip_comments((REPO_ROOT / rel).read_text(
                encoding="utf-8", errors="replace"))
        except OSError:
            continue
        if not ACCESS.search(code):
            continue
        for i, line in enumerate(code.splitlines(), start=1):
            if not ACCESS.search(line):
                continue
            if WRITE.search(line):
                continue                      # a host filling the config
            if rel == OWNER:
                owner_reads += 1
                continue
            stray.append((rel, i))

    if owner_reads == 0:
        print("check_culling_mask_single_source: FAIL - %s no longer reads "
              "%s, so nothing copies it into %s and both passes are culling "
              "on a stale zero." % (OWNER, FIELD, FRAME), file=sys.stderr)
        return 1

    missing = []
    for rel in PASSES:
        try:
            code = strip_comments((REPO_ROOT / rel).read_text(
                encoding="utf-8", errors="replace"))
        except OSError:
            code = ""
        if FRAME not in code:
            missing.append(rel)

    if "--list" in sys.argv:
        print("%s reads inside %s: %d" % (FIELD, OWNER, owner_reads))
        for rel in PASSES:
            print("pass %-56s reads %s: %s"
                  % (rel, FRAME, rel not in missing))
        for rel, line in stray:
            print("STRAY %s:%d" % (rel, line))

    ok = True
    for rel, line in stray:
        ok = False
        print("  %s:%d reads %s directly.  Only %s may: the depth/velocity "
              "prepass has no config parameter, so the two passes agree only "
              "because both read sr->%s, copied once per frame.  A pass that "
              "reads the config itself is how they drift -- and a prepass that "
              "disagrees with the colour cull draws objects with no motion "
              "vector, which shimmers every frame."
              % (rel, line, FIELD, OWNER, FRAME), file=sys.stderr)
    for rel in missing:
        ok = False
        print("  %s no longer reads sr->%s, so it is not applying the camera "
              "culling mask at all while the other pass is.  Both passes must "
              "cull identically." % (rel, FRAME), file=sys.stderr)

    if not ok:
        print("check_culling_mask_single_source: FAILED", file=sys.stderr)
        return 1

    print("check_culling_mask_single_source: OK (%s read only in %s; both "
          "culling passes read sr->%s)" % (FIELD, OWNER, FRAME))
    return 0


if __name__ == "__main__":
    sys.exit(main())
