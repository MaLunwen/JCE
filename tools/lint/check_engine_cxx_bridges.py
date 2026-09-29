#!/usr/bin/env python3
"""
check_engine_cxx_bridges.py — AGENTS.md §11's first invariant, as a gate.

§11 opens with "引擎内不出现 C++", and then carves out one exception: a `.cpp`
that is a THIN WRAPPER bridging a third-party C++ library to the engine's C
API.  The exception is the whole point -- Bullet, ozz, Recast and
behaviortree.cpp have no C API, so somebody has to speak C++ to them -- but an
exception nobody checks is just a second rule saying "C++ is fine here".

It had drifted exactly that way.  engine/src carried 16 first-party .cpp files
and `jce_audio_stream.cpp` was one of them: 381 lines, includes only jce
headers and <string.h>, bridges to NOTHING.  It was a C file with the wrong
extension, and it compiled as C11 with zero errors and zero warnings the first
time anyone tried (30,412-byte object).  It is `.c` now.

WHY AN ALLOW-LIST RATHER THAN "DOES IT INCLUDE A THIRD-PARTY HEADER".
Sniffing for a third-party include is the obvious rule and it is the wrong
one: it passes any file that includes <RmlUi/Core.h> once and then holds two
thousand lines of first-party C++ logic, which is the drift this is meant to
stop.  Naming each bridge WITH the library it bridges makes the exception
finite and reviewable -- and makes adding a 17th an explicit decision instead
of a file appearing.

The engine is C99.  If your new file has no third-party C++ library on the
other side of it, it is a .c file.

USAGE
    python tools/lint/check_engine_cxx_bridges.py
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCAN_ROOT = REPO_ROOT / "engine" / "src"
ALLOW_FILE = Path(__file__).resolve().parent / "engine_cxx_bridge_allow.txt"

# Vendored upstream sources are not first-party code and are not this gate's
# business: fdk-aac and openh264 alone are 300+ .cpp files written by other
# people, and §11 speaks about what WE write.
VENDOR_MARKERS = ("/third_party/",)

EXIT_SKIPPED = 2


def load_allow() -> dict[str, str]:
    """{repo-relative path: reason}. A line with no '#' reason is IGNORED.

    Same fail-closed shape as api_closure_exempt.txt: an entry here says a C++
    translation unit belongs in a C engine, which is exactly the claim that
    should have to be written down.
    """
    out: dict[str, str] = {}
    if not ALLOW_FILE.is_file():
        return out
    for raw in ALLOW_FILE.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "#" not in line:
            continue
        path, reason = line.split("#", 1)
        if path.strip() and reason.strip():
            out[path.strip()] = reason.strip()
    return out


# Every extension that means "this is C++, not C99".  It was ".cpp" alone
# until 2026-09-01, when splitting the Bullet backend produced
# jce_physics_bullet_internal.hpp -- a first-party engine header holding a
# struct whose members are btRigidBody*, INVISIBLE to this gate.  §11's
# exception is for a .cpp bridge; a .hpp full of C++ is the same violation
# with a different suffix, and scanning one extension made the other one free.
CXX_SUFFIXES = (".cpp", ".cxx", ".cc", ".hpp", ".hxx", ".hh")


def first_party_cpp() -> list[str]:
    out = []
    for suffix in CXX_SUFFIXES:
        for p in SCAN_ROOT.rglob("*" + suffix):
            rel = p.relative_to(REPO_ROOT).as_posix()
            if any(m in "/" + rel for m in VENDOR_MARKERS):
                continue
            out.append(rel)
    return sorted(out)


def main() -> int:
    if not SCAN_ROOT.is_dir():
        print("engine-cxx-bridge check: SKIPPED — %s is not present."
              % SCAN_ROOT)
        return EXIT_SKIPPED

    allow = load_allow()
    found = first_party_cpp()

    unlisted = [f for f in found if f not in allow]
    stale = [f for f in allow if f not in found]

    if not unlisted and not stale:
        print("engine-cxx-bridge check: OK — %d first-party C++ file(s) under "
              "engine/src, each named as a bridge to a specific third-party "
              "C++ library." % len(found))
        return 0

    print("engine-cxx-bridge check: FAILED")
    print()
    if unlisted:
        print("First-party C++ in the engine with no recorded bridge (%d):"
              % len(unlisted))
        for f in unlisted:
            print("  %s" % f)
        print()
        print("  §11: the engine is C99.  A .cpp is allowed only as a thin")
        print("  wrapper over a third-party C++ library that has no C API.")
        print("  If yours has one, add it to")
        print("  tools/lint/engine_cxx_bridge_allow.txt with the library")
        print("  named on the same line.  If it does not, it is a .c file --")
        print("  jce_audio_stream.cpp was, and compiled as C11 unchanged.")
        print()
    if stale:
        # A stale entry is a standing claim that a C++ bridge exists where the
        # tree has none.  Left alone it silently re-authorises whatever file
        # later takes that path.
        print("Recorded bridges that no longer exist (%d):" % len(stale))
        for f in stale:
            print("  %s   # %s" % (f, allow[f]))
        print()
    return 1


if __name__ == "__main__":
    sys.exit(main())
