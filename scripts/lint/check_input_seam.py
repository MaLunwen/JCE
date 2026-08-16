#!/usr/bin/env python3
"""Exactly one input translation unit is allowed to speak SDL.

Input used to be a wall.  `jce_input_handle_event(JceInput *, const void *)`
took an SDL_Event, walked a switch, and mutated private state -- with SDL on
one side and the state machine on the other and no seam between them.  The
consequence was measured, not guessed: wrapping the ENTIRE SDL gamepad dispatch
in `#if 0` left all four input tests green, 24/24 assertions passing while the
code that reads a controller was deleted.  Nothing could reach it, so nothing
tested it.

The split that fixed it only stays fixed if SDL stays on one side of it:

  * engine/src/os/platform/jce_input_sdl.c  -- the translator and the device
    backend.  The ONLY input TU that includes SDL, and the only place a JCE
    code is produced from an SDL code by a cast.
  * everything else -- the state machine (jce_input.c), the action evaluator
    (jce_input_actions.c), the recorder (jce_input_record.c), and every public
    input header.  These must be reachable, and testable, with nothing plugged
    in and no SDL_Init anywhere.

One `#include <SDL3/SDL.h>` back in jce_input.c would look completely
reasonable in review -- it is one line, and it compiles -- and it would rebuild
the wall a piece at a time.  So this is a gate rather than a convention.

Two rules:

  1. No SDL token in any scanned input file except jce_input_sdl.c.
  2. jce_input_sdl.c MUST still contain SDL.  A translator that has quietly
     stopped speaking SDL is not a success; it is a file that no longer does
     its job, and rule 1 alone would applaud it.

The scan set is DERIVED, not listed (see SCAN_DIRS): every .c/.h under the two
platform directories whose name looks like input or whose code names the input
vocabulary.  A name-prefix list would only ever catch violations in files it
already knew about -- and raw joysticks are a planned later batch, so the file
that most needs catching is the one that does not exist yet.

Comments and string literals are stripped before scanning, deliberately: the
headers describe SDL behaviour in prose ("values match SDL3 scancodes"), and
prose is documentation, not a dependency.  `jce_input_sdl` as an identifier is
likewise not a match -- the pattern is the `SDL_` / `SDL3/` namespace, not the
letters.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The one file allowed to speak SDL, and required to.
SDL_OWNER = "engine/src/os/platform/jce_input_sdl.c"

# The scan set is chosen by DIRECTORY + PURPOSE, never by name prefix.
#
# A `jce_input*` glob would have been simpler and would have been wrong in the
# way that matters: it catches violations in files it already knows about, and
# says nothing about knowing about the right files.  Raw joysticks are an
# explicitly planned later batch, and Plan B adds device files -- a
# `jce_joystick.c` or `jce_devices.c` written tomorrow would have landed
# OUTSIDE a name-prefix gate on the day it was created, which is precisely the
# silence this gate exists to prevent.
#
# So: every .c/.h under the platform directories, and a file is IN if its name
# looks like input OR its code names the input vocabulary.  A new input file is
# inside the gate before anyone remembers this file exists.
SCAN_DIRS = [
    "engine/src/os/platform",
    "engine/include/jce/os/platform",
]
SUFFIXES = {".c", ".h"}

# Joined by NAME: catches a brand-new file on its first commit, before it has
# grown a single JCE type -- e.g. a jce_joystick.c that is nothing but an SDL
# include and a TODO.
INPUT_NAME_RE = re.compile(
    r"^jce_(input|joystick|gamepad|keys|keyboard|mouse|touch|controller|"
    r"haptic|rumble|hid)(_|$)"
)

# Joined by PURPOSE: catches a file whose name says nothing (jce_devices.c,
# jce_hotplug.c) but which handles input state or input codes.
INPUT_VOCAB_RE = re.compile(
    r"\bJceInput\w*|\bjce_input_\w+|\bJCE_INPUT_\w+|"
    r"\bJCE_KEY_\w+|\bJCE_GAMEPAD_\w+|\bJCE_MOUSE_BUTTON_MASK\b|"
    r"\bJceKey\b|\bJceGamepad\w*|\bJceFingerID\b|"
    r"\bJceJoystick\w*|\bjce_joystick_\w+"
)

# Platform files that name the input vocabulary but are deliberately NOT part
# of the seam.  Each needs a written reason, and the count is printed on every
# run so the list cannot grow quietly.  Empty today, and that is the point:
# every platform file that touches input is inside the gate.
NOT_INPUT = {
    # "engine/src/os/platform/example.c": "reason it is not an input file",
}

# `SDL_Event`, `SDL_SCANCODE_A`, `<SDL3/SDL.h>`, `SDL_BUTTON_MASK`.  Not
# `jce_input_sdl_translate` (lowercase, and no word boundary before `SDL`).
SDL_TOKEN_RE = re.compile(r"\bSDL_[A-Za-z0-9_]*|\bSDL[0-9]+/")

OPT_OUT = "jce-input-seam-exempt"


def strip_comments_and_strings(src: str) -> str:
    """Blank out /* */, //, "..." and '...' while preserving line count.

    Newlines survive so reported line numbers stay true; everything else in a
    comment or literal becomes a space.
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in src[i:j]))
            i = j
        elif c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in ('"', "'"):
            quote = c
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == quote or src[j] == "\n":
                    break
                j += 1
            j = min(j + 1, n)
            out.append("".join(ch if ch == "\n" else " " for ch in src[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def scanned_files():
    """Every platform .c/.h that participates in input, and why it joined.

    Returns {relpath: (Path, "name"|"vocabulary")}.  Membership is derived, so
    a file created tomorrow is inside the gate tomorrow.
    """
    seen = {}
    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            rel = path.relative_to(ROOT).as_posix()
            if rel in NOT_INPUT:
                continue
            if INPUT_NAME_RE.match(path.stem):
                seen[rel] = (path, "name")
                continue
            code = strip_comments_and_strings(
                path.read_text(encoding="utf-8", errors="replace"))
            if INPUT_VOCAB_RE.search(code):
                seen[rel] = (path, "vocabulary")
    return seen


def main() -> int:
    files = scanned_files()
    failures = []
    exemptions = []

    if SDL_OWNER not in files:
        print("check_input_seam: FAILED\n")
        print(f"  {SDL_OWNER} is missing.  That file IS the seam: it holds the\n"
              f"  pure SDL_Event -> JceInputEvent translator and the device backend.\n"
              f"  Without it the split does not exist.")
        return 1

    for rel, (path, _why) in sorted(files.items()):
        raw = path.read_text(encoding="utf-8", errors="replace")
        code_lines = strip_comments_and_strings(raw).splitlines()
        raw_lines = raw.splitlines()
        hits = [(i + 1, line) for i, line in enumerate(code_lines)
                if SDL_TOKEN_RE.search(line)]

        if rel == SDL_OWNER:
            if not hits:
                failures.append(
                    f"{rel}: contains no SDL token at all.\n"
                    f"    This file is the ONLY one allowed to speak SDL, and it is\n"
                    f"    the one that must: it is the translator.  An empty seam\n"
                    f"    passes rule 1 while doing nothing."
                )
            continue

        for lineno, _ in hits:
            src = raw_lines[lineno - 1] if lineno <= len(raw_lines) else ""
            if OPT_OUT in src:
                exemptions.append(f"{rel}:{lineno}")
                continue
            failures.append(
                f"{rel}:{lineno}: SDL outside the translator.\n"
                f"    {src.strip()}\n"
                f"    Input speaks SDL in exactly one place, {SDL_OWNER}.\n"
                f"    Produce a JceInputEvent there and submit it "
                f"(jce_input_submit); everything\n"
                f"    on this side of the seam must be testable with nothing "
                f"plugged in."
            )

    by_name = sum(1 for _, why in files.values() if why == "name")
    by_vocab = len(files) - by_name

    if failures:
        print("check_input_seam: FAILED\n")
        for f in failures:
            print("  " + f + "\n")
        print(f"  A deliberate exception needs '{OPT_OUT}' on the offending line,\n"
              f"  with a comment saying why the seam does not apply.")
        return 1

    # The counts are printed on every run, pass or fail, so an exemption or a
    # NOT_INPUT entry is a number a reviewer sees drift in the CI log rather
    # than a line nobody reads.  Nothing fails as exemptions accumulate;
    # they merely stop being invisible.
    print(f"check_input_seam: ok — {len(files)} input file(s) scanned "
          f"({by_name} by name, {by_vocab} by vocabulary), "
          f"{len(NOT_INPUT)} declared not-input, "
          f"{len(exemptions)} line exemption(s) in force"
          + (": " + ", ".join(exemptions) if exemptions else "")
          + f"; SDL confined to {SDL_OWNER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
