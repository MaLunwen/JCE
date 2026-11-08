#!/usr/bin/env python3
"""Check installed hook bytes in the native builder's read-only planned home.

An explicit CONAN_HOME is strict. Without it, a conflicting default home selects
exactly the same owned configuration overlay as the supported native builder.
The original global hooks remain untouched. Current source hooks/policy determine
that tuple; bad installed overlay bytes never cause selection of another home.

Missing installed hooks retain clean-machine semantics. They do not prove that
Conan or a build executed: this gate neither creates nor synchronizes anything.
Retired source-writing hooks in the selected home fail; unrelated extras are
reported only. Use --list to display current selected-hook states.
"""
from __future__ import annotations

import hashlib
import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SRC = REPO_ROOT / "conan" / "hooks"
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))
from tools.build.conan_config import select_hook_home


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    if not SRC.is_dir():
        print("check_conan_hooks_synced: SKIPPED - repository hooks are absent.")
        return 0
    ours = sorted(SRC.glob("hook_*.py"))
    if not ours:
        print("check_conan_hooks_synced: SKIPPED - this repo ships no hooks.")
        return 0
    try:
        selection = select_hook_home(REPO_ROOT, dict(os.environ))
    except (OSError, ValueError, KeyError) as error:
        print("check_conan_hooks_synced: FAILED - " + str(error), file=sys.stderr)
        return 1
    home = selection["home"]
    destination = selection["destination"]
    print("Planned Conan hook home: " + str(home))
    print("Read-only selection; installed-hook checks are not build-execution proof.")
    if not destination.is_dir():
        print("check_conan_hooks_synced: OK (%d hooks not installed in the planned "
              "home; a future supported build may populate them)" % len(ours))
        return 0

    stale, missing = [], []
    for name in selection["retired"]:
        installed = destination / name
        if name not in selection["hooks"] and installed.is_file():
            stale.append((name, "RETIRED", digest(installed)))
    for hook in ours:
        installed = destination / hook.name
        if not installed.is_file():
            missing.append(hook.name)
        elif digest(installed) != digest(hook):
            stale.append((hook.name, digest(hook), digest(installed)))

    if "--list" in sys.argv:
        for hook in ours:
            installed = destination / hook.name
            state = ("NOT INSTALLED" if not installed.is_file() else
                     "stale" if digest(installed) != digest(hook) else "same")
            print("%-40s %s" % (hook.name, state))
        for path in sorted(destination.glob("hook_*.py")):
            if path.name not in selection["hooks"]:
                state = "RETIRED" if path.name in selection["retired"] else "other project's, ignored"
                print("%-40s %s" % (path.name, state))
    if stale:
        for name, ours_digest, installed_digest in stale:
            print("  %s: selected installed copy differs (repo %s, installed %s)."
                  % (name, ours_digest[:12], installed_digest[:12]), file=sys.stderr)
        print("  Preserve installed bytes. Unknown hooks require a separately "
              "selected CONAN_HOME; syncing does not overwrite them. Supported "
              "builds only archive recognized retired hooks in their admitted home.",
              file=sys.stderr)
        print("check_conan_hooks_synced: FAILED", file=sys.stderr)
        return 1
    note = ("" if not missing else
            "; %d not installed yet, requiring a future supported build" % len(missing))
    print("check_conan_hooks_synced: OK (%d hook(s), every installed copy in the "
          "planned home matches this repo%s)" % (len(ours), note))
    return 0


if __name__ == "__main__":
    sys.exit(main())
