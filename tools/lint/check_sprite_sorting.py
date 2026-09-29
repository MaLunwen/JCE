#!/usr/bin/env python3
"""check_sprite_sorting.py -- the four things that made SpriteRenderer inert.

Every one of these was true in the tree at once, and each is silent: nothing
crashes, nothing logs, the sprite just ignores what the author typed.

  1. The batch comparator ordered by TEXTURE HANDLE first and by the authored
     sort key second, so `sortingOrder` was a tiebreak within one atlas and
     the paint order across atlases came from texture LOAD ORDER.

  2. The batch's render state carried BGFX_STATE_WRITE_Z.  An alpha-blended
     quad that writes depth behaves like opaque geometry: the depth buffer
     decided every sprite-vs-sprite question, so the authored order could not
     show up at all except as z-fighting between coplanar quads.  Measured
     2x2 -- blue physically nearer, red farther, orders swapped between runs:

         authored            before              after
         red=10 blue=0    red=0  blue=61775   red=26384 blue=35032
         red=0  blue=10   red=0  blue=61775   red=0     blue=61775

     Before: IDENTICAL both ways.  The number was inert.

  3. Call sites passed a bare `0` as the sort key.  Once the key is a packed
     (layer, order) int, literal 0 decodes as order -32768 -- "draw in front
     of everything", not "unsorted".  Build it with jce_sprite_sort_key().

  4. The batch drew with the MESH program, whose fragment stage takes
     (v_normal, v_texcoord0, v_worldpos) and never reads a vertex colour, so
     SpriteRenderer.color was packed, uploaded and discarded: tinting a sprite
     pure green produced a frame identical to tinting it white, 26384 red
     pixels either way.  fs_textured.sc -- inputs (a_position, a_color0,
     a_texcoord0), body `texel * v_color0` -- is this batch's shader in all
     but name and was already built.  The mesh program also left sprites lit
     by u_lightDir/u_lightColor, which the batch never sets, so a (220,40,40)
     texel reached the screen as (104,19,19), coloured by whatever drew last.

Run standalone or via tools/lint/run_all.py.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BATCH = ROOT / "engine/src/renderer/jce_sprite_batch.c"
DRAW = ROOT / "engine/src/middleware/scene/jce_sr_draw.c"
HEADER = ROOT / "engine/include/jce/renderer/jce_sprite_batch.h"


def _strip_comments(src: str) -> str:
    """Drop comments so a claim in prose cannot satisfy a check about code.

    A gate in this tree was once permanently disarmed because committing it
    put its own docstring into the set it searched.
    """
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def check() -> list[str]:
    fails: list[str] = []

    for p in (BATCH, DRAW, HEADER):
        if not p.exists():
            fails.append("missing source: %s" % p.relative_to(ROOT))
    if fails:
        return fails

    batch = _strip_comments(BATCH.read_text(encoding="utf-8"))
    draw = _strip_comments(DRAW.read_text(encoding="utf-8"))
    header = _strip_comments(HEADER.read_text(encoding="utf-8"))

    # ── 1. comparator: sort key must be consulted BEFORE the texture ──
    m = re.search(r"sprite_cmp\s*\([^)]*\)\s*\{(.*?)\n\}", batch, re.S)
    if not m:
        fails.append("sprite_cmp not found in %s" % BATCH.name)
    else:
        body = m.group(1)
        i_key = body.find("sort_key")
        i_tex = body.find("texture.idx")
        if i_key < 0:
            fails.append("sprite_cmp never mentions sort_key")
        elif i_tex >= 0 and i_tex < i_key:
            fails.append(
                "sprite_cmp compares texture.idx before sort_key -- the "
                "authored sortingOrder becomes a tiebreak within one atlas "
                "and texture load order decides the paint order")
        if "view_dist2" not in body:
            fails.append(
                "sprite_cmp has no view_dist2 tiebreak -- without depth "
                "write, equal-key sprites composite in submission order")

    # ── 2. render state must NOT write depth ──
    m = re.search(r"uint64_t\s+state\s*=(.*?);", batch, re.S)
    if not m:
        fails.append("sprite batch render state not found")
    else:
        state = m.group(1)
        if "BGFX_STATE_WRITE_Z" in state:
            fails.append(
                "sprite batch sets BGFX_STATE_WRITE_Z -- an alpha-blended "
                "batch that writes depth makes sortingOrder unobservable and "
                "lets transparent borders punch holes in what is behind")
        if "BGFX_STATE_DEPTH_TEST" not in state:
            fails.append(
                "sprite batch dropped its depth TEST -- sprites must still "
                "be occluded by the opaque world")

    # ── 3. no bare literal sort keys at any call site ──
    for name, src in (("jce_sr_draw.c", draw),):
        for m in re.finditer(r"jce_sprite_batch_add\s*\(", src):
            # Balance the parens by hand.  A non-greedy .*? stops at the
            # first ")" -- which is the one closing jce_sprite_sort_key(0, 0)
            # -- so the check reported every CORRECT call site as a literal.
            i, depth, comma = m.end(), 1, m.end()
            while i < len(src) and depth:
                if src[i] == "(":
                    depth += 1
                elif src[i] == ")":
                    depth -= 1
                elif src[i] == "," and depth == 1:
                    comma = i + 1          # last TOP-LEVEL comma
                i += 1
            # rsplit(",") alone splits inside jce_sprite_sort_key(0, 0) and
            # hands back "0)" -- which reported every correct call as a bare
            # literal, i.e. the gate was red on arrival for its own bug.
            last = src[comma:i - 1].strip()
            if "jce_sprite_sort_key" not in last:
                fails.append(
                    "%s: jce_sprite_batch_add passes %r as the sort key; "
                    "build it with jce_sprite_sort_key() -- a literal 0 is "
                    "order -32768, which draws in FRONT of everything"
                    % (name, last[:40]))

    # ── 4. the batch must use the textured program ──
    if "jce_renderer_get_program_mesh" in batch:
        fails.append(
            "sprite batch draws with the MESH program, whose fragment stage "
            "reads no vertex colour -- SpriteRenderer.color is uploaded and "
            "discarded, and sprites inherit stale lighting uniforms")
    if "jce_renderer_get_program_textured" not in batch:
        fails.append(
            "sprite batch does not use jce_renderer_get_program_textured")

    # ── 5. the key builder must exist and be the only packer ──
    if "jce_sprite_sort_key" not in header:
        fails.append("jce_sprite_sort_key() missing from the public header")

    return fails


def main() -> int:
    fails = check()
    for f in fails:
        print("FAIL check_sprite_sorting: %s" % f)
    if fails:
        return 1
    print("check_sprite_sorting: OK -- sort key outranks texture, no depth "
          "write, no literal keys, textured program")
    return 0


if __name__ == "__main__":
    sys.exit(main())
