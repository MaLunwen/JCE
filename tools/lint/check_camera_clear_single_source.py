#!/usr/bin/env python3
"""
check_camera_clear_single_source.py — the shipped exe and the Game View must
answer Camera.clearFlags the same way, and only one place may answer it.

THE FAILURE THIS GUARDS, IN TWO HALVES.

(1) A HOST THAT STOPS APPLYING IT.  JceCameraComponent.clear_mode is carried
out of the resolved primary camera on JceSceneCameraPose and turned into
JceSceneRenderConfig.draw_skybox by both hosts.  Delete either host's line and
NOTHING else notices: the component still parses, the pose still carries the
value, the mapping function still returns the right answer, and a unit test of
either of those stays green.  That is the built-but-unwired shape this
campaign keeps finding -- jce_scene_camera_apply_primary itself sat with zero
callers in the entire tree until 694b77d3, while every one of its own
behaviours was correct.

(2) A SECOND ANSWER TO THE SAME QUESTION.  The enum has four values and TWO
questions to ask about them -- does the sky draw, and what does the target
keep from last frame -- so there are two mapping functions and each must be
the only place its half becomes behaviour.  If a host open-codes
`clear_mode == 0` instead of asking, the decision exists in one place and not
the other, and the day one of them changes only one host moves.  The tree has
this exact scar twice over: the GLSL profile table written three times, and
the two "Layers" panels editing two files.

The "keeps" half is the newer one and the more dangerous to leave unguarded,
because its failure is INVISIBLE.  A host that stops passing the keeps still
renders a correct picture -- just always the fully-cleared one.  Nothing looks
broken; the dropdown simply goes back to deciding nothing, which is the state
this whole campaign exists to find.

WHAT IS CHECKED.
  Rule 1  Each mapping function is DEFINED in exactly one TU.
  Rule 2  Both hosts call BOTH of them:
            engine/include/jce/application/jce_default_main.inc.h  (shipped)
            editor/src/scene/jce_editor_game_render.cpp            (Game View)
  Rule 3  Both hosts reach the sink each answer is for -- .draw_skybox for the
          sky half, jce_offscreen_target_prepare_keep for the keeps half --
          so the answer reaches the renderer instead of being computed and
          dropped.
  Rule 4  Outside that one TU, nothing compares clear_mode to a literal --
          the mapping is the only place the enum becomes BEHAVIOUR.  The
          serializers are exempt for the reason the name gives: they turn the
          enum into BYTES, and `if (c->clear_mode != 0)` there is the standard
          omit-when-default guard, identical to the stack_index line above it.
          Confirmed rather than assumed: it was this rule's first and only hit
          on a clean tree, at jce_scene_components_json.c:1468.

WHAT IS NOT CHECKED.  That the renderer honours draw_skybox; that is
jce_scene_renderer.c's single read of it, covered by its own tests.  Nor that
the FRESH-TARGET override still fires -- that lives in jce_offscreen_target.c
and is the reason the keeps are safe to expose at all.  And this is a text
scan: it sees a call, not whether the call is reachable.

Usage:
    python tools/lint/check_camera_clear_single_source.py
    python tools/lint/check_camera_clear_single_source.py --list
Exit 0 when the arrangement holds, 1 otherwise.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

OWNER = "engine/src/middleware/scene/jce_scene_camera.c"

# The two halves of Camera.clearFlags -- one row per QUESTION the enum answers,
# so a third question means adding a row rather than editing the checks.
MAP_FN = "jce_scene_camera_clear_draws_skybox"
KEEPS_FN = "jce_scene_camera_clear_keeps"

HOSTS = (
    ("engine/include/jce/application/jce_default_main.inc.h",
     "the shipped game -- both its draw paths build their config from "
     "s_default_scene_cfg()"),
    ("editor/src/scene/jce_editor_game_render.cpp",
     "the editor Game View -- the runtime-parity surface; the Scene View "
     "deliberately keeps the Show > Skybox flag instead"),
)

SCAN_PREFIXES = ("engine/src/", "engine/include/", "editor/src/")
SOURCE_SUFFIXES = (".c", ".cpp", ".h", ".hpp", ".inc")

def _define_re(fn):
    return re.compile(r"^\s*(?:bool|void)\s+" + fn + r"\s*\(", re.M)


def _call_re(fn):
    return re.compile(r"\b" + fn + r"\s*\(")
SINK_WRITE = re.compile(r"(?:->|\.)\s*draw_skybox\s*=(?!=)")
# The keeps reach the renderer only through the prepare that takes them; a host
# that computes them and then calls plain prepare() has dropped the answer.
KEEPS_SINK = re.compile(r"\bjce_offscreen_target_prepare_keep\s*\(")

HALVES = (
    (MAP_FN, SINK_WRITE, "assigns .draw_skybox",
     "a host that stops applying the authored clear mode breaks nothing "
     "visible to any test: the component still parses, the pose still carries "
     "the value, and the mapping still returns the right answer to nobody"),
    (KEEPS_FN, KEEPS_SINK, "calls jce_offscreen_target_prepare_keep",
     "a host that stops passing the keeps still renders a correct picture -- "
     "just always the fully-cleared one, so nothing looks broken and the "
     "dropdown goes back to deciding nothing"),
)
# `clear_mode == 0` / `!= 1` / `< 2` -- a host deciding for itself.
LITERAL_CMP = re.compile(r"(?:->|\.)\s*clear_mode\s*(?:==|!=|<|>|<=|>=)\s*\d")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def sources() -> dict:
    out = {}
    for prefix in SCAN_PREFIXES:
        root = REPO_ROOT / prefix
        if not root.is_dir():
            continue
        for p in sorted(root.rglob("*")):
            if p.suffix.lower() not in SOURCE_SUFFIXES or not p.is_file():
                continue
            rel = p.relative_to(REPO_ROOT).as_posix()
            out[rel] = strip_comments(p.read_text(encoding="utf-8",
                                                  errors="replace"))
    return out


def main() -> int:
    src = sources()
    if len(src) < 200:
        print("check_camera_clear_single_source: FAIL - only %d source file(s) "
              "scanned; a silently empty scan would pass every rule"
              % len(src), file=sys.stderr)
        return 1

    problems = []

    for fn, sink_re, sink_name, blindness in HALVES:
        define_re, call_re = _define_re(fn), _call_re(fn)
        definers = sorted(rel for rel, t in src.items() if define_re.search(t))
        if definers != [OWNER]:
            problems.append(
                "%s must be defined in exactly one TU (%s); found: %s.  Two "
                "definitions mean two answers to what the four enum values do, "
                "and the day one of them changes only one host moves."
                % (fn, OWNER, definers or "none"))

        for rel, why in HOSTS:
            text = src.get(rel)
            if text is None:
                problems.append("%s is missing; it is %s." % (rel, why))
                continue
            if not call_re.search(text):
                problems.append(
                    "%s never calls %s.  It is %s.  %s."
                    % (rel, fn, why, blindness))
            if not sink_re.search(text):
                problems.append(
                    "%s never %s, so whatever %s resolves cannot reach the "
                    "renderer.  It is %s." % (rel, sink_name, fn, why))

    for rel, text in sorted(src.items()):
        if rel == OWNER or "jce_scene_components_" in rel:
            continue
        m = LITERAL_CMP.search(text)
        if m:
            line = text[:m.start()].count("\n") + 1
            problems.append(
                "%s:%d compares clear_mode to a literal.  Ask %s or %s "
                "instead: the enum answers two separate questions and each "
                "answer must exist in exactly one place, or a host starts "
                "deciding for itself." % (rel, line, MAP_FN, KEEPS_FN))

    if "--list" in sys.argv:
        print("owner  : %s" % OWNER)
        for fn, sink_re, sink_name, _ in HALVES:
            print("map fn : %s  -> %s" % (fn, sink_name))
            for rel, _why in HOSTS:
                t = src.get(rel, "")
                print("  host : %-56s call=%s sink=%s"
                      % (rel, bool(_call_re(fn).search(t)),
                         bool(sink_re.search(t))))

    if problems:
        for p in problems:
            print("  " + p, file=sys.stderr)
        print("check_camera_clear_single_source: FAIL - %d problem(s)."
              % len(problems), file=sys.stderr)
        return 1

    print("check_camera_clear_single_source: OK (%d mapping function(s), each "
          "defined once in %s; both hosts call each and reach its sink)"
          % (len(HALVES), OWNER))
    return 0


if __name__ == "__main__":
    sys.exit(main())
