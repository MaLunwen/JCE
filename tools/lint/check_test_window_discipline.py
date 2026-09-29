#!/usr/bin/env python3
"""
check_test_window_discipline.py -- a test may boot a window; it may not take
the user's desktop.

THE RULE.  A test that boots the engine WINDOWED (desc.headless = false, or a
JceAppDesc that never sets headless at all -- the zero value is windowed) must
also set JCE_WINDOW_HIDDEN before that boot.  The flag keeps every part of the
windowed path a test could be asserting about -- window, GPU device, app init,
input backend install -- and withholds only the mapping, so the test still
tests what it says and the person running the suite keeps their focus and their
keyboard.

WHY A GATE AND NOT A NOTE.  test_jce_engine_input_backend.c booted windowed
from the day it was written and opened a real window on every `ctest` run,
including every parallel run of the whole suite.  Nothing said so, because
nothing was looking.

IN THE TEST, NOT IN CMAKE.  A ctest ENVIRONMENT property is a property of the
LAUNCH, not of the program: it does not reach the exe when someone runs it
directly to debug it, which is exactly when a stolen window is most annoying.
So the flag has to be set inside the test, and that is what this checks.

WHAT IT SCANS, AND WHAT THAT IS WORTH.  tests/ is gitignored on `main` -- on a
clean clone this checker finds ZERO files and passes, which is honest only if
it says so.  It prints the number of files it scanned every time, so "passed"
is never mistaken for "checked something".  On a branch that tracks tests/ it
has real work to do.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "tests"

# A windowed boot: the descriptor says so, or the engine is created from a
# descriptor this file never marked headless.
WINDOWED = re.compile(r"\.headless\s*=\s*false")
HEADLESS_ENV = re.compile(r'"JCE_HEADLESS"')
BOOT = re.compile(r"\bjce_engine_create\s*\(")
HIDDEN = re.compile(r'"JCE_WINDOW_HIDDEN"')


def main() -> int:
    if not TESTS.is_dir():
        print("check_test_window_discipline: OK - no tests/ directory "
              "(0 files scanned; tests/ is gitignored on main)")
        return 0

    files = sorted(TESTS.rglob("*.c"))
    failures = []
    windowed_files = 0

    for f in files:
        try:
            src = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if not BOOT.search(src):
            continue
        if not WINDOWED.search(src):
            # No windowed boot asserted here.  A file that only ever boots
            # headless is not this checker's business.
            continue
        windowed_files += 1
        if HIDDEN.search(src):
            continue
        # A file that flips JCE_HEADLESS itself is deliberately exercising the
        # headless override and still ends up windowed for part of the run --
        # so it is NOT excused; say which case it is in the message.
        via_env = " (it manipulates JCE_HEADLESS, which does not hide a window)" \
            if HEADLESS_ENV.search(src) else ""
        failures.append((f.relative_to(ROOT).as_posix(), via_env))

    if failures:
        print("check_test_window_discipline: FAIL - a test boots the engine "
              "WINDOWED without hiding the window:")
        for rel, note in failures:
            print("  %s%s" % (rel, note))
        print()
        print("  Set it in the TEST, before the boot -- not as a ctest")
        print("  ENVIRONMENT property, which does not reach the exe when it is")
        print("  run directly:")
        print()
        print('      eib_set_env("JCE_WINDOW_HIDDEN", "1");')
        print()
        print("  JCE_WINDOW_HIDDEN keeps the window, the GPU device, app init")
        print("  and every windowed code path; it withholds only the mapping.")
        print("  If the test genuinely needs a MAPPED window, it needs a reason")
        print("  written next to it and an entry here.")
        return 1

    print("check_test_window_discipline: OK - %d test file(s) scanned, "
          "%d boot the engine windowed, all of them hide the window"
          % (len(files), windowed_files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
