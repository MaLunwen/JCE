#!/usr/bin/env python3
"""
check_entity_tag_layer_single_writer.py — an entity's tag and layer have TWO
storages, and every editor write must reach the engine's one.

THE SHAPE.  JceEditorMeta.tag[64] and .layer are the editor-side copies that
the Hierarchy row and the Inspector combo display and that the scene file
carries.  The ENGINE's authorities are elsewhere:

  * jce_scene_set_entity_tag_name / _get / find_with_tag / find_all_with_tag,
    backed by the process-wide interned tag registry -- what every gameplay
    query answers from;
  * JceLayerComponent, via jce_scene_set_entity_layer -- what the camera
    culling mask reads (694b77d3).

Writing only the editor-side copy leaves a tag that is visible in the
Hierarchy and invisible to the game, and a layer the Inspector shows and the
culling mask ignores.  It survives Play, too: Play does not reload the scene
from disk, so the loader's mirror never runs.

Before this gate, exactly ONE of the three editor write sites remembered to
call the engine (the Inspector's layer combo, which called both by hand).  The
Hierarchy tag menu and jce_state_duplicate_entity did not -- so a duplicated
entity lost its tag AND its layer, silently, while both still showed in the UI.

WHAT IS CHECKED.
  Rule 1  The two editor-state setters mirror into the engine:
            jce_state_set_entity_tag   -> jce_scene_set_entity_tag_name
            jce_state_set_entity_layer -> jce_scene_set_entity_layer
  Rule 2  jce_state_duplicate_entity carries both, since a copy that keeps the
          editor's string and drops the engine's is the same defect one level
          down.
  Rule 3  No OTHER file under editor/src writes meta->tag or meta->layer
          directly -- a bypass of the setters is how a fourth site would join
          the two that already had.

WHAT IS NOT CHECKED.  That the mirrored value is CORRECT; that is the engine
tests' job.  And this is a text scan: it sees a call, not whether it runs.

Usage:
    python tools/lint/check_entity_tag_layer_single_writer.py
    python tools/lint/check_entity_tag_layer_single_writer.py --list
Exit 0 when the arrangement holds, 1 otherwise.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
STATE = "editor/src/core/jce_editor_state.cpp"

# function name -> the engine call its body must contain
MIRRORS = {
    "jce_state_set_entity_tag":   ["jce_scene_set_entity_tag_name"],
    "jce_state_set_entity_layer": ["jce_scene_set_entity_layer"],
    "jce_state_duplicate_entity": ["jce_scene_set_entity_tag_name",
                                   "jce_scene_set_entity_layer"],
}

# A direct write to the editor-side copy.
#
# SCOPED BY THE DECLARED VARIABLE, never by field name alone.  The first
# version matched `->tag =` anywhere and reported jce_panel_memory_profiler.cpp
# writing `st.rows[i].tag = i` -- the memory profiler's own allocation-tag
# enum, nothing to do with an entity.  That is the same collision every other
# gate in this tree exists to avoid, and this rule found it on its first run.
DECL = re.compile(r"JceEditorMeta\s*\*\s*(?:const\s+)?([A-Za-z_]\w*)")


def direct_writes(text: str):
    """[(line, var)] where a JceEditorMeta* has its tag or layer written."""
    out = []
    for var in set(DECL.findall(text)):
        v = re.escape(var)
        pat = re.compile(r"\b" + v + r"\s*->\s*(?:tag|layer)\s*=(?!=)"
                         r"|snprintf\s*\(\s*" + v + r"\s*->\s*tag\s*,")
        for m in pat.finditer(text):
            out.append((text[:m.start()].count(chr(10)) + 1, var))
    return sorted(out)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def function_body(text: str, name: str) -> str | None:
    """Body of a top-level C function, by brace matching from its header."""
    m = re.search(r"^[\w:<>,\s\*&]*?\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{",
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
    return text[m.end():i]


def main() -> int:
    path = REPO_ROOT / STATE
    if not path.is_file():
        print("check_entity_tag_layer_single_writer: FAIL - missing %s" % STATE,
              file=sys.stderr)
        return 1
    state = strip_comments(path.read_text(encoding="utf-8", errors="replace"))

    problems = []
    for fn, needs in MIRRORS.items():
        body = function_body(state, fn)
        if body is None:
            problems.append(
                "%s: %s not found.  The mirror rule cannot hold a function "
                "that is not there, and a silently absent one is how this "
                "gate would pass while the defect returns." % (STATE, fn))
            continue
        for call in needs:
            if call not in body:
                problems.append(
                    "%s: %s does not call %s.  The editor-side copy would be "
                    "written and the engine's authority left stale -- a tag "
                    "the Hierarchy shows and gameplay cannot find, or a layer "
                    "the Inspector shows and the culling mask ignores."
                    % (STATE, fn, call))

    # Rule 3: no other editor file writes the mirrored fields directly.
    for p in sorted((REPO_ROOT / "editor/src").rglob("*.cpp")):
        rel = p.relative_to(REPO_ROOT).as_posix()
        if rel == STATE:
            continue
        text = strip_comments(p.read_text(encoding="utf-8", errors="replace"))
        for line, _var in direct_writes(text):
            problems.append(
                "%s:%d writes the editor-side tag/layer directly.  Go through "
                "jce_state_set_entity_tag / jce_state_set_entity_layer, which "
                "mirror into the engine -- a site that writes only the copy is "
                "exactly the two sites this gate was written for."
                % (rel, line))

    if "--list" in sys.argv:
        for fn, needs in MIRRORS.items():
            body = function_body(state, fn) or ""
            for call in needs:
                print("%-32s -> %-34s %s"
                      % (fn, call, "OK" if call in body else "MISSING"))

    if problems:
        for p_ in problems:
            print("  " + p_, file=sys.stderr)
        print("check_entity_tag_layer_single_writer: FAIL - %d problem(s)."
              % len(problems), file=sys.stderr)
        return 1

    print("check_entity_tag_layer_single_writer: OK (%d editor setter(s) "
          "mirror into the engine; no direct writes elsewhere)" % len(MIRRORS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
