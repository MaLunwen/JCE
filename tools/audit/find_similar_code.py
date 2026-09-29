#!/usr/bin/env python3
"""
find_similar_code.py — dedup-audit detector: structurally identical function
bodies across different files (copy-paste candidates).

Approach: tokenise each git-tracked first-party C/C++ file, extract function
definitions by brace matching, then compute a *structural fingerprint* of each
body — the token-type stream with identifiers, numbers, strings and chars
normalised to placeholders while keywords and punctuation are kept literal.
Functions whose bodies share a fingerprint are structurally identical even if
variable names / literals differ, which is the signature of copy-paste.

Only groups spanning >=2 distinct files with a non-trivial body are reported;
platform-variant siblings (foo_win.c/foo_linux.c) are collapsed so their
shared shape is not counted as a defect.

This is a candidate generator for the engine/editor deduplication audit
(.docs/way/...DEDUP_REFACTOR_AUDIT_PLAN_STRICT.md §7.4 / §B2).  A match is not
automatically a defect (small boilerplate, generated code, legitimate parallel
platform bodies) — it is a triage list.

Usage:
  python tools/audit/find_similar_code.py                 # human report
  python tools/audit/find_similar_code.py --json
  python tools/audit/find_similar_code.py --min-tokens 40 # raise the floor
  python tools/audit/find_similar_code.py --check --max-groups N  # CI baseline
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
# User projects are OUT of this gate's scan surface (owner decision, 2026-08-27).
# The general engine and editor are the product; a user project is a downstream
# dogfooding consumer.  Folding the consumer in means a defect inside
# examples/caged_kingdom/ can turn the ENGINE's architecture gate red -- and the question
# this gate answers is whether the engine and the editor held their boundaries.
# Whether consumers deserve a gate of their own is a separate question.
SCAN_PREFIXES = ("engine/src/", "editor/src/", "tools/")
SRC_EXT = (".c", ".cc", ".cpp", ".cxx", ".m", ".mm")
SKIP_FRAGMENTS = ("/third_party/", "/generated/")
VENDOR_PREFIXES = ("stb_", "miniaudio", "dr_", "cJSON", "cgltf", "tinyexr")

PLATFORM_SUFFIXES = (
    "_win", "_windows", "_linux", "_mac", "_macos", "_cocoa", "_posix",
    "_android", "_web", "_wasm", "_emscripten", "_ios", "_null", "_stub",
    "_d3d11", "_d3d12", "_vk", "_vulkan", "_gl", "_gles", "_metal",
)

C_KEYWORDS = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline",
    "int", "long", "register", "restrict", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned",
    "void", "volatile", "while", "bool", "true", "false", "nullptr",
    "class", "namespace", "template", "typename", "public", "private",
    "protected", "virtual", "override", "new", "delete", "this", "using",
    "constexpr", "noexcept", "decltype", "operator", "friend", "explicit",
}

TOKEN_RE = re.compile(r"""
    (?P<ws>\s+)
  | (?P<lc>//[^\n]*)
  | (?P<bc>/\*.*?\*/)
  | (?P<pp>\#[^\n]*)
  | (?P<str>"(?:\\.|[^"\\])*")
  | (?P<chr>'(?:\\.|[^'\\])*')
  | (?P<num>\.?\d[\w.]*)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op>->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||::|[-+*/%&|^~!<>=?:;,.(){}\[\]])
  | (?P<other>.)
""", re.S | re.X)


def rel(p: Path) -> str:
    return p.as_posix()


def base_stem(stem: str) -> str:
    for suf in PLATFORM_SUFFIXES:
        if stem.endswith(suf):
            return stem[: -len(suf)]
    return stem


def tracked_files() -> list[str]:
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
        if Path(rp).name.startswith(VENDOR_PREFIXES):
            continue
        files.append(rp)
    return files


def tokenize(text: str) -> list[tuple[str, str, int]]:
    """Return [(kind, value, lineno)] skipping whitespace/comments/preproc."""
    toks: list[tuple[str, str, int]] = []
    line = 1
    for m in TOKEN_RE.finditer(text):
        kind = m.lastgroup
        val = m.group()
        if kind in ("ws", "lc", "bc", "pp"):
            line += val.count("\n")
            continue
        toks.append((kind, val, line))
        line += val.count("\n")
    return toks


def norm(kind: str, val: str) -> str:
    if kind == "id":
        return val if val in C_KEYWORDS else "V"
    if kind == "num":
        return "N"
    if kind == "str":
        return "S"
    if kind == "chr":
        return "C"
    return val  # operators/punct kept literal


def extract_functions(toks: list[tuple[str, str, int]]):
    """Yield (name, start_line, body_norm_tokens) for each definition."""
    n = len(toks)
    i = 0
    brace = 0
    while i < n:
        kind, val, line = toks[i]
        if val == "{":
            brace += 1
            i += 1
            continue
        if val == "}":
            brace = max(0, brace - 1)
            i += 1
            continue
        # Only look for definitions at file scope.
        if brace == 0 and kind == "id" and val not in C_KEYWORDS and i + 1 < n and toks[i + 1][1] == "(":
            # find matching ')'
            depth = 0
            j = i + 1
            while j < n:
                v = toks[j][1]
                if v == "(":
                    depth += 1
                elif v == ")":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            if j >= n:
                i += 1
                continue
            k = j + 1
            # skip trailing qualifiers (const, noexcept, override, ->type)
            while k < n and toks[k][1] in ("const", "noexcept", "override", "final"):
                k += 1
            if k < n and toks[k][1] == "->":
                while k < n and toks[k][1] not in ("{", ";"):
                    k += 1
            if k < n and toks[k][1] == "{":
                # capture body
                depth = 0
                b = k
                body: list[str] = []
                while b < n:
                    bk, bv, _ = toks[b]
                    if bv == "{":
                        depth += 1
                    elif bv == "}":
                        depth -= 1
                        if depth == 0:
                            break
                    else:
                        body.append(norm(bk, bv))
                    b += 1
                yield (val, line, body)
                i = b + 1
                continue
        i += 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--min-tokens", type=int, default=30,
                    help="ignore function bodies shorter than this many tokens")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--max-groups", type=int, default=0,
                    help="with --check, exit 1 if group count exceeds this")
    args = ap.parse_args()

    # fingerprint -> list of (file, name, line, ntok)
    groups: dict[str, list[dict]] = defaultdict(list)
    for rp in tracked_files():
        try:
            text = (REPO_ROOT / rp).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        toks = tokenize(text)
        for name, line, body in extract_functions(toks):
            if len(body) < args.min_tokens:
                continue
            fp = hashlib.blake2b(("|".join(body)).encode(), digest_size=16).hexdigest()
            groups[fp].append({"file": rp, "name": name, "line": line, "ntok": len(body)})

    reported = []
    for fp, members in groups.items():
        files = {m["file"] for m in members}
        if len(files) < 2:
            continue
        # collapse pure platform-variant sibling groups
        stems = {base_stem(Path(m["file"]).stem) for m in members}
        if len(stems) == 1 and len({m["file"] for m in members}) == len(members):
            # same logical file across platform variants -> legitimate
            variant_only = all(
                base_stem(Path(m["file"]).stem) != Path(m["file"]).stem for m in members
            )
            if variant_only:
                continue
        reported.append((fp, sorted(members, key=lambda m: m["file"])))

    reported.sort(key=lambda kv: (-len(kv[1]), -kv[1][0]["ntok"]))

    if args.json:
        print(json.dumps([{"fingerprint": fp, "ntok": members[0]["ntok"],
                           "members": members} for fp, members in reported], indent=2))
    else:
        print(f"== structurally identical function bodies across files: {len(reported)} group(s) ==\n")
        for fp, members in reported:
            head = members[0]
            print(f"  [{head['ntok']} tok] {len(members)} copies:")
            for m in members:
                print(f"      {m['file']}:{m['line']}  {m['name']}()")
            print()
        total_copies = sum(len(m) for _, m in reported)
        print(f"similar-code scan: {len(reported)} clone group(s), {total_copies} function(s) involved.")

    if args.check and args.max_groups and len(reported) > args.max_groups:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
