#!/usr/bin/env python3
"""
check_light_parser_parity.py — one light, three parsers, one key set.

THE SHAPE.  A light reaches a JCE scene through FOUR parsers in
engine/src/middleware/scene/jce_scene_components_render.c:

    parse_unified_light   the "Light" row -- the ONLY one with a serializer,
                          so it is what the editor writes and reads back
    parse_dir_light       the "DirectionalLight" compat row
    parse_point_light     the "PointLight" compat row
    parse_spot_light      the "SpotLight" compat row

The three concrete rows REG a NULL writer (jce_scene_components_json.c), so
they are READ-ONLY compat paths: nothing in the editor's own round trip ever
exercises them, and no scene in this tree uses those type names.  They exist
for exactly the input nobody was testing -- hand-written scenes, and scenes an
AI CLI or an exporter emits.

WHAT WENT WRONG.  parse_point_light and parse_spot_light read neither
castsShadow nor shadowBias.  Both memset their struct and never looked at the
keys, while the unified parser read both and parse_dir_light read castsShadow.
So `{"type":"PointLight","castsShadow":true}` loaded a light that could not
cast a shadow, silently -- and because only the unified row serializes, opening
that scene and saving it rewrote it as "Light" with castsShadow=false.  The
authored intent was DESTROYED, not merely ignored.

Three parsers for one concept is how they drifted, and nothing was watching.

WHAT IS CHECKED.  Every JSON key the unified parser reads for light type T must
also be read by T's concrete parser.  Keys are the string literals handed to
j_num / j_num2 / j_bool / j_str inside each function body, so this tracks the
code rather than a list someone has to remember to update.

WHAT IS NOT CHECKED, and this was MEASURED rather than assumed: deleting the
unified branch's own shadowBias read leaves this gate GREEN.  The check is
compat-covers-unified, one direction only, so it watches the three parsers
nobody exercises and not the one everybody does -- which is the right way round
(the unified row is the one the editor round-trips, so it fails loudly), but it
is worth knowing before trusting a green run to mean more than it does.

Also not checked.  The reverse direction: a compat parser MAY read keys the
unified one does not (posX/posY/posZ is exactly that -- a light with no
Transform has nowhere else to get a position, and the unified row does not
offer the fallback).  Nor are DEFAULTS compared: parse_spot_light defaults its
cone to 30/45 and the unified branch to 25/35, which is a real divergence but
one that changing would restyle existing content, so it is recorded here and
left alone rather than silently "fixed" by a lint.

EXEMPTIONS are in EXEMPT below, each with the reason it is not a defect.

Usage:
    python tools/lint/check_light_parser_parity.py
    python tools/lint/check_light_parser_parity.py --list
Exit 0 when every concrete parser covers its branch, 1 otherwise.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SRC = "engine/src/middleware/scene/jce_scene_components_render.c"

# Which struct each unified branch builds, and the concrete parser that must
# match it.  The branch is located by the setter it calls, not by a line range:
# a line range is a comment that rots the first time the file is edited.
BRANCHES = [
    ("point", "jce_scene_set_point_light", "parse_point_light", "PointLight"),
    ("spot",  "jce_scene_set_spot_light",  "parse_spot_light",  "SpotLight"),
    ("dir",   "jce_scene_set_dir_light",   "parse_dir_light",   "DirectionalLight"),
]

# key -> why the concrete parser is allowed not to read it.
EXEMPT = {
    "lightType": "the DISPATCH key.  It tells parse_unified_light which of the "
                 "three lights to build; a row already named PointLight has "
                 "nothing to dispatch on.",
    "type":      "the dispatch key's alias, and separately the component type "
                 "name itself -- same reason.",
    "dirX": "direction is meaningless for a point light and comes from the "
            "shared prologue, which runs before the branch.",
    "dirY": "see dirX.",
    "dirZ": "see dirX.",
    "dir_x": "snake alias of dirX.",
    "dir_y": "snake alias of dirY.",
    "dir_z": "snake alias of dirZ.",
    "color_r": "snake alias the unified row accepts via j_num2 and the compat "
               "rows do not.  Accepting fewer spellings of a key that IS read "
               "is not the drift this gate is about -- losing the key is.",
    "color_g": "see color_r.",
    "color_b": "see color_r.",
    "inner_cone_deg": "see color_r -- snake alias only.",
    "outer_cone_deg": "see color_r -- snake alias only.",
}

READER = re.compile(r"\bj_(?:num2?|bool|str)\s*\(")


def function_body(text: str, name: str) -> str | None:
    """Body of a top-level C function, by brace matching from its header."""
    m = re.search(r"^[\w\s\*]*?\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{",
                  text, re.M)
    if not m:
        return None
    i, depth = m.end(), 1
    while i < len(text) and depth:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[m.end():i - 1]


def keys_read(body: str) -> set[str]:
    """Every JSON key literal handed to a j_* reader in `body`.

    j_num2 takes TWO key literals (canonical + snake alias), so the arguments
    are walked rather than the first string taken -- the first version of this
    took only the first literal and would have called the snake aliases absent
    from both sides, which is a gate that agrees with itself and with nothing
    else.
    """
    out = set()
    for m in READER.finditer(body):
        i, depth, args, start = m.end(), 1, [], m.end()
        while i < len(body) and depth:
            ch = body[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    args.append(body[start:i])
                    break
            elif ch == "," and depth == 1:
                args.append(body[start:i])
                start = i + 1
            i += 1
        for a in args[1:3]:              # arg0 is the cJSON*, then the keys
            lit = re.match(r'\s*"([^"]*)"\s*$', a)
            if lit:
                out.add(lit.group(1))
    return out


def branch_body(unified: str, setter: str) -> str | None:
    """The slice of parse_unified_light that ends in `setter`.

    Taken backwards from the setter call to the nearest preceding `{`, which is
    the branch's own block: each branch is exactly one if/else arm ending in
    its jce_scene_set_*_light call.
    """
    i = unified.find(setter)
    if i < 0:
        return None
    depth, j = 0, i
    while j > 0:
        j -= 1
        if unified[j] == "}":
            depth += 1
        elif unified[j] == "{":
            if depth == 0:
                return unified[j + 1:i]
            depth -= 1
    return None


def main() -> int:
    path = REPO_ROOT / SRC
    if not path.is_file():
        print("check_light_parser_parity: FAIL - missing %s" % SRC,
              file=sys.stderr)
        return 1
    text = path.read_text(encoding="utf-8", errors="replace")

    unified = function_body(text, "parse_unified_light")
    if unified is None:
        print("check_light_parser_parity: FAIL - parse_unified_light not "
              "found.  This gate compares against it, so its absence is not a "
              "pass.", file=sys.stderr)
        return 1

    problems, rows = [], []
    for label, setter, concrete, type_name in BRANCHES:
        bb = branch_body(unified, setter)
        cb = function_body(text, concrete)
        if bb is None:
            problems.append("parse_unified_light has no branch calling %s -- "
                            "either the unified parser stopped building %s "
                            "lights, or this gate's anchor is stale."
                            % (setter, label))
            continue
        if cb is None:
            problems.append("%s not found.  A silently absent parser is how "
                            "this gate would pass while the compat type stops "
                            "loading at all." % concrete)
            continue
        # The branch's own keys, plus the prologue's (colour, intensity,
        # castsShadow, direction) which every branch inherits.
        #
        # A THIRD TERM USED TO SIT HERE -- keys_read(everything before the
        # setter) -- and it was wrong in the way that matters: for the LAST
        # branch that slice contains the two branches above it, so the dir row
        # was asked to read radius, innerConeDeg, outerConeDeg, iesPath and
        # shadowBias, and this gate FAILED on its first run for its own bug
        # rather than for a defect.  The prologue term below already covers
        # what the branches share.
        want = keys_read(bb) | keys_read(unified.split("int ltype", 1)[0])
        have = keys_read(cb)
        missing = sorted(k for k in want - have if k not in EXEMPT)
        rows.append((label, concrete, len(want), len(have), missing))
        for k in missing:
            problems.append(
                "%s: '%s' is read by parse_unified_light's %s branch and NOT "
                "by %s.  A scene typed \"%s\" loses it silently, and "
                "because only the unified row has a serializer, saving that "
                "scene from the editor writes the lost value back as a "
                "default -- the authored intent is destroyed, not ignored."
                % (SRC, k, label, concrete, type_name))

    if "--list" in sys.argv:
        for label, concrete, nw, nh, missing in rows:
            print("%-6s %-20s unified reads %2d, %s reads %2d  %s"
                  % (label, concrete, nw, concrete, nh,
                     "MISSING " + ",".join(missing) if missing else "ok"))
        print("\n%d exemption(s):" % len(EXEMPT))
        for k, why in sorted(EXEMPT.items()):
            print("  %-16s %s" % (k, why))

    if problems:
        for p_ in problems:
            print("  " + p_, file=sys.stderr)
        print("check_light_parser_parity: FAIL - %d problem(s)." % len(problems),
              file=sys.stderr)
        return 1

    print("check_light_parser_parity: OK (%d compat parser(s) cover their "
          "unified branch; %d exempted key(s))" % (len(rows), len(EXEMPT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
