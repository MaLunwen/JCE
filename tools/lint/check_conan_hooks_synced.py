#!/usr/bin/env python3
"""
check_conan_hooks_synced.py — the conan hooks conan actually runs are the ones
in ~/.conan2, not the ones in this repository.

WHY THIS EXISTS.  conan/hooks/ is version-controlled, but conan loads hooks
from the conan home; tools/build/jce.py copies them across with sync_conan_hooks()
on its way into a build.  Call `conan install` directly -- which is what any
targeted rebuild does -- and the copy never happens.

Measured 2026-09-01: lowering the stable graphics tier from OpenGL 3.3 to 3.1
edited conan/hooks/hook_bgfx_wasm_fix.py, and the rebuild that followed ran
with the installed 3.3 copy for several minutes.  Nothing failed.  The one
thing that gave it away was a log line the hook happens to print --
"graphics tier stable: OpenGL 3.3, GLES 3.0" -- read by chance while checking
progress.  Had the hook been silent, a bgfx built at the wrong floor would
have been indistinguishable from one built at the right one, and the package
id would have said the new value while the binary held the old.

WHAT THIS CHECKS.  For every hook this repository ships, if a file of that
name exists in the conan home it must be byte-identical.  A hook that is NOT
installed is fine -- that is a clean machine, and jce.py will install it.  A
hook that is installed and DIFFERENT is the hazard, because conan silently
prefers it.

Known retired JCE source-writing hooks fail. Other extra files are reported
but do not fail: other projects
share that directory and their hooks are none of this repo's business.

Fix when it fires:
    python -c "import sys;sys.path.insert(0,'scripts');import jce;jce.sync_conan_hooks()"
or just run any tools/build/jce.py build command, which syncs on the way in.

Usage:
    python tools/lint/check_conan_hooks_synced.py
    python tools/lint/check_conan_hooks_synced.py --list
Exit 0 clean, 1 when an installed hook differs from this repo's.
"""

from __future__ import annotations

import json
import hashlib
import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SRC = REPO_ROOT / "conan" / "hooks"
DST = Path(os.environ.get("CONAN_HOME", str(Path.home() / ".conan2"))) / "extensions/hooks"


def digest(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()[:12]


def main() -> int:
    if not SRC.is_dir():
        print("check_conan_hooks_synced: SKIPPED — %s is not present."
              % SRC.relative_to(REPO_ROOT).as_posix())
        return 0

    ours = sorted(SRC.glob("hook_*.py"))
    if not ours:
        print("check_conan_hooks_synced: SKIPPED — this repo ships no hooks.")
        return 0

    if not DST.is_dir():
        print("check_conan_hooks_synced: OK (%d hook(s) here; the conan home "
              "has no hooks directory yet, so nothing stale can be preferred "
              "over them)" % len(ours))
        return 0

    stale, missing = [], []
    retired = json.loads((REPO_ROOT / "contracts/conan-source-policy.json").read_text(encoding="utf-8"))["retired_hooks"]
    for name in retired:
        if not (SRC / name).is_file() and (DST / name).is_file():
            stale.append((name, "RETIRED", digest(DST / name)))
    for h in ours:
        installed = DST / h.name
        if not installed.is_file():
            missing.append(h.name)
        elif digest(installed) != digest(h):
            stale.append((h.name, digest(h), digest(installed)))

    if "--list" in sys.argv:
        for h in ours:
            installed = DST / h.name
            state = ("NOT INSTALLED" if not installed.is_file()
                     else "stale" if digest(installed) != digest(h) else "same")
            print("%-40s %s" % (h.name, state))
        extra = sorted(p.name for p in DST.glob("hook_*.py")
                       if not (SRC / p.name).is_file())
        for name in extra:
            print("%-40s %s" % (name, "other project's, ignored"))

    if stale:
        for name, ours_d, theirs_d in stale:
            print("  %s: the installed copy differs (repo %s, installed %s).\n"
                  "      conan runs the INSTALLED one, so an edit here has not "
                  "reached any build you have run since.  Sync with\n"
                  "        python -c \"import sys;sys.path.insert(0,'scripts');"
                  "import jce;jce.sync_conan_hooks()\"\n"
                  "      or run any tools/build/jce.py build command."
                  % (name, ours_d, theirs_d), file=sys.stderr)
        print("check_conan_hooks_synced: FAILED", file=sys.stderr)
        return 1

    note = ("" if not missing
            else "; %d not installed yet, which jce.py will do" % len(missing))
    print("check_conan_hooks_synced: OK (%d hook(s), every installed copy "
          "matches this repo%s)" % (len(ours), note))
    return 0


if __name__ == "__main__":
    sys.exit(main())
