#!/usr/bin/env python3
"""
check_component_authoring_surface.py — a component that can be SAVED must be
addable through the generic component API.

WHY THIS EXISTS.  This engine is meant to be driven from outside the editor: a
user project, one of the seven script bindings, or a CLI that authors a scene
through the public API.  None of those can call jce_scene_set_mesh_renderer by
name for a component they learned about at runtime -- they go through the
registry's type-erased pair:

    jce_scene_set_comp(scene, entity, comp_id, &payload)
    jce_scene_get_comp(scene, entity, comp_id, &size)

A registry row whose SER (serializer) is non-NULL is a component the engine
will write into a scene file.  If that same row has no GET/SET, then a generic
authoring path can READ such a component out of a file and never ADD one --
and nothing says so.  The failure is quiet in the worst way: the CLI writes
the scene, the component simply is not there, and the first sign is a designer
opening the scene and finding it empty.

WHAT IT CHECKS.  Every REG(...) row in the scene component registry:

    SER != NULL  and  (GET == NULL or SET == NULL)  ->  FAIL

unless the row is on the EXEMPT list below, where each name carries the reason
it is there.  Rows with SER == NULL are retired migration-only rows (their own
comments say so) and are not required to be settable.

WHY A LINT AND NOT ONLY A TEST.  The runtime sweep that found this question
(tests/middleware/scene/test_jce_authoring_surface_sweep.c) walks the live
registry and is the stronger check -- but tests/ is gitignored on this branch,
so it does not travel with a clone.  This reads the registry SOURCE, which is
tracked, so the guarantee survives the checkout.

USAGE
    python tools/lint/check_component_authoring_surface.py        # gate 0/1
    python tools/lint/check_component_authoring_surface.py --list # rows only
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]
REGISTRY = (REPO_ROOT / "engine" / "src" / "middleware" / "scene"
            / "jce_scene_components_json.c")

# Rows that serialise but deliberately have no type-erased accessor.  NAMED,
# with the reason, because a bare count is what lets the next one in.
EXEMPT = {
    "Light": "unified light row: one registry entry over three light "
             "component types, so there is no single struct to copy "
             "(jce_component_registry.h says so in its own contract)",
}
# Tag and Layer were on this list on the first draft and the stale-exemption
# check below rejected them on its first run: they have SER == NULL, so the
# rule never applied and the permission was decoration.  A standing exemption
# nobody uses is how the next real one gets waved through.

# REG(NAME, A0, A1, A2, FLAG, HAS, REMOVE, PARSE, SER, GET, SET, SIZE)
REG_RE = re.compile(
    r"\bREG\(\s*\"(?P<name>[^\"]+)\"\s*,"      # NAME
    r"(?P<rest>(?:[^();]|\([^()]*\))*)\)\s*;", # the remaining 11 args
    re.S)


def split_args(rest: str) -> list[str]:
    """Split the macro's remaining arguments on top-level commas."""
    out, depth, cur = [], 0, ""
    for ch in rest:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    out.append(cur.strip())
    return out


def rows():
    """[(name, ser, get, set)] for every REG() row in the registry."""
    if not REGISTRY.is_file():
        return None
    text = REGISTRY.read_text(encoding="utf-8", errors="replace")
    found = []
    for m in REG_RE.finditer(text):
        args = split_args(m.group("rest"))
        # rest = A0, A1, A2, FLAG, HAS, REMOVE, PARSE, SER, GET, SET, SIZE
        if len(args) < 11:
            continue
        ser, get, setter = args[7], args[8], args[9]
        found.append((m.group("name"), ser, get, setter))
    return found


def self_test() -> None:
    """Runs every time.  A parser that silently matches nothing reports a
    clean sheet, which is the one result this gate must never invent."""
    sample = ('REG("Thing", "thing", NULL, NULL,\n'
              '    JCE_COMP_FLAG_THING,\n'
              '    jce_scene_has_thing, jce_scene_remove_thing,\n'
              '    parse_thing, serw_thing,\n'
              '    reg_get_thing, reg_set_thing, sizeof(JceThing));\n')
    m = REG_RE.search(sample)
    assert m is not None, "self-test: the REG regex matched nothing"
    args = split_args(m.group("rest"))
    assert m.group("name") == "Thing", "self-test: name"
    assert args[7] == "serw_thing", "self-test: SER position moved: %r" % args
    assert args[8] == "reg_get_thing", "self-test: GET position moved"
    assert args[9] == "reg_set_thing", "self-test: SET position moved"

    retired = ('REG("Old", "old", NULL, NULL,\n'
               '    JCE_COMP_FLAG_OLD,\n'
               '    jce_scene_has_old, jce_scene_remove_old,\n'
               '    parse_old_migrate, NULL,\n'
               '    NULL, NULL, 0);\n')
    a2 = split_args(REG_RE.search(retired).group("rest"))
    assert a2[7] == "NULL", "self-test: a retired row must read SER as NULL"


def main() -> int:
    self_test()

    found = rows()
    if found is None:
        print("check_component_authoring_surface: FAIL - %s not found"
              % REGISTRY, file=sys.stderr)
        return 1
    if len(found) < 50:
        # The registry has ~90 rows; a regex that stops matching would
        # otherwise report a clean sheet.
        print("check_component_authoring_surface: FAIL - only %d registry "
              "row(s) parsed; the REG() shape changed and this gate stopped "
              "seeing the file" % len(found), file=sys.stderr)
        return 1

    if "--list" in sys.argv:
        for name, ser, get, setter in found:
            print("  %-28s ser=%-24s get=%-24s set=%s"
                  % (name, ser, get, setter))
        return 0

    bad, exempt_hit = [], []
    for name, ser, get, setter in found:
        if ser == "NULL":
            continue                      # retired / migration-only row
        if get != "NULL" and setter != "NULL":
            continue
        if name in EXEMPT:
            exempt_hit.append(name)
            continue
        bad.append(name)

    stale = sorted(set(EXEMPT) - set(exempt_hit))
    if stale:
        print("check_component_authoring_surface: FAIL - exempt but no longer "
              "needs to be: %s.  Remove it from EXEMPT rather than leaving a "
              "standing permission nobody uses." % ", ".join(stale),
              file=sys.stderr)
        return 1

    if bad:
        print("check_component_authoring_surface: FAIL - these components are "
              "SERIALISED but cannot be added through jce_scene_set_comp, so "
              "a script, an SDK consumer or a CLI can read them out of a "
              "scene file and never write one: %s" % ", ".join(sorted(bad)),
              file=sys.stderr)
        print("  Give the row a get/set pair, or add it to EXEMPT WITH THE "
              "REASON it cannot have one.", file=sys.stderr)
        return 1

    print("check_component_authoring_surface: OK (%d registry row(s); every "
          "serialised component is addable through the generic API, %d "
          "exempt with a stated reason)" % (len(found), len(exempt_hit)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
