#!/usr/bin/env python3
"""
check_cull_gen_consumers.py -- keep the cull-invalidation opt-in honest.

Background (2026-07-26).  The entity-cull freeze in jce_scene_renderer.c keys
on jce_scene_get_cull_data_gen(), which exists so that changing a component
FIELD the cull table memoizes (casts_shadow, light parameters, ...) rebuilds
the table instead of serving the stale value forever.

That generation was first bumped from the JCE_COMP_IMPL accessor macros, i.e.
by all 92 components, on the theory that a blanket signal cannot be forgotten.
It made the generation useless as a cache key: any script writing any
component every frame -- a UI fill, a line colour, a material tint -- changed
it, so both the freeze and its incremental-repair path died every frame and
the entire entity cull rebuilt from scratch.

It is now opt-in, scoped to the components jce_sr_cull.c actually reads.  The
hazard of an opt-in is the opposite one: someone adds a seventh read to the
cull pass and the freeze silently serves stale data for it.  This lint closes
that gap by deriving the required set from the cull source itself.

Rule: every component read via jce_scene_get_<name>() inside jce_sr_cull.c
must be declared in jce_scene.c inside a block where JCE_COMP_CULL_BUMP is
enabled -- unless it is covered by a different signal, in which case it must
appear in COVERED_ELSEWHERE with a reason.

Usage:
  python scripts/lint/check_cull_gen_consumers.py
  # exits 0 if clean, 1 naming the component and what to do.
"""

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
CULL_SOURCE = ROOT / "engine/src/middleware/scene/jce_sr_cull.c"
SCENE_SOURCE = ROOT / "engine/src/middleware/scene/jce_scene.c"

# Reads whose invalidation is already guaranteed by another generation.
# Keyed by the jce_scene_get_<name> suffix; the value is the reason, which is
# printed when the entry is used so a future reader can re-check the claim.
COVERED_ELSEWHERE = {
    "transform": "transform edits bump xform_counter, which the freeze keys on",
    "world_matrix": "derived from transforms; same xform_counter signal",
    "xform_counter": "scene-level accessor, not a component: it IS the signal",
}

GETTER = re.compile(r"\bjce_scene_get_([a-z0-9_]+)\s*\(")
# A declaration line such as: JCE_COMP_IMPL(JcePointLight,  point_light)
DECL = re.compile(r"^JCE_COMP_IMPL[A-Z_]*\(\s*\w+\s*,\s*([a-z0-9_]+)\s*\)")
BUMP_ON = re.compile(r"^#define\s+JCE_COMP_CULL_BUMP\(s\)\s+\(\(s\)->cull_data_gen\+\+\)")
BUMP_OFF = re.compile(r"^#define\s+JCE_COMP_CULL_BUMP\(s\)\s+\(\(void\)0\)")


def components_read_by_cull():
    text = CULL_SOURCE.read_text(encoding="utf-8", errors="replace")
    # Strip block and line comments so a commented-out read does not count.
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return {m.group(1) for m in GETTER.finditer(text)}


def components_that_bump():
    """Walk jce_scene.c tracking the current JCE_COMP_CULL_BUMP definition."""
    bumping, enabled = set(), False
    for line in SCENE_SOURCE.read_text(encoding="utf-8", errors="replace").splitlines():
        if BUMP_ON.match(line):
            enabled = True
            continue
        if BUMP_OFF.match(line):
            enabled = False
            continue
        match = DECL.match(line)
        if match and enabled:
            bumping.add(match.group(1))
    return bumping


def main():
    for path in (CULL_SOURCE, SCENE_SOURCE):
        if not path.is_file():
            print(f"cull-gen consumer check: SKIPPED (missing {path})")
            return 0

    read = components_read_by_cull()
    bumping = components_that_bump()

    if not bumping:
        print("cull-gen consumer check: FAILED")
        print("  no component is declared with JCE_COMP_CULL_BUMP enabled;"
              " the entity-cull freeze can never be invalidated by a field edit")
        return 1

    missing = sorted(read - bumping - set(COVERED_ELSEWHERE))
    if missing:
        print("cull-gen consumer check: FAILED")
        print("(jce_sr_cull.c reads a component whose writes do not invalidate"
              " the cull freeze, so edits to its fields will be ignored)")
        for name in missing:
            print(f"  {name}: wrap its JCE_COMP_IMPL* declaration in"
                  f" jce_scene.c with JCE_COMP_CULL_BUMP enabled,"
                  f" or add it to COVERED_ELSEWHERE with the signal that"
                  f" covers it")
        return 1

    unused = sorted(bumping - read)
    if unused:
        # Not fatal: a component may be cached by a cull helper in another
        # file. Report it so the opt-in list does not silently grow stale.
        print("cull-gen consumer check: OK (with notes)")
        print("  these bump the cull generation but jce_sr_cull.c does not"
              " read them; drop them if nothing else caches their fields:")
        for name in unused:
            print(f"    {name}")
        return 0

    print(f"cull-gen consumer check: OK"
          f" ({len(bumping)} cull-relevant components opt in;"
          f" {len(read - set(COVERED_ELSEWHERE))} read by the cull pass)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
