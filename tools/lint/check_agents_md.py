#!/usr/bin/env python3
"""
check_agents_md.py — Verify every required AGENTS.md is in place.

The JCE project documents per-module rules via `AGENTS.md` files placed at
**leaf directories only** (no aggregator/parent AGENTS.md). The canonical
list of required AGENTS.md locations lives in `contracts/module-memory-index.md`;
this script hard-codes the same list so CI can fail fast when:

  * A required AGENTS.md is missing (someone deleted or forgot it).
  * A new AGENTS.md appears in a directory that is intentionally excluded
    (e.g. `engine/proto/` or `engine/shaders/` — VS Code mis-associates
    `.md` files inside those globs as INI / shader language; those rules
    are folded into a sibling module's AGENTS.md instead).

Update REQUIRED / EXCLUDED in tandem with `contracts/module-memory-index.md`.

Usage:
  python tools/lint/check_agents_md.py
  # exits 0 if clean, 1 with missing/extra report.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# run_all.py reads 2 as SKIPPED and prints "NOT a pass"; 0 would be a pass.
EXIT_SKIPPED = 2

# Required AGENTS.md locations (39 files). Paths are POSIX-style relative to
# repo root.  It was 40 until 2026-08-27, when the user project's charter left
# the list: a general gate must not go red because a dogfooding consumer's
# charter is missing.
REQUIRED = [
    # Root
    "AGENTS.md",

    # Engine — public API umbrella
    "engine/include/jce/AGENTS.md",

    # Engine — L1-L2 OS / Core / Platform
    "engine/src/os/core/AGENTS.md",
    "engine/src/os/platform/AGENTS.md",

    # Engine — L3 services
    "engine/src/renderer/AGENTS.md",
    "engine/src/resource/AGENTS.md",
    "engine/resources/AGENTS.md",

    # Engine — L4 middleware (10 leaves)
    "engine/src/middleware/ai/AGENTS.md",
    "engine/src/middleware/animation/AGENTS.md",
    "engine/src/middleware/audio/AGENTS.md",
    "engine/src/middleware/net/AGENTS.md",
    "engine/src/middleware/physics/AGENTS.md",
    "engine/src/middleware/save/AGENTS.md",
    "engine/src/middleware/scene/AGENTS.md",
    "engine/src/middleware/ui/AGENTS.md",
    "engine/src/middleware/video/AGENTS.md",
    "engine/src/middleware/world/AGENTS.md",

    # Engine — L5 / L6 runtime + application
    "engine/src/runtime/AGENTS.md",
    "engine/src/application/AGENTS.md",

    # Engine — cross-platform / interface glue
    "engine/java/AGENTS.md",
    "engine/tools_include/AGENTS.md",
    "engine/ui/AGENTS.md",

    # Editor — root + 10 sub-leaves + resources
    "editor/AGENTS.md",
    "editor/src/core/AGENTS.md",
    "editor/src/panels/AGENTS.md",
    "editor/src/ui/AGENTS.md",
    "editor/src/dialogs/AGENTS.md",
    "editor/src/viewers/AGENTS.md",
    "editor/src/gizmo/AGENTS.md",
    "editor/src/io/AGENTS.md",
    "editor/src/scene/AGENTS.md",
    "editor/src/widgets/AGENTS.md",
    "editor/src/game_modules/AGENTS.md",
    "editor/resources/AGENTS.md",

    # User projects are NOT required here (owner decision, 2026-08-27):
    # this gate covers the general engine + editor + tooling surface, and a
    # general gate must not go red because a dogfooding consumer's charter
    # is missing.  examples/caged_kingdom/AGENTS.md used to be listed under
    # "# Consumer" -- and it is not even tracked on main.

    # Build / tooling
    "scripts/AGENTS.md",
    "tools/coverage/AGENTS.md",
    "tools/AGENTS.md",
    "tools/build/AGENTS.md",
    "tools/lint/AGENTS.md",
    "tools/audit/AGENTS.md",
    "conan/AGENTS.md",

    # Tests (Unity + CTest; 阶段 1 hardening tree)
    "tests/AGENTS.md",
]

# Directories that must NOT contain a top-level AGENTS.md.
# Reason: VS Code's user-level file association maps `.md` files inside
# these globs to non-markdown languages, which silently mis-highlights
# them. Their rules are folded into a sibling/parent module's AGENTS.md.
EXCLUDED = [
    "engine/proto",     # rules live in engine/src/middleware/net/AGENTS.md
    "engine/shaders",   # rules live in engine/src/renderer/AGENTS.md
    "conan/profiles",   # rules live in conan/AGENTS.md (parent)
]


# tracked_required() lived here until 2026-08-31.  It asked git how many of
# the REQUIRED paths were versioned, and main() skipped unless all 39 were --
# which, on `main`, is never: .gitignore's `agents.*` pattern matches them all,
# so the count is 0 on every machine and the gate could not fail anywhere.
# The question this gate asks ("did someone delete one?") is answered by
# is_file(), so git is not needed at all.  Removed rather than left unused: a
# helper nothing calls is read as a helper something calls.


def main() -> int:
    # PRECONDITION: "are any of them here", NOT "are they tracked".
    #
    # This gate answers "did someone delete a module's AGENTS.md?", which is a
    # question about a working tree, and every check below is an is_file() on
    # disk -- git is not needed to answer it.  It used to skip when
    # tracked_required() < 39, and on `main` these files match .gitignore's
    # `agents.*` pattern, so that count is 0 on EVERY machine.  The gate could
    # not fail anywhere, and it returned 0 rather than EXIT_SKIPPED, so
    # run_all.py counted it as PASS -- the exact "never pass silently" that
    # the comment here used to promise.
    #
    # The honest split needs no git at all:
    #   none on disk   -> a fresh clone that never carried them: SKIPPED
    #   some on disk   -> a working tree with a hole in it: FAILED, which is
    #                     precisely the deletion this gate exists to catch
    present = [rel for rel in REQUIRED if (REPO_ROOT / rel).is_file()]
    if not present:
        print(f"agents-md check: SKIPPED — none of the {len(REQUIRED)} "
              f"required AGENTS.md are on disk.  On `main` they match "
              f".gitignore's `agents.*` pattern, so a clean clone carries "
              f"none of them and there is nothing to have deleted.  This is "
              f"NOT a pass: nothing was verified.")
        return EXIT_SKIPPED

    missing = [rel for rel in REQUIRED if rel not in present]

    extra_in_excluded: list[str] = []
    for excl in EXCLUDED:
        candidate = REPO_ROOT / excl / "AGENTS.md"
        if candidate.is_file():
            extra_in_excluded.append(f"{excl}/AGENTS.md")

    if not missing and not extra_in_excluded:
        print(f"agents-md check: OK ({len(REQUIRED)} required files in place)")
        return 0

    print("agents-md check: FAILED")
    print()
    if missing:
        print(f"Missing required AGENTS.md ({len(missing)}):")
        for m in missing:
            print(f"  {m}")
        print()
    if extra_in_excluded:
        print(f"AGENTS.md present in excluded directory ({len(extra_in_excluded)}):")
        for x in extra_in_excluded:
            print(f"  {x}")
        print("  (rules for excluded dirs must live in sibling/parent AGENTS.md;")
        print("   see contracts/module-memory-index.md for the canonical mapping.)")
        print()
    return 1


if __name__ == "__main__":
    sys.exit(main())
