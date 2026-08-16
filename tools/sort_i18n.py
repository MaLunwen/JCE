#!/usr/bin/env python3
"""sort_i18n.py — Normalize + audit editor i18n JSON files.

Sorts keys alphabetically and groups them by the leading dotted segment
(``profiler.*``, ``project.*`` …). Groups are separated by a single blank
line so the file remains diff-friendly.

Default targets: ``editor/resources/assets/i18n/*.json``.

Examples:
    python tools/sort_i18n.py                   # in-place, default tree
    python tools/sort_i18n.py --check           # CI mode (non-zero on drift)
    python tools/sort_i18n.py --report          # key counts + en-based diff
    python tools/sort_i18n.py --report --strict # ^ + exit 1 if a locale lacks en keys
    python tools/sort_i18n.py path/to/file.json # specific file(s)

CMake integration:
    Auto: editor builds depend on the ``jce_i18n_auto`` ALL-target which
          re-runs this script whenever any i18n JSON file changes
          (idempotent + stamp-driven).  Nobody has to remember to invoke it.
    cmake --build <build> --target i18n-sort   # apply normalization manually
    cmake --build <build> --target i18n-check  # CI gate (drift = failure)

Behaviour:
    * Top-level only — values may be strings or any JSON, untouched.
    * Keys without a ``.`` form their own group (sorted, kept first).
    * UTF-8 with ``ensure_ascii=False`` preserves CJK characters.
    * A leading UTF-8 BOM is tolerated on read and STRIPPED on write (a BOM
      counts as drift so ``--check`` flags it and the auto-sort step fixes it
      rather than hard-failing the build).
    * Idempotent: re-running on a sorted file is a no-op.
    * ``--check`` reports drifted files and exits 1 without writing.
    * ``--report`` prints per-locale key counts and an en.json-based key-set
      diff (each locale's missing / extra keys vs the canonical English file).
    * ``--strict`` (with ``--report`` or ``--check``) additionally exits 1 when
      any locale is missing a key that en.json defines — the i18n completeness
      gate, so new UI strings can't ship localized-in-English-only.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DIR = REPO_ROOT / "editor" / "resources" / "assets" / "i18n"
CANONICAL = "en.json"          # the reference locale every other file syncs to
_BOM = b"\xef\xbb\xbf"


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


def _load(path: Path) -> Tuple[Optional[Dict[str, object]], bool, str]:
    """Read a locale file tolerantly.

    Returns ``(data, had_bom, text)`` where ``data`` is the parsed object (or
    None on error), ``had_bom`` is True when the raw file began with a UTF-8
    BOM, and ``text`` is the BOM-stripped source text.  Decoding uses
    ``utf-8-sig`` so a stray BOM does not blow up the parser (it is normalized
    away on the next write instead of hard-failing the build).
    """
    raw = path.read_bytes()
    had_bom = raw.startswith(_BOM)
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError as e:
        print(f"[error] {path}: not valid UTF-8 ({e})", file=sys.stderr)
        return None, had_bom, ""
    # Normalize CRLF/CR → LF so drift detection is line-ending agnostic on
    # Windows checkouts (git stores LF; render_sorted emits LF).  Without this
    # a CRLF working-tree file would look "drifted" against the LF render and
    # be rewritten on every run (non-idempotent).
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    try:
        data = json.loads(text)
    except json.JSONDecodeError as e:
        print(f"[error] {path}: invalid JSON ({e})", file=sys.stderr)
        return None, had_bom, text
    if not isinstance(data, dict):
        print(f"[skip ] {path}: top-level is not an object", file=sys.stderr)
        return None, had_bom, text
    return data, had_bom, text


def process_file(path: Path, check: bool) -> bool:
    """Returns True if file is already normalized, False otherwise."""
    data, had_bom, text = _load(path)
    if data is None:
        # Distinguish "not an object" (benign) from a real parse error: the
        # message was already printed; a non-dict top level is tolerated.
        return text != "" and not had_bom and _is_non_object(path)

    rendered = render_sorted(data)
    # A file is normalized only when it is byte-identical to the canonical
    # rendering AND carries no BOM.  A BOM always counts as drift so it gets
    # stripped by the auto-sort step (instead of failing the build later).
    if rendered == text and not had_bom:
        print(f"[ok   ] {path}: {len(data)} keys")
        return True

    if check:
        reason = "has UTF-8 BOM" if had_bom else "would be reformatted"
        print(f"[drift] {path}: {reason}")
        return False

    # Write UTF-8 WITHOUT a BOM (encoding='utf-8' never emits one) and with
    # LF line endings.
    #
    # newline="" is load-bearing: without it, write_text applies the platform
    # line-ending translation, so every run on Windows rewrote all 15 locale
    # files as CRLF -- against .gitattributes, which mandates LF in the
    # repository AND the working tree.  Because this script runs from a
    # stamp-driven build step, that happened on every build that touched a
    # locale file, silently undoing any normalisation.
    path.write_text(rendered, encoding="utf-8", newline="")
    note = "stripped BOM, " if had_bom else ""
    print(f"[fix  ] {path}: {note}rewrote {len(data)} keys")
    return True


def _is_non_object(path: Path) -> bool:
    """True when the file parsed but its top level is not a dict (benign)."""
    try:
        return not isinstance(json.loads(path.read_bytes().decode("utf-8-sig")), dict)
    except Exception:
        return False


def _key_set(path: Path) -> Optional[set]:
    data, _bom, _text = _load(path)
    return set(data.keys()) if isinstance(data, dict) else None


def report_vs_en(files: List[Path], strict: bool) -> bool:
    """Per-locale key counts + an en.json-based completeness diff.

    en.json is the canonical key set; every other locale is reported with its
    total key count, the number of keys it is MISSING relative to en, and any
    EXTRA keys not present in en (candidates for stale/removed strings).

    Returns True when every locale has at least the full en key set (no
    missing keys); False otherwise.  Callers gate on this only under --strict.
    """
    by_name = {f.name: f for f in files}
    en_path = by_name.get(CANONICAL) or (DEFAULT_DIR / CANONICAL)
    en_keys = _key_set(en_path)
    if en_keys is None:
        print(f"[error] canonical {en_path} unreadable — cannot audit", file=sys.stderr)
        return False

    others = sorted(f for f in files if f.name != CANONICAL)
    print()
    print(f"── i18n key audit (canonical {CANONICAL}: {len(en_keys)} keys) ──")
    complete = True
    for f in others:
        keys = _key_set(f)
        if keys is None:
            print(f"  {f.name:<14} UNREADABLE")
            complete = False
            continue
        missing = sorted(en_keys - keys)
        extra = sorted(keys - en_keys)
        flag = "OK" if not missing else f"MISSING {len(missing)}"
        extra_s = f", +{len(extra)} extra" if extra else ""
        print(f"  {f.name:<14} {len(keys):>5} keys  [{flag}]{extra_s}")
        for k in missing[:15]:
            print(f"      - missing: {k}")
        if len(missing) > 15:
            print(f"      ... and {len(missing) - 15} more missing")
        for k in extra[:8]:
            print(f"      + extra:   {k}")
        if len(extra) > 8:
            print(f"      ... and {len(extra) - 8} more extra")
        if missing:
            complete = False

    print(f"── {'all locales complete vs ' + CANONICAL if complete else 'INCOMPLETE locales above'} ──")
    if strict and not complete:
        print("[strict] one or more locales are missing en keys", file=sys.stderr)
    return complete


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", type=Path,
                    help="JSON files (default: editor/resources/assets/i18n/*.json)")
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if any file would be reformatted")
    ap.add_argument("--report", action="store_true",
                    help="print per-locale key counts + en.json-based missing/extra diff")
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero if any locale is missing a key en.json defines")
    args = ap.parse_args()

    targets: List[Path] = list(args.files) if args.files else \
        sorted(DEFAULT_DIR.glob("*.json"))
    if not targets:
        print(f"no JSON files found in {DEFAULT_DIR}", file=sys.stderr)
        return 1

    all_ok = True
    total_keys = 0
    for path in targets:
        if not path.is_file():
            print(f"[error] {path}: not found", file=sys.stderr)
            all_ok = False
            continue
        ks = _key_set(path)
        if ks is not None:
            total_keys += len(ks)
        if not process_file(path, args.check):
            all_ok = False

    print(f"[total] {len(targets)} file(s), {total_keys} keys")

    sync_ok = True
    if args.report or args.strict:
        sync_ok = report_vs_en(targets, args.strict)

    if not all_ok:
        return 1
    if args.strict and not sync_ok:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
