# The four tools/audit/ gates that used to run here -- abi-snapshot
# --committed, script-vm-parity, script-language-catalog and
# sdk-scripting-export -- were removed with tools/audit/ itself (owner
# decision, 2026-08-17).  They are listed here rather than silently
# dropped because each answered a question nothing else does, and a
# reader of this file should know the questions stopped being asked:
#   * did a commit change a public header without updating the snapshot
#   * is JceScriptVM still the same lifecycle as the public jce_script_*
#     surface (a 20th function added Lua-only is green in every test)
#   * does every backend's claimed extension have a catalog row
#   * does the SDK still ship what an out-of-tree project needs
# Restoring them means restoring tools/audit/ and re-adding the entries.
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
REPO_ROOT = LINT_DIR.parents[1]

# Order: cheap structural checks first, then content scans.
#
# An entry is either a bare filename (resolved under scripts/lint/) or a
# (repo-relative path, argv) pair for a checker that lives elsewhere.
LINTS = [
    "check_agents_md.py",
    "check_public_api_purity.py",
    "check_editor_consumer_purity.py",
    "check_layer_dependencies.py",
    "check_engine_native_io.py",
    "check_platform_macros.py",
    "check_input_seam.py",
    "check_mesh_enable_consumers.py",
    "check_cull_gen_consumers.py",
    "check_instance_sort_depth.py",
    "check_raw_allocator.py",
    "check_sdk_asset_embed_shape.py",
    "check_project_build_layout.py",
    "check_jce_tests_closure.py",
    "check_env_light_authority.py",
    "check_water_field_authority.py",
    "check_terrain_single_owner.py",
    "check_shader_sampler_slots.py",
    "check_shader_branch_order.py",
    "check_shader_varying_pairs.py",
    "check_format_has_producer.py",
    "check_material_texture_sampler.py",
    "i18n_audit.py",
    "i18n_hardcoded.py",
    "check_i18n_dup_values.py",
]


def resolve(entry) -> tuple[Path, list[str], str]:
    if isinstance(entry, tuple):
        rel, argv = entry
        return REPO_ROOT / rel, list(argv), " ".join([rel, *argv])
    return LINT_DIR / entry, [], entry


def run_one(entry) -> tuple[str, int, str]:
    path, argv, label = resolve(entry)
    if not path.is_file():
        return label, 127, f"(skipped: {label} not found)"
    proc = subprocess.run(
        [sys.executable, str(path), *argv],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    out = (proc.stdout or "") + (proc.stderr or "")
    return label, proc.returncode, out.rstrip()


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
