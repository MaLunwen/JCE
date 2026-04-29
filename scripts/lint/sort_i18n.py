#!/usr/bin/env python3
"""sort_i18n.py — Normalize editor i18n JSON files.

Sorts keys alphabetically and groups them by the leading dotted segment
(``profiler.*``, ``project.*`` …). Groups are separated by a single blank
line so the file remains diff-friendly.

Default targets: ``editor/resources/assets/i18n/*.json``.

Examples:
    python scripts/lint/sort_i18n.py                   # in-place, default tree
    python scripts/lint/sort_i18n.py --check           # CI mode (non-zero on drift)
    python scripts/lint/sort_i18n.py path/to/file.json # specific file(s)

Behaviour:
    * Top-level only — values may be strings or any JSON, untouched.
    * Keys without a ``.`` form their own group (sorted, kept first).
    * UTF-8 with ``ensure_ascii=False`` preserves CJK characters.
    * Idempotent: re-running on a sorted file is a no-op.
    * ``--check`` reports drifted files and exits 1 without writing.
    * ``--report`` prints en↔zh_cn key-set diff (missing translations).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Dict, List, Tuple


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_DIR = REPO_ROOT / "editor" / "resources" / "assets" / "i18n"


def _group_key(k: str) -> Tuple[int, str]:
    """Top-level prefix used for grouping. Non-dotted keys get an empty
    prefix and sort together as their own group."""
    if "." in k:
        return (1, k.split(".", 1)[0])
    return (0, "")


def render_sorted(data: Dict[str, object]) -> str:
    """Return a normalized JSON string for ``data``.

    Keys are alphabetically sorted with a blank line between groups
    (groups defined by the leading dotted segment).
    """
    keys = sorted(data.keys())
    lines: List[str] = ["{"]
    prev_group: Tuple[int, str] | None = None
    for i, k in enumerate(keys):
        grp = _group_key(k)
        if prev_group is not None and grp != prev_group:
            lines.append("")
        prev_group = grp
        # json.dumps each piece independently to preserve nested formatting
        # (current files are flat strings, but be tolerant).
        key_s = json.dumps(k, ensure_ascii=False)
        val_s = json.dumps(data[k], ensure_ascii=False)
        comma = "," if i < len(keys) - 1 else ""
        lines.append(f"    {key_s}: {val_s}{comma}")
    lines.append("}")
    lines.append("")  # trailing newline
    return "\n".join(lines)


def process_file(path: Path, check: bool) -> bool:
    """Returns True if file is already normalized, False otherwise."""
    original = path.read_text(encoding="utf-8")
    try:
        data = json.loads(original)
    except json.JSONDecodeError as e:
        print(f"[error] {path}: invalid JSON ({e})", file=sys.stderr)
        return False
    if not isinstance(data, dict):
        print(f"[skip ] {path}: top-level is not an object", file=sys.stderr)
        return True

    rendered = render_sorted(data)
    if rendered == original:
        print(f"[ok   ] {path}")
        return True

    if check:
        print(f"[drift] {path}: would be reformatted")
        return False

    path.write_text(rendered, encoding="utf-8")
    print(f"[fix  ] {path}: rewrote {len(data)} keys")
    return True


def report_sync(files: List[Path]) -> None:
    """Cross-locale key-set diff — useful when adding new strings."""
    sets: Dict[Path, set] = {}
    for f in files:
        try:
            sets[f] = set(json.loads(f.read_text(encoding="utf-8")).keys())
        except (OSError, json.JSONDecodeError):
            continue
    if len(sets) < 2:
        return
    union = set().union(*sets.values())
    print()
    print(f"── key-set diff across {len(sets)} locale(s) ──")
    for f, keys in sets.items():
        missing = sorted(union - keys)
        if missing:
            print(f"  {f.name}: missing {len(missing)} key(s)")
            for k in missing[:20]:
                print(f"    - {k}")
            if len(missing) > 20:
                print(f"    ... and {len(missing) - 20} more")
        else:
            print(f"  {f.name}: complete ({len(keys)} keys)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", type=Path,
                    help="JSON files (default: editor/resources/assets/i18n/*.json)")
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if any file would be reformatted")
    ap.add_argument("--report", action="store_true",
                    help="print missing-key report across locales")
    args = ap.parse_args()

    targets: List[Path] = list(args.files) if args.files else \
        sorted(DEFAULT_DIR.glob("*.json"))
    if not targets:
        print(f"no JSON files found in {DEFAULT_DIR}", file=sys.stderr)
        return 1

    all_ok = True
    for path in targets:
        if not path.is_file():
            print(f"[error] {path}: not found", file=sys.stderr)
            all_ok = False
            continue
        if not process_file(path, args.check):
            all_ok = False

    if args.report:
        report_sync(targets)

    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
