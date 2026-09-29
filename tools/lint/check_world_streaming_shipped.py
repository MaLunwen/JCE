#!/usr/bin/env python3
"""
check_world_streaming_shipped.py — world streaming must exist outside the editor.

WHY THIS EXISTS.  On 2026-09-01 jce_world_streamer_create had exactly TWO
callers in the whole repository:

    editor/src/core/jce_editor_play.cpp         the Play session
    editor/src/scene/jce_editor_scene_render.cpp  the scene-view preview

and jce_world_streamer_update had the same two.  The shipped runtime created
none.  So a scene whose streaming settings were enabled with N chunks streamed
nothing in a shipped game: the chunks were authored, serialized into the scene
file, and never read.  For a large world that is the entire world.

It is the same shape as every other defect this audit found -- alive in the
editor, dead in the shipped exe -- and invisible for the same reason: the only
configuration in which it is broken is the one nobody opens while authoring.

WHAT THIS CHECKS.

  RULE 1  jce_world_streamer_create has at least one caller under engine/,
          i.e. a path that exists without an editor.

  RULE 2  jce_world_streamer_update likewise.  Creating a streamer and never
          updating it loads the chunks around the origin once and then freezes
          -- a world that streams in and never streams again, which reads as
          "streaming works" for exactly as long as the player stands still.

  RULE 3  Whatever engine file creates it also destroys it.  A streamer that
          outlives its scene holds chunk state for entities that are gone;
          scene transitions run through the same setup path, so the old one
          must die before the new one is built.

WHAT THIS DOES NOT CHECK.  That the chunks actually load, or that the focus
position is the player's.  A text scan sees call sites.  The behavioural claim
needs a scene with authored chunks and a run.

Usage:  python tools/lint/check_world_streaming_shipped.py
Exit 0 clean, 1 on any finding.
"""

import re
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]
ENGINE = REPO_ROOT / "engine" / "src"

SKIP_DIRS = ("third_party", "external", "vendor", "3rdparty")

CREATE = re.compile(r"\bjce_world_streamer_create\s*\(")
UPDATE = re.compile(r"\bjce_world_streamer_update\s*\(")
DESTROY = re.compile(r"\bjce_world_streamer_destroy\s*\(")

# The streamer's own implementation DEFINES these; a definition is not a caller.
IMPL = "engine/src/resource/jce_world_streamer.c"


def strip_comments(text: str) -> str:
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


ENCLOSING = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \t\*]*?\b(\w+)\s*\([^;{]*\)\s*\{",
                       re.M)


def update_is_reached(code: str) -> bool:
    """Is the function holding the _update call itself called anywhere?

    Asking only "does this file contain jce_world_streamer_update" is not
    enough, and the gate's own negative control proved it: deleting the ONE
    call to the tick helper left the helper -- and its _update inside -- sitting
    in the file, and the gate stayed green.  A streamer created and never
    ticked loads the ring around the origin once and then freezes, which reads
    as working for exactly as long as the player stands still.

    So find the function the call sits in and require at least one OTHER
    mention of its name: a definition mentions the name once, a call makes two.
    """
    pos = UPDATE.search(code).start()
    name = None
    for m in ENCLOSING.finditer(code):
        if m.start() > pos:
            break
        name = m.group(1)
    if not name:
        return False
    # Across the ENGINE, not just this file.  The first version looked only at
    # the file holding the call, and extracting the streaming setup into its
    # own translation unit -- which is what the size gate asks for, and what
    # jce_rt_physics.c / jce_rt_script.c / jce_rt_audio.c already do -- moved
    # the caller into jce_runtime.c and turned this rule red on correct code.
    # A per-file reachability rule punishes exactly the refactor the rest of
    # the tree is organised around.
    hits = 0
    for p, _rel in sources():
        raw = p.read_text(encoding="utf-8", errors="replace")
        if name not in raw:
            continue
        hits += len(re.findall(r"\b%s\s*\(" % re.escape(name),
                               strip_comments(raw)))
    return hits >= 2


def sources():
    for pat in ("**/*.c", "**/*.cpp"):
        for p in ENGINE.glob(pat):
            if any(d in p.parts for d in SKIP_DIRS):
                continue
            rel = p.relative_to(REPO_ROOT).as_posix()
            if rel == IMPL:
                continue
            yield p, rel


def main() -> int:
    if not ENGINE.is_dir():
        print("check_world_streaming_shipped: FAIL - engine/src not found",
              file=sys.stderr)
        return 1

    creators, updaters, destroyers = [], [], []
    for p, rel in sources():
        raw = p.read_text(encoding="utf-8", errors="replace")
        if "jce_world_streamer_" not in raw:
            continue
        code = strip_comments(raw)
        if CREATE.search(code):
            creators.append(rel)
        if UPDATE.search(code) and update_is_reached(code):
            updaters.append(rel)
        if DESTROY.search(code):
            destroyers.append(rel)

    problems = []
    if not creators:
        problems.append(
            "no engine-side caller of jce_world_streamer_create.  Streaming "
            "exists only where an editor does, so a shipped game loads none of "
            "the chunks its scene authored -- for a large world, none of the "
            "world")
    if not updaters:
        problems.append(
            "no engine-side caller of jce_world_streamer_update.  A streamer "
            "that is created and never updated loads the ring around the "
            "origin once and then freezes; that reads as working for exactly "
            "as long as the player stands still")
    for c in creators:
        if c not in destroyers:
            problems.append(
                "%s creates a world streamer and never destroys one.  Scene "
                "transitions run through the same setup, so the old streamer "
                "would outlive its scene and hold chunk state for entities "
                "that are gone" % c)

    if problems:
        for p_ in problems:
            print("  " + p_, file=sys.stderr)
        print("check_world_streaming_shipped: FAIL - %d finding(s)." % len(problems),
              file=sys.stderr)
        return 1

    print("check_world_streaming_shipped: OK (create in %s; update in %s; "
          "each creator also destroys)"
          % (", ".join(creators), ", ".join(updaters)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
