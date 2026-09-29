#!/usr/bin/env python3
"""
find_duplicate_symbols.py — dedup-audit detector: the same function name
defined in more than one first-party translation unit.

This is a *candidate generator* for the engine/editor deduplication audit
(.docs/way/JCE_ENGINE_EDITOR_DEDUP_REFACTOR_AUDIT_PLAN_STRICT.md §7.4).  A
duplicated name is not automatically a defect — it may be a legitimate
platform variant (foo_win.c / foo_linux.c), a test double, or an unrelated
static helper.  The tool therefore classifies each collision so a human /
reviewer can triage, and only the cross-consumer (engine<->editor) and
non-platform external collisions are treated as hard signals.

It is deliberately independent from the runtime code path so it can serve
as the plan's "independent static verification" (§5.2): re-running it after
a refactor should show the collision gone.

Usage:
  python tools/audit/find_duplicate_symbols.py            # human report
  python tools/audit/find_duplicate_symbols.py --json     # machine output
  python tools/audit/find_duplicate_symbols.py --check    # exit 1 on hard signal
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# First-party source roots to scan (repo-relative prefixes).  Only git-TRACKED
# files under these roots are analysed, so local scratch (_retired_*,
# halloween-test, build/, dist/) is excluded automatically and correctly.
#
# User projects are OUT of this gate's scan surface (owner decision, 2026-08-27).
# The general engine and editor are the product; a user project is a downstream
# dogfooding consumer.  Folding the consumer in means a defect inside
# examples/caged_kingdom/ can turn the ENGINE's architecture gate red -- and the question
# this gate answers is whether the engine and the editor held their boundaries.
# Whether consumers deserve a gate of their own is a separate question.
# examples/caged_kingdom/src/ used to be here as "the canonical sandbox"; space/ and
# street_demo/ never were.  Now none of them are.
SCAN_PREFIXES = (
    "engine/src/",
    "editor/src/",
    "tools/",
)

SRC_EXT = (".c", ".cc", ".cpp", ".cxx", ".m", ".mm")

# Path fragments that are never first-party.
SKIP_FRAGMENTS = ("/third_party/", "/generated/")

# Vendored single-file libraries dropped into the tree.
VENDOR_PREFIXES = ("stb_", "miniaudio", "dr_", "cJSON", "cgltf", "tinyexr")

# Names whose multi-file definition is never a duplication defect.
NAME_IGNORE = {"main", "WinMain", "wWinMain", "DllMain"}

# Platform-variant suffixes: a name shared by these siblings is a legitimate
# single logical symbol with per-platform bodies, not a duplicate owner.
PLATFORM_SUFFIXES = (
    "_win", "_windows", "_linux", "_mac", "_macos", "_cocoa", "_posix",
    "_android", "_web", "_wasm", "_emscripten", "_ios", "_null", "_stub",
    "_d3d11", "_d3d12", "_vk", "_vulkan", "_gl", "_gles", "_metal",
)

# C/C++ keywords that can precede '(' but are not function definitions.
KEYWORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "else", "do",
    "case", "default", "typedef", "struct", "union", "enum", "static_assert",
    "_Static_assert", "alignof", "_Alignof", "defined", "and", "or", "not",
    "catch", "template", "decltype", "noexcept", "constexpr", "operator",
}

# A function definition header: optional leading return-type tokens, an
# identifier, a parenthesised list, then '{' (same line or next).  We anchor
# on non-indented lines to avoid matching call sites and nested lambdas.
DEF_RE = re.compile(
    r"^(?P<pre>[A-Za-z_][\w\s\*\&:<>,]*?)\b(?P<name>[A-Za-z_]\w*)\s*\([^;{}]*\)\s*"
    r"(?:const\b\s*)?(?:noexcept\b\s*)?(?:->[^;{]*)?\{?\s*$"
)


def rel(p: Path) -> str:
    return p.relative_to(REPO_ROOT).as_posix()


def is_vendored(name: str) -> bool:
    return name.startswith(VENDOR_PREFIXES)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def base_stem(stem: str) -> tuple[str, bool]:
    """Return (platform-neutral stem, is_platform_variant)."""
    for suf in PLATFORM_SUFFIXES:
        if stem.endswith(suf):
            return stem[: -len(suf)], True
    return stem, False


def extract_defs(path: Path) -> list[tuple[str, int, bool]]:
    """Return [(func_name, lineno, is_static)] for a source file."""
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    text = strip_comments(raw)
    lines = text.splitlines()
    out: list[tuple[str, int, bool]] = []
    for i, line in enumerate(lines, start=1):
        if not line or line[0].isspace() or line[0] == "#":
            continue
        # Join a definition header that puts '{' on the next line.
        probe = line
        if "(" in probe and "{" not in probe and ";" not in probe:
            nxt = lines[i] if i < len(lines) else ""
            if nxt.strip().startswith("{"):
                probe = probe + " {"
        m = DEF_RE.match(probe.rstrip())
        if not m:
            continue
        name = m.group("name")
        pre = m.group("pre")
        if name in KEYWORDS or pre.strip() in KEYWORDS:
            continue
        # A macro invocation like FOO(x) at column 0 usually has no return type.
        if not pre.strip():
            continue
        is_static = bool(re.search(r"\bstatic\b", pre))
        out.append((name, i, is_static))
    return out


def tracked_source_files() -> list[str]:
    """Repo-relative posix paths of git-tracked first-party source files."""
    out = subprocess.run(
        ["git", "ls-files", "-z", "--", *SCAN_PREFIXES],
        cwd=REPO_ROOT, capture_output=True, text=True, encoding="utf-8",
    )
    if out.returncode != 0:
        print("error: git ls-files failed:\n" + out.stderr, file=sys.stderr)
        sys.exit(2)
    files = []
    for rp in out.stdout.split("\0"):
        if not rp or not rp.endswith(SRC_EXT):
            continue
        if any(frag in ("/" + rp) for frag in SKIP_FRAGMENTS):
            continue
        if is_vendored(Path(rp).name):
            continue
        files.append(rp)
    return files


def scan() -> dict[str, list[dict]]:
    # name -> list of {file, line, static, root, stem, platform}
    table: dict[str, list[dict]] = defaultdict(list)
    for rp in tracked_source_files():
        p = REPO_ROOT / rp
        root_tag = rp.split("/", 1)[0]
        stem, is_plat = base_stem(Path(rp).stem)
        for name, line, is_static in extract_defs(p):
            if name in NAME_IGNORE:
                continue
            table[name].append({
                "file": rp,
                "line": line,
                "static": is_static,
                "root": root_tag,
                "stem": stem,
                "platform": is_plat,
            })
    return table


def classify(name: str, defs: list[dict]) -> str | None:
    """Return a signal class for a multi-file collision, or None to drop it."""
    files = {d["file"] for d in defs}
    if len(files) < 2:
        return None
    roots = {d["root"] for d in defs}
    stems = {d["stem"] for d in defs}
    # Same platform-neutral stem across only platform variants => legitimate.
    if len(stems) == 1 and all(d["platform"] for d in defs):
        return None
    # Engine <-> editor collision is the strongest dedup signal.
    if "engine" in roots and "editor" in roots:
        return "cross_consumer"
    any_external = any(not d["static"] for d in defs)
    if any_external and len(stems) > 1:
        return "external_multi_owner"
    if not any_external:
        return "static_helper_collision"
    return "review"


HARD_SIGNALS = {"cross_consumer", "external_multi_owner"}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if any hard-signal collision exists")
    ap.add_argument("--min-name-len", type=int, default=4,
                    help="ignore short names (init/run/...) below this length")
    args = ap.parse_args()

    table = scan()
    grouped: dict[str, list[tuple[str, list[dict]]]] = defaultdict(list)
    for name, defs in table.items():
        if len(name) < args.min_name_len:
            continue
        cls = classify(name, defs)
        if cls is None:
            continue
        grouped[cls].append((name, defs))

    for cls in grouped:
        grouped[cls].sort(key=lambda kv: kv[0])

    if args.json:
        payload = {
            cls: [{"name": n, "definitions": d} for n, d in items]
            for cls, items in grouped.items()
        }
        print(json.dumps(payload, indent=2))
    else:
        order = ["cross_consumer", "external_multi_owner",
                 "static_helper_collision", "review"]
        titles = {
            "cross_consumer": "ENGINE<->EDITOR name collisions (strongest dedup signal)",
            "external_multi_owner": "External-linkage name in multiple stems",
            "static_helper_collision": "Same static-helper name in many files (copy-paste candidate)",
            "review": "Other multi-file collisions (review)",
        }
        for cls in order:
            items = grouped.get(cls, [])
            if not items:
                continue
            print(f"== {titles[cls]}: {len(items)} ==")
            for name, defs in items:
                locs = ", ".join(f"{d['file']}:{d['line']}"
                                 f"{'(static)' if d['static'] else ''}"
                                 for d in defs)
                print(f"  {name}  ->  {locs}")
            print()

    hard = sum(len(grouped.get(c, [])) for c in HARD_SIGNALS)
    soft = sum(len(grouped.get(c, [])) for c in grouped if c not in HARD_SIGNALS)
    print(f"duplicate-symbol scan: {hard} hard-signal, {soft} soft-signal collision(s).")
    if args.check and hard:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
