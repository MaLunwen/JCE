#!/usr/bin/env python3
"""check_no_machine_paths.py -- a build script may not name one machine's disk.

A path literal naming a developer's own drive is not a default.  On
the machine it was written on it is invisible, because it is correct; on every
other machine it converts a clear failure ("you have not installed emsdk")
into a confusing one ("the toolchain is at a path that does not exist"), and
the person who hits it has no way to know the path was never meant for them.

MEASURED, 2026-09-18: four tracked build scripts carried eight such literals.

    tools/build/jce.py:576              emsdk    a checkout on one developer's drive
    scripts/build-web.bat:37        emsdk    the same one, in batch spelling
    scripts/build-android.bat:42    NDK      a pinned android-ndk-r27d under it
    scripts/build-android.bat:45    SDK      an android-sdk beside that
    scripts/build-android.bat:67    JDK      an openjdk-21 on the same drive
    scripts/package-jni-jar.bat:51  JDK      an openjdk-8 on the same drive

The JDK one was the worst, and shows why a reviewer cannot be relied on to
catch these: it did not DEFAULT `JAVA_HOME`, it OVERWROTE it, unconditionally,
on every machine.  A developer with a working JDK got the broken path.

THE RULE.  In a tracked build script, an absolute path that names a machine --
a drive-letter path, or a home directory under /home or /Users -- is a
failure.  Toolchains are located by deriving them: the environment variable
the installer exports, the tool already on PATH walked back to its root, or a
location stated RELATIVE to this repository.  Those three mean the same thing
everywhere; a literal does not.

WHAT IS NOT A MACHINE PATH.  A handful of Windows system locations are
OS constants, not machine choices, and appear only as fallbacks beside the
environment variable that supersedes them (`%ProgramFiles%`, `%SystemRoot%`).
They are allowed by name, below, and the list is deliberately short.

Anything else needs a line in machine_path_exempt.txt with a reason on the
same line.  A bare path with no reason is IGNORED, not accepted -- so "I will
write the justification later" fails closed.

THIS FILE CARRIES NO LITERAL PATH OF ITS OWN, deliberately.  The first
version spelled the six offenders out above, passed every control, and then
went RED on itself the moment it was committed -- because the scan is
`git ls-files`, so while it was untracked it never scanned itself.  Its
green had been an artefact of not yet being in the tree, which is this
repository's disarmed-gate failure wearing its other face.  Exempting
itself was the wrong repair: a checker that cannot survive its own rule is
one the next reader will feel entitled to exempt too.

EXIT CODES
    0  no tracked build script names a machine
    1  at least one does, and is not exempt
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXEMPT = Path(__file__).resolve().parent / "machine_path_exempt.txt"

# Where build scripts live.  tools/ and cmake/ are in scope for the same
# reason scripts/ is: they run commands and resolve toolchains.
SCAN_DIRS = ("scripts", "tools", "cmake", "conan")
SCAN_EXT = (".py", ".bat", ".cmd", ".sh", ".ps1", ".cmake", ".gradle", ".properties")

# A drive-letter path, or a POSIX home under /home or /Users.  The context
# class in front keeps `re.compile(...)` and ordinary prose from matching by
# accident -- a literal in real code is preceded by a quote, an equals, a
# space or an open paren.
DRIVE = re.compile(r"""["'=(\s][A-Za-z]:[\\/][A-Za-z0-9_.$%]""")
HOME = re.compile(r"""["'=(\s](?:/home/|/Users/)[A-Za-z0-9_.-]+/""")

# OS constants, not machine choices.  Each is allowed only as the FALLBACK
# argument beside the environment variable that names it, which is how all
# three appear in this tree today; the check is textual, so the list stays
# short on purpose rather than growing into a second exemption file.
OS_CONSTANTS = (
    r"C:\Program Files",
    r"C:/Program Files",
    r"C:\Windows",
    r"C:/Windows",
)


def tracked_files():
    """git ls-files, NOT rglob.

    An untracked scratch script is not something a clone can run, and
    including it would make this gate's verdict depend on what happens to be
    lying in the working tree -- which is the shape of a gate that says
    different things to different people.
    """
    out = subprocess.run(["git", "ls-files", *SCAN_DIRS],
                         cwd=ROOT, capture_output=True, text=True)
    if out.returncode != 0:
        return []
    files = []
    for rel in out.stdout.splitlines():
        rel = rel.strip()
        if rel and rel.endswith(SCAN_EXT):
            files.append(rel)
    return sorted(files)


def read_exempt():
    """`<path>:<line-or-*>` then whitespace then the reason.

    A line with no reason is IGNORED -- not accepted.  Same fail-closed shape
    as shader_axis_exempt.txt and api_closure_exempt.txt, for the same reason:
    an exemption whose justification nobody wrote is one nobody can review.
    """
    out = {}
    if not EXEMPT.is_file():
        return out
    for raw in EXEMPT.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[1].strip():
            out[parts[0]] = parts[1].strip()
    return out


def is_exempt(exempt, rel, lineno):
    return f"{rel}:{lineno}" in exempt or f"{rel}:*" in exempt


def offenders():
    found = []
    for rel in tracked_files():
        p = ROOT / rel
        try:
            text = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for i, ln in enumerate(text.splitlines(), 1):
            stripped = ln
            for c in OS_CONSTANTS:
                stripped = stripped.replace(c, "")
            if DRIVE.search(stripped) or HOME.search(stripped):
                found.append((rel, i, ln.strip()))
    return found


def main() -> int:
    exempt = read_exempt()
    found = offenders()
    exempted = [f for f in found if is_exempt(exempt, f[0], f[1])]
    failures = [f for f in found if not is_exempt(exempt, f[0], f[1])]

    for rel, i, ln in exempted:
        print("  [exempt] %s:%d  %s" % (rel, i, exempt.get(f"{rel}:{i}")
                                        or exempt.get(f"{rel}:*")))

    if failures:
        print("check_no_machine_paths: FAIL - %d build-script line(s) name a "
              "machine's disk:" % len(failures))
        for rel, i, ln in failures:
            print("  %s:%d" % (rel, i))
            print("      %s" % (ln[:150]))
        print("  Derive it instead: the environment variable the installer "
              "exports, the tool already on PATH walked back to its root, or "
              "a location RELATIVE to this repository. If the literal is "
              "genuinely unavoidable, add '<path>:<line>' to "
              "tools/lint/machine_path_exempt.txt WITH the reason.")
        return 1

    print("check_no_machine_paths: OK - %d tracked build script(s), no machine "
          "paths (%d exempt with a recorded reason)"
          % (len(tracked_files()), len(exempted)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
