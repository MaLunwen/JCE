#!/usr/bin/env python3
"""
check_singleton_ownership.py — dedup-audit detector: enumerate file-scope
mutable global / static state and flag same-concept holders that may encode a
fact in more than one place.

The engine's contract (AGENTS.md §6/§11) is that every global service has a
single owner and initialisation order, and that the same fact is not stored in
two places.  This tool lists all file-scope mutable statics and non-const
globals in the first-party engine + editor, groups by a normalised concept
name, and highlights "current X" / registry / cache / pool style holders and
any concept whose holders span more than one module (a dual-ownership smell).

It doubles as a Phase-0 freeze gate (§31): `--check --baseline <file>` fails
when the global count rises above a recorded baseline, so no *new* global
state slips in during the refactor.

Usage:
  python tools/audit/check_singleton_ownership.py
  python tools/audit/check_singleton_ownership.py --json
  python tools/audit/check_singleton_ownership.py --write-baseline tools/audit/globals.baseline
  python tools/audit/check_singleton_ownership.py --check --baseline tools/audit/globals.baseline
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
SCAN_PREFIXES = ("engine/src/", "editor/src/")
SRC_EXT = (".c", ".cc", ".cpp", ".cxx")
SKIP_FRAGMENTS = ("/third_party/", "/generated/")
VENDOR_PREFIXES = ("stb_", "miniaudio", "dr_", "cJSON", "cgltf", "tinyexr")

# A file-scope (column-0) declaration of a mutable variable.  We deliberately
# require column 0 so function-local statics (always indented in this tree)
# are excluded, and skip const / constexpr / typedef / function definitions.
# The separator between the type and the name used to be a bare `\s`, which
# silently excluded every pointer written in the dominant C style -- `static
# SDL_Window *s_win;` has no whitespace between the `*` and the name, so the
# match failed and the declaration was never counted.  Measured 2026-09-01:
# 88 file-scope mutable pointers were invisible, among them s_play_runtime,
# s_play_audio and s_play_streamer -- pointers to owned subsystems, which is
# the single most on-point kind of state for a checker named
# check_singleton_ownership.  The separator now accepts whitespace OR "the
# type already ended in a sigil".
#
# The obvious fix -- taking `*` and `&` out of the type class and matching them
# as their own group -- was tried and REJECTED by its own measurement: it lost
# `std::vector<FolderPickRequest *> g_pending_pick_requests`, because the star
# there belongs to the template argument, not to the variable.  A fix that
# trades 83 finds for 1 loss is still a fix that loses one.
DECL_RE = re.compile(
    r"^(?P<quals>(?:static\s+|_Thread_local\s+|thread_local\s+|extern\s+)*)"
    r"(?P<type>[A-Za-z_][\w\s\*\&:<>,]*?)"
    r"(?:\s+|(?<=[\*\&]))"
    r"(?P<name>[A-Za-z_]\w*)\s*"
    r"(?P<tail>(?:\[[^\]]*\])*)\s*"
    r"(?:=|;)"
)

# `static struct Tag { ... } name;` spanning lines.  DECL_RE is single-line
# and matches neither half of it: the opening line ends in `{`, not `=` or
# `;`, and the closing line does not start with `static`.  That made the most
# idiomatic way to hold module state in C invisible to a state inventory --
# found 2026-09-03 when collapsing three scalars into one struct dropped the
# count by three instead of raising it by one.  Only one such declaration
# existed in the tree at the time, which is why it went unnoticed: the hole
# was real but nothing had fallen into it yet.
AGGREGATE_OPEN = re.compile(
    r"^(?P<quals>(?:static\s+|_Thread_local\s+|thread_local\s+)*)"
    r"(?P<kw>struct|union)\s+(?P<tag>[A-Za-z_]\w*)?\s*\{\s*$"
)
AGGREGATE_CLOSE = re.compile(
    r"^\}\s*(?P<name>[A-Za-z_]\w*)\s*(?P<tail>(?:\[[^\]]*\])*)\s*(?:=|;)"
)

INTEREST = re.compile(
    r"(?:^|_)(?:current|active|global|singleton|instance|registry|cache|pool|"
    r"table|manager|state|ctx|context|world|scene|selection|project)(?:$|_)",
    re.I,
)


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
        if any(f in ("/" + rp) for f in SKIP_FRAGMENTS):
            continue
        if Path(rp).name.startswith(VENDOR_PREFIXES):
            continue
        files.append(rp)
    return files


def _line_starts(text: str) -> list[int]:
    starts = [0]
    for i, ch in enumerate(text):
        if ch == "\n":
            starts.append(i + 1)
    return starts


def anon_ns_line_set(text: str) -> set[int]:
    """1-indexed line numbers inside a C++ anonymous namespace (internal
    linkage).  A variable declared there is file-local, not shared state."""
    # Neutralise comments and string/char literals so brace counting is sound.
    t = re.sub(r"/\*.*?\*/", lambda m: " " * len(m.group()), text, flags=re.S)
    t = re.sub(r"//[^\n]*", "", t)
    t = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', t)
    t = re.sub(r"'(?:\\.|[^'\\\n])*'", "''", t)
    starts = _line_starts(text)

    def line_of(off: int) -> int:
        lo, hi = 0, len(starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if starts[mid] <= off:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    inside: set[int] = set()
    for m in re.finditer(r"\bnamespace\s*\{", t):
        brace = m.end() - 1  # index of the opening '{'
        depth = 0
        j = brace
        n = len(t)
        while j < n:
            c = t[j]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        for ln in range(line_of(brace), line_of(min(j, n - 1)) + 1):
            inside.add(ln)
    return inside


def module_of(rp: str) -> str:
    parts = rp.split("/")
    # engine/src/<layer>/<module>/...  or  editor/src/<area>/...
    if rp.startswith("engine/src/") and len(parts) >= 4:
        return "/".join(parts[2:4])
    if rp.startswith("editor/src/") and len(parts) >= 3:
        return "/".join(parts[1:3])
    return "/".join(parts[:3])


def scan() -> list[dict]:
    found: list[dict] = []
    for rp in tracked_files():
        try:
            text = (REPO_ROOT / rp).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        lines = text.splitlines()
        # C++ anonymous-namespace bodies have internal linkage: a variable
        # declared there is file-local, exactly like `static`.
        anon = anon_ns_line_set(text) if rp.endswith((".cc", ".cpp", ".cxx")) else set()
        pending_aggregate = None   # (line, quals, kw, tag)
        for i, line in enumerate(lines, start=1):
            if pending_aggregate is not None:
                mc = AGGREGATE_CLOSE.match(line)
                if mc:
                    a_line, a_quals, a_kw, a_tag = pending_aggregate
                    pending_aggregate = None
                    a_name = mc.group("name")
                    a_static = "static" in a_quals or a_line in anon
                    if a_static or a_name.startswith(("g_", "gs_", "s_")):
                        found.append({
                            "file": rp, "line": a_line, "module": module_of(rp),
                            "name": a_name,
                            "type": (a_kw + " " + (a_tag or "<anonymous>")),
                            "static": a_static, "extern": False,
                            "interesting": bool(INTEREST.search(a_name)),
                            "decl": ("%s%s %s { ... } %s;" %
                                     (a_quals, a_kw, a_tag or "", a_name))[:120],
                        })
                    continue
                if line.startswith("}"):
                    pending_aggregate = None   # a plain type definition
                continue
            ma = AGGREGATE_OPEN.match(line)
            if ma:
                pending_aggregate = (i, ma.group("quals"), ma.group("kw"),
                                     ma.group("tag"))
                continue
            if not line or line[0].isspace() or line[0] in "#/}{":
                continue
            code = line.split("//", 1)[0]
            if "static" not in code and not code.startswith(("g_", "gs_")):
                # non-static file-scope definitions are rarer; still catch g_*.
                pass
            m = DECL_RE.match(code)
            if not m:
                continue
            quals = m.group("quals")
            typ = m.group("type")
            name = m.group("name")
            if "(" in code[: m.end()]:
                continue  # function
            if re.search(r"\b(const|constexpr|typedef)\b", quals + " " + typ):
                continue
            if typ.strip() in ("return", "else", "case", "goto", "break"):
                continue
            # Only file-scope statics or explicit g_-prefixed globals.
            # A declaration inside an anonymous namespace has internal
            # linkage, so treat it as `static` (not shared state).
            is_static = "static" in quals or i in anon
            is_extern = "extern" in quals
            if not is_static and not is_extern and not name.startswith(("g_", "gs_", "s_")):
                continue
            found.append({
                "file": rp, "line": i, "module": module_of(rp),
                "name": name, "type": typ.strip(),
                "static": is_static, "extern": is_extern,
                "interesting": bool(INTEREST.search(name)),
                "decl": code.strip()[:120],
            })
    return found


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--write-baseline", metavar="FILE")
    ap.add_argument("--baseline", metavar="FILE")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    found = scan()
    total = len(found)

    if args.write_baseline:
        Path(args.write_baseline).write_text(str(total) + "\n", encoding="utf-8")
        print(f"wrote baseline {total} to {args.write_baseline}")
        return 0

    if args.json:
        print(json.dumps(found, indent=2))
    else:
        # A genuine cross-module shared global has EXTERNAL linkage (an `extern`
        # declaration or a non-static definition) under the same identifier in
        # more than one module.  Coincidental same-named `static` file-locals are
        # separate storage and are NOT reported here.
        by_name: dict[str, list[dict]] = defaultdict(list)
        for f in found:
            by_name[f["name"]].append(f)
        print(f"== file-scope mutable global/static state: {total} declaration(s) ==\n")
        shared = {}
        for name, v in by_name.items():
            if len({x["module"] for x in v}) <= 1:
                continue
            # Require >=2 externally-linked occurrences: either one linked
            # symbol shared across modules (def + extern) or a genuine ODR
            # collision (two non-static defs).  One external + a coincidental
            # file-local `static` elsewhere is NOT shared state.
            ext = sum(1 for x in v if x["extern"] or not x["static"])
            if ext < 2:
                continue
            shared[name] = v
        if shared:
            print("-- cross-module SHARED mutable global (external linkage, un-owned state smell) --")
            for k, v in sorted(shared.items()):
                mods = sorted({x["module"] for x in v})
                print(f"  '{k}'  ({len(v)} decls across {len(mods)} modules)")
                for x in sorted(v, key=lambda y: y["file"]):
                    kind = "extern" if x["extern"] else ("def" if not x["static"] else "static")
                    print(f"      {x['file']}:{x['line']}  [{kind}]  {x['decl']}")
            print()
        print("-- notable holders (current/registry/cache/pool/world/scene/...) --")
        interesting = [f for f in found if f["interesting"]]
        for f in sorted(interesting, key=lambda x: (x["module"], x["name"]))[:200]:
            print(f"  {f['file']}:{f['line']}  ({f['module']})  {f['name']}")
        print(f"\n  ...{len(interesting)} interesting of {total} total globals.")

    if args.check and args.baseline:
        try:
            base = int(Path(args.baseline).read_text(encoding="utf-8").strip())
        except (OSError, ValueError):
            print(f"error: cannot read baseline {args.baseline}", file=sys.stderr)
            return 2
        if total > base:
            print(f"singleton-ownership check: FAILED — {total} globals > baseline {base} "
                  f"(+{total - base} new). Declare an owner or lower the count.")
            return 1
        print(f"singleton-ownership check: OK ({total} <= baseline {base}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
