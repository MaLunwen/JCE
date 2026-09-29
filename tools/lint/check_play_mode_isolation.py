#!/usr/bin/env python3
"""
check_play_mode_isolation.py — Play must not leak into what persists.

WHY THIS EXISTS.  Play is a transient sandbox: jce_state_stop() restores the
pre-play snapshot and the playtest is meant to leave no trace.  Two things
persist past it -- the SAVED SCENE FILE and the UNDO STACK -- and both leaked.

MEASURED WHEN THIS GATE LANDED (2026-09-05), both silent, both in the single
most common editor loop (press Play, tune something live, press Stop):

  SAVE.  jce_state_save_scene_file_ex serialised s.scene, which during Play IS
  the live simulated scene.  Ctrl+S wrote physics-settled transforms,
  jce.spawn'd entities and script-mutated values over the authored level; Stop
  then restored the good scene in memory with scene_modified CLEARED, so no
  unsaved-changes prompt fired and the correct version was gone with no
  warning.  The AUTOSAVE timer was already play-gated, with a comment saying
  why ("don't bake play-time mutations into the file") -- the manual save it
  was protecting against never was.

  UNDO.  jce_state_undo/_redo refused to run during Play and their comment
  claimed the mutation stream was "isolated from edit-mode history".  That was
  true of the READ side only: history_begin_edit and history_push_undo_snapshot
  gated on s_history_suspend_depth alone, and jce_editor_play.cpp never raises
  it.  So each live tweak pushed a full play-mode snapshot onto a FIFO-evicting
  stack -- discarding the session's real history -- and left entries that, after
  Stop, restored play state over the authored scene.

Both were one missing condition next to code that already had it.  That is the
shape this gate is for: a rule enforced on one side of a pair.

WHAT THIS CHECKS.  Each guarded function below must mention jce_state_get_play_state
within its body.  That is a coarse test -- it says the question is ASKED, not
that the answer is used correctly -- and it is deliberately coarse: a stricter
parse of C++ control flow would be a second, worse compiler, while "nobody even
asks" is exactly how both defects looked.

Usage:  python tools/lint/check_play_mode_isolation.py
Exit 0 clean, 1 when a guarded function stops asking.
"""

import re
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]

PLAY_QUERY = "jce_state_get_play_state"

# (relative path, function name, why it must ask)
GUARDED = [
    ("editor/src/io/jce_editor_scene_serial.cpp", "jce_state_save_scene_file_ex",
     "saving during Play writes the SIMULATED scene over the authored level, "
     "and Stop then clears scene_modified so no prompt ever fires"),
    ("editor/src/core/jce_editor_history.cpp", "history_production_suspended",
     "history PRODUCTION must stop during Play, or live tweaks evict the "
     "session's real undo history and become undoable over the authored scene "
     "after Stop"),
    ("editor/src/core/jce_editor_history.cpp", "jce_state_undo",
     "undo must not consume play-mode entries"),
    ("editor/src/core/jce_editor_history.cpp", "jce_state_redo",
     "redo must not consume play-mode entries"),
]


def body_of(text: str, fname: str):
    """The braced body of `fname`, or None.

    Finds the definition by name followed by a parameter list and an opening
    brace, then brace-matches.  Good enough for these four; a declaration ends
    in ';' and is skipped."""
    for m in re.finditer(r'\b' + re.escape(fname) + r'\s*\([^;{]*\)\s*\{', text):
        i = text.index("{", m.start())
        depth, j = 0, i
        while j < len(text):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    return text[i:j + 1]
            j += 1
    return None


def main() -> int:
    problems = []
    checked = 0
    for rel, fname, why in GUARDED:
        p = REPO_ROOT / rel
        if not p.is_file():
            problems.append("%s not found -- this gate is blind to %s"
                            % (rel, fname))
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        body = body_of(text, fname)
        if body is None:
            problems.append(
                "%s: could not find the body of %s().  It was renamed, split "
                "or removed -- re-point this gate rather than deleting the "
                "entry, because the rule it enforces did not go away."
                % (rel, fname))
            continue
        checked += 1
        if PLAY_QUERY not in body:
            problems.append(
                "%s: %s() never calls %s.  %s.  Both defects this gate exists "
                "for looked exactly like this: a rule enforced on one side of "
                "a pair, with the other side's comment claiming otherwise."
                % (rel, fname, PLAY_QUERY, why))

    if problems:
        for pr in problems:
            print("  " + pr, file=sys.stderr)
        print("check_play_mode_isolation: FAIL - %d problem(s)." % len(problems),
              file=sys.stderr)
        return 1

    print("check_play_mode_isolation: OK (%d guarded function(s); each asks %s)"
          % (checked, PLAY_QUERY))
    return 0


if __name__ == "__main__":
    sys.exit(main())
