#!/usr/bin/env python3
"""
check_agents_md.py — Verify every required AGENTS.md is in place.

The JCE project documents per-module rules via `AGENTS.md` files placed at
**leaf directories only** (no aggregator/parent AGENTS.md). The canonical
list of required AGENTS.md locations lives in `.docs/MEMORY_INDEX.md`;
this script hard-codes the same list so CI can fail fast when:

  * A required AGENTS.md is missing (someone deleted or forgot it).
  * A new AGENTS.md appears in a directory that is intentionally excluded
    (e.g. `engine/proto/` or `engine/shaders/` — VS Code mis-associates
    `.md` files inside those globs as INI / shader language; those rules
    are folded into a sibling module's AGENTS.md instead).

Update REQUIRED / EXCLUDED in tandem with `.docs/MEMORY_INDEX.md`.

Usage:
  python scripts/lint/check_agents_md.py
  # exits 0 if clean, 1 with missing/extra report.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Required AGENTS.md locations (40 files). Paths are POSIX-style relative to repo root.
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

    # Consumer
    "caged_kingdom/AGENTS.md",

    # Build / tooling
    "scripts/AGENTS.md",
    "scripts/coverage/AGENTS.md",
    "tools/AGENTS.md",
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


def main() -> int:
    missing: list[str] = []
    for rel in REQUIRED:
        if not (REPO_ROOT / rel).is_file():
            missing.append(rel)

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
        print("   see .docs/MEMORY_INDEX.md for the canonical mapping.)")
        print()
    return 1


if __name__ == "__main__":
    sys.exit(main())
