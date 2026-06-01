#!/usr/bin/env python3
"""
run_all.py — Run every JCE lint in sequence and aggregate results.

Each lint script is invoked as a child process so failures in one do not
short-circuit the others; the aggregate exit code is non-zero if any
individual lint fails. CI calls this as the single entry point.

Usage:
  python scripts/lint/run_all.py
  # exits 0 if all lints pass, 1 if any fail.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent

# Order: cheap structural checks first, then content scans.
LINTS = [
    "check_agents_md.py",
    "check_public_api_purity.py",
    "check_editor_consumer_purity.py",
    "check_layer_dependencies.py",
    "check_engine_native_io.py",
    "check_platform_macros.py",
    "check_raw_allocator.py",
    "i18n_audit.py",
    "i18n_hardcoded.py",
]


def run_one(script: str) -> tuple[str, int, str]:
    path = LINT_DIR / script
    if not path.is_file():
        return script, 127, f"(skipped: {script} not found)"
    proc = subprocess.run(
        [sys.executable, str(path)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    out = (proc.stdout or "") + (proc.stderr or "")
    return script, proc.returncode, out.rstrip()


def main() -> int:
    print(f"running {len(LINTS)} lint(s)...")
    print("=" * 72)
    results: list[tuple[str, int, str]] = []
    for s in LINTS:
        results.append(run_one(s))

    failures = [(s, c, o) for (s, c, o) in results if c != 0]

    for s, c, o in results:
        status = "PASS" if c == 0 else f"FAIL ({c})"
        print(f"[{status}] {s}")

    if not failures:
        print("=" * 72)
        print(f"all {len(LINTS)} lint(s) passed.")
        return 0

    print("=" * 72)
    print(f"{len(failures)} lint(s) failed. details:")
    print()
    for s, c, o in failures:
        print(f"--- {s} (exit {c}) ---")
        print(o)
        print()
    return 1


if __name__ == "__main__":
    sys.exit(main())
