#!/usr/bin/env python3
"""
check_config_key_roundtrip.py — a settings file the engine rewrites must not
lose the keys it understands.

THE BUG THIS EXISTS FOR.  jce_config_save() builds ONE fixed snprintf template
and writes it with jce_fs_host_write_all() -- a whole-file truncating write,
not a merge.  jce_config_load() understood 25 keys; the template emitted 19.
The six it did not emit were deleted from the user's jce.ini the first time
anything saved:

    window.title           window.resizable
    renderer.debug_text    renderer.clear_color
    logging.level          logging.colors

And something always saves: the first launch on a machine resolves the
renderer backend and writes it back, which is a whole-file rewrite.  So a user
who had set a log level or a window title lost it on the first run of a build
that remembers its backend, with no message.

WHAT IS CHECKED.  The set of "<section>.<key>" strings jce_config_load's
apply() compares against, versus the "<key> = " lines the save template emits
under their [section] headers.  They must be equal.

  * a key parsed but never written  -> DATA LOSS on the next save
  * a key written but never parsed  -> a setting that cannot be read back

Both directions are reported, because either one means the file is not a
round-trip.

WHY NOT JUST DIFF THE TWO FUNCTIONS BY HAND: because the failure is silent and
one-directional.  Nothing crashes, nothing warns, and the only symptom is a
setting that quietly reverts -- which reads as "I must have forgotten to save".

Usage:
    python tools/lint/check_config_key_roundtrip.py
    python tools/lint/check_config_key_roundtrip.py --list
Exit 0 when the two sides agree, 1 otherwise.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCE = REPO_ROOT / "engine" / "src" / "os" / "core" / "jce_config.c"

# apply() compares the joined "section.key" against string literals.
PARSED = re.compile(r'strcmp\(\s*full\s*,\s*"([a-z_0-9]+\.[a-z_0-9]+)"\s*\)')

# The save template emits "[section]\n" headers and "key = %..." lines, each
# as its own C string literal.
SECTION = re.compile(r'"(?:\\n)?\[([a-z_0-9]+)\]\\n"')
WRITTEN = re.compile(r'"([a-z_0-9]+) = %[^"]*\\n"')

# Keys the loader accepts but the writer must NOT emit, with a reason.
# Keep this empty unless there is a real one: an exemption here is a key the
# user can set and the engine will silently drop.
EXEMPT = {}


def main() -> int:
    if not SOURCE.is_file():
        print("check_config_key_roundtrip: FAIL - %s is missing; the config "
              "implementation moved and this gate no longer sees it."
              % SOURCE.relative_to(REPO_ROOT).as_posix(), file=sys.stderr)
        return 1

    src = SOURCE.read_text(encoding="utf-8", errors="replace")

    parsed = set(PARSED.findall(src))
    if len(parsed) < 5:
        print("check_config_key_roundtrip: FAIL - only %d parsed key(s) found; "
              "apply() no longer matches the shape this gate reads, so it "
              "would pass on an empty set." % len(parsed), file=sys.stderr)
        return 1

    # Walk the save function once, tracking the section each written key is in.
    save_at = src.find("jce_config_save")
    body = src[save_at:] if save_at >= 0 else ""
    written = set()
    section = ""
    for m in re.finditer(r'"(?:\\n)?\[([a-z_0-9]+)\]\\n"|"([a-z_0-9]+) = %[^"]*\\n"',
                         body):
        if m.group(1):
            section = m.group(1)
        elif section:
            written.add("%s.%s" % (section, m.group(2)))
    if len(written) < 5:
        print("check_config_key_roundtrip: FAIL - only %d written key(s) found; "
              "the save template no longer matches the shape this gate reads."
              % len(written), file=sys.stderr)
        return 1

    lost = sorted(k for k in parsed - written if k not in EXEMPT)
    unread = sorted(written - parsed)

    if "--list" in sys.argv:
        print("parsed : %d" % len(parsed))
        print("written: %d" % len(written))
        for k in sorted(parsed | written):
            mark = "both" if k in parsed and k in written else (
                "PARSED-ONLY" if k in parsed else "WRITTEN-ONLY")
            print("  %-32s %s" % (k, mark))

    ok = True
    for k in lost:
        ok = False
        print("  %s is parsed by jce_config_load but never written by "
              "jce_config_save.  The save is a whole-file truncating write, so "
              "the next save DELETES this key from the user's file -- and the "
              "first launch on a machine saves, to remember the resolved "
              "renderer backend.  Emit it in the template."
              % k, file=sys.stderr)
    for k in unread:
        ok = False
        print("  %s is written by jce_config_save but never parsed by "
              "jce_config_load, so it is a setting the user can see in the "
              "file and the engine will never read back."
              % k, file=sys.stderr)
    for k in sorted(EXEMPT):
        if k not in parsed:
            ok = False
            print("  %s is exempted but no longer parsed; drop the exemption."
                  % k, file=sys.stderr)

    if not ok:
        print("check_config_key_roundtrip: FAILED", file=sys.stderr)
        return 1

    print("check_config_key_roundtrip: OK (%d key(s) round-trip through "
          "jce.ini)" % len(parsed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
