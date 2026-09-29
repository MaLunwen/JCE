#!/usr/bin/env python3
"""Every JceLifecycleEvent must have somewhere that emits it.

WHY.  JCE_LIFECYCLE_DEVICE_LOST shipped in the public ABI with ZERO emit
sites.  The whole tracked tree named it four times: the enum declaration, its
case in jce_lifecycle_event_to_string(), its row in contracts/abi-snapshot.txt, and
a table in engine/src/os/platform/AGENTS.md saying "(no SDL counterpart yet);
renderer may emit manually".

A consumer's jce_lifecycle_register(JCE_LIFECYCLE_DEVICE_LOST, ...) returned a
valid handle and the callback was dead code.  Nothing in the engine, the
editor or the log said so -- the authorisation was real and the event never
came.  That is the same silhouette as four authorised WheelColliders nobody
read, and it is worse for a consumer than for us, because the failure is
invisible until the day the device actually goes.

WHAT THIS CHECKS, AND -- MORE IMPORTANTLY -- WHAT IT DOES NOT.

It checks that each enumerator appears as the argument of a jce_lifecycle_emit
call somewhere in engine/ or editor/.  That is exactly the defect above and
nothing more.

IT CANNOT SEE REACHABILITY, and this file is the wrong place to pretend
otherwise.  Measured, in the same investigation: JCE_LIFECYCLE_DEVICE_RESET
HAS an emit site -- `case SDL_EVENT_RENDER_DEVICE_RESET:` in jce_engine.c --
and that site is dead on every real backend, because SDL_EVENT_RENDER_* are
SDL_Render events raised for an SDL_Renderer, and this engine creates one only
in its safe-mode software fallback.  So DEVICE_RESET passes this check today
while being as undeliverable as DEVICE_LOST was.

Saying that out loud is the point.  A gate whose limits are undocumented gets
read as a stronger guarantee than it is, and the next person to add an event
deserves to know that passing here means "somebody wrote an emit", not
"somebody will receive it".

ZERO ENUMERATORS FOUND IS A FAILURE, not a pass: an absence check whose input
came back empty has measured nothing, and this one parses a header that could
be moved or renamed.
"""
from __future__ import annotations

import io
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "engine" / "include" / "jce" / "application" / "jce_lifecycle.h"
SCAN_DIRS = (ROOT / "engine", ROOT / "editor")
SCAN_EXT = (".c", ".cpp", ".h", ".hpp", ".inc.h")

# The sentinel is a count, not an event.
NOT_AN_EVENT = {"JCE_LIFECYCLE_EVENT_COUNT"}


def enumerators():
    """The JceLifecycleEvent values, read from the header that defines them."""
    if not HEADER.is_file():
        return None
    src = io.open(str(HEADER), encoding="utf-8", errors="replace").read()
    m = re.search(r"typedef\s+enum\s+JceLifecycleEvent\s*\{(.*?)\}\s*JceLifecycleEvent",
                  src, re.S)
    if not m:
        return None
    names = re.findall(r"\b(JCE_LIFECYCLE_[A-Z0-9_]+)\b", m.group(1))
    return [n for n in names if n not in NOT_AN_EVENT]


def emit_sites():
    """event name -> [rel:line], for every jce_lifecycle_emit(<NAME>) call.

    Matches the CALL, not a mention: the whole defect was a name that appeared
    in prose, in an enum and in a snapshot without anybody calling it, so a
    substring search would have reported this one as fine.
    """
    pat = re.compile(r"jce_lifecycle_emit\s*\(\s*(JCE_LIFECYCLE_[A-Z0-9_]+)\s*\)")
    out, scanned = {}, 0
    for d in SCAN_DIRS:
        if not d.is_dir():
            continue
        for p in d.rglob("*"):
            if not p.is_file() or not p.name.endswith(SCAN_EXT):
                continue
            try:
                src = io.open(str(p), encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            scanned += 1
            for n, line in enumerate(src.splitlines(), 1):
                for m in pat.finditer(line):
                    rel = p.relative_to(ROOT).as_posix()
                    out.setdefault(m.group(1), []).append("%s:%d" % (rel, n))
    return out, scanned


def main():
    names = enumerators()
    if not names:
        print("check_lifecycle_events_emitted: FAIL - no JceLifecycleEvent "
              "enumerators found in %s.  The header moved, was renamed, or no "
              "longer declares the enum -- this check measured nothing."
              % HEADER.relative_to(ROOT).as_posix())
        return 1

    sites, scanned = emit_sites()
    if scanned < 200:
        print("check_lifecycle_events_emitted: FAIL - only %d source file(s) "
              "scanned, which is too few to have looked at the engine.  A "
              "clean result here would be about an empty search." % scanned)
        return 1

    missing = [n for n in names if n not in sites]
    if missing:
        print("check_lifecycle_events_emitted: FAIL - event(s) a consumer can "
              "register for that nothing ever emits:")
        for n in missing:
            print("    %s" % n)
        print("  Registering for one of these returns a valid handle and the "
              "callback is dead code.  Either emit it, or remove it from the "
              "enum -- but note removing a public enumerator moves every value "
              "after it, which check_abi_snapshot.py will call an incompatible "
              "change, correctly.")
        return 1

    print("check_lifecycle_events_emitted: OK - all %d JceLifecycleEvent "
          "value(s) have an emit site, across %d scanned file(s).  This does "
          "NOT mean each site is reachable: JCE_LIFECYCLE_DEVICE_RESET's only "
          "emitter translates an SDL_Render event, and this engine creates an "
          "SDL_Renderer solely in its safe-mode software fallback."
          % (len(names), scanned))
    return 0


if __name__ == "__main__":
    sys.exit(main())
