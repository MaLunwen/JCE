#!/usr/bin/env python3
"""
check_morph_clip_wired.py — a glTF clip's blendshape weights must reach the
deform, and no headless test can say so.

THE FAILURE THIS GUARDS.  glTF animates blendshapes through a "weights"
animation channel.  This tree imported it (jce_gltf_loader.c), stored it
(JceModelMorphAnim), and exposed it (jce_model_morph_anim_track) -- and that
accessor's only two references in the entire tree were its own definition and
its own prototype.  The scene renderer passed `track = NULL` with a comment
saying a clip channel "can later be sampled", and returned early unless the
entity authored a JceMorphWeights component.

So facial animation baked into a glTF clip -- the primary use of blendshapes
in Unity, UE and Godot -- did not play.  Not badly: at all.  And it reported
nothing, because a blendshape whose weight is ignored and one whose weight is
genuinely 0 produce the same picture.  The only thing that could move a
blendshape was a designer dragging one of at most 16 static sliders.

WHY A GATE AND NOT A TEST.  The wiring lives in sr_resolve_morph_weights,
which needs a JceSceneRenderer, and no test in this suite drives one --
`grep -rl jce_scene_renderer_create tests/` is empty.  The data path IS
asserted (tests/middleware/animation/test_jce_morph.c: the sampled track
combines with an absent component and the old NULL-and-no-component arguments
resolve to all zeros).  What cannot be asserted there is that the renderer
still asks.  This does that, and it is the half that regressed once already.

TWO RULES, each reproducing one half of the original defect:

  1. jce_model_morph_anim_track must have a caller under engine/src outside
     the file that defines it.  Zero callers is the state it shipped in.

  2. jce_morph_resolve_weights must not be called with a literal NULL as its
     FIRST argument (the track) anywhere under engine/src.  That literal was
     the defect: the combine ran, correctly, over nothing.

NOT CHECKED, and said here rather than left to be discovered: that the sampled
track belongs to the clip that is playing.  Matching the wrong animation index
would still animate, and only a renderer-driven test could tell the
difference.
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ENGINE_SRC = REPO_ROOT / "engine/src"

ACCESSOR = "jce_model_morph_anim_track"
DEFINER  = "jce_model.c"          # and its header
COMBINE  = "jce_morph_resolve_weights"

failures = []


def sources():
    for ext in ("*.c", "*.cpp", "*.h"):
        for p in ENGINE_SRC.rglob(ext):
            yield p


def read(p):
    try:
        return p.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def strip_comments(text):
    """Block and line comments out.

    A rule that counted a MENTION would be satisfied by the very comment that
    explains the defect -- this tree has been bitten by exactly that: a gate's
    own docstring entered the identifier set it searched and permanently
    exempted the one name it was written for.
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def main():
    scanned = 0
    callers = []
    null_track = []

    for p in sources():
        src = strip_comments(read(p))
        if not src:
            continue
        scanned += 1
        rel = p.relative_to(REPO_ROOT).as_posix()

        # Rule 1: a caller that is not the definition or the prototype.
        if p.name not in (DEFINER, "jce_model.h"):
            if re.search(r"\b%s\s*\(" % re.escape(ACCESSOR), src):
                callers.append(rel)

        # Rule 2: the combine's first argument may not be a literal NULL.
        for m in re.finditer(r"\b%s\s*\(" % re.escape(COMBINE), src):
            tail = src[m.end():m.end() + 200]
            first = tail.split(",")[0].strip()
            if first in ("NULL", "0", "(const float *)NULL", "nullptr"):
                line = src[:m.start()].count("\n") + 1
                null_track.append("%s:%d" % (rel, line))

    if scanned < 50:
        print("check_morph_clip_wired: FAIL - only %d engine source(s) "
              "scanned; the scan is broken, not the tree." % scanned,
              file=sys.stderr)
        return 1

    if not callers:
        failures.append(
            "%s has NO caller under engine/src outside %s. That is the state "
            "it shipped in: the glTF weights channel was imported, stored and "
            "exposed, and nothing ever sampled it, so a clip-driven "
            "blendshape did not move and nothing said so."
            % (ACCESSOR, DEFINER))

    for site in null_track:
        failures.append(
            "%s: %s is called with a literal NULL track. That literal WAS the "
            "defect -- the combine ran correctly over nothing, and every "
            "weight resolved to the authored value or to zero."
            % (site, COMBINE))

    if failures:
        print("check_morph_clip_wired: FAIL - %d problem(s):" % len(failures),
              file=sys.stderr)
        for f in failures:
            print("  " + f, file=sys.stderr)
        return 1

    print("check_morph_clip_wired: OK (%d engine source(s); %s reached from "
          "%s; no literal-NULL track)"
          % (scanned, ACCESSOR, ", ".join(sorted(callers))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
