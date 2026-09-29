#!/usr/bin/env python3
"""
check_api_closure.py — AGENTS.md §4's promise, as a gate that can fail.

§4 says: "用户代码只需 #include <jce/api.h> 即可获得整套引擎".  That sentence is
the whole contract between this engine and everyone who builds on it, and it
was measured but never enforced.  check_editor_consumption.py:413 PRINTS
"API CLOSURE GAP: N public headers unreachable from <jce/api.h>" and then
appends nothing to its failure list, so the number could drift forever with
run_all.py green.  It did: CLAUDE.md §3 recorded 111 of 299 unreachable on
2026-08-28, and re-measuring on 2026-08-31 gave exactly 111 of 299 again.

WHAT WAS ACTUALLY WRONG, because it was not what it looked like.  The obvious
diagnosis -- "api.h forgot to include some umbrellas" -- was false.  api.h did
omit three (api_input.h, api_middleware.h, api_ai_dispatch.h), but wiring all
twenty umbrellas in recovered only THREE real headers.  The other 105 were
missing from the umbrellas THEMSELVES: api_ai.h shipped 2 of 10 AI headers
while §4's own table names navmesh and steering; api_render.h omitted SSAO,
SSR and TAA; api_audio.h omitted the mixer, occlusion and reverb; api_scene.h
omitted prefab and sequencer; middleware/save and middleware/video had no
umbrella at all.  100 of the 105 declared JCE_API symbols, so they were not
internal headers that had wandered in -- they were shipped, exported
capability that the documented entry point could not deliver.

THE RULE THIS ENFORCES.  Every header under engine/include/jce/ is reachable
from <jce/api.h>, or it is listed in api_closure_exempt.txt WITH A REASON on
the same line.  A bare path with no reason is not an exemption -- the same
fail-closed shape editor_consumption_exempt.txt uses, so "I'll write the
reason later" cannot quietly become permanent.

There are exactly two exemptions today and both are structural rather than
negotiable: a consumer main() template that would inject definitions into
every translation unit, and an alias FOR api.h that would be a cycle.

USAGE
    python tools/lint/check_api_closure.py            # gate (exit 0/1)
    python tools/lint/check_api_closure.py --list     # what is reachable
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
INCLUDE_ROOT = REPO_ROOT / "engine" / "include"
JCE_ROOT = INCLUDE_ROOT / "jce"
ENTRY = JCE_ROOT / "api.h"
EXEMPT_FILE = Path(__file__).resolve().parent / "api_closure_exempt.txt"

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.M)

EXIT_SKIPPED = 2


def load_exempt() -> dict[str, str]:
    """{relpath: reason}.  A line without a '#' reason is IGNORED, not honoured.

    Same shape as editor_consumption_exempt.txt: an exemption is a decision
    someone has to defend in writing, and a gate that accepted bare paths
    would collect them until it enforced nothing.
    """
    out: dict[str, str] = {}
    if not EXEMPT_FILE.is_file():
        return out
    for raw in EXEMPT_FILE.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "#" not in line:
            continue                      # no reason -> not an exemption
        path, reason = line.split("#", 1)
        path, reason = path.strip(), reason.strip()
        if path and reason:
            out[path] = reason
    return out


def closure(entry: Path) -> set[Path]:
    """Every header transitively included from `entry`.

    Textual, not preprocessed: it follows every #include regardless of #if,
    which is the RIGHT over-approximation here.  A header behind a platform
    guard is still part of the surface a consumer on that platform gets, and a
    gate that resolved conditionals would answer a different question on every
    machine.
    """
    seen: set[Path] = set()
    stack = [entry]
    while stack:
        p = stack.pop()
        if p in seen or not p.is_file():
            continue
        seen.add(p)
        try:
            text = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for m in INCLUDE_RE.finditer(text):
            target = m.group(1)
            for cand in (INCLUDE_ROOT / target, p.parent / target):
                cand = Path(os.path.normpath(cand))
                if cand.is_file():
                    stack.append(cand)
                    break
    return seen


def public_headers() -> set[Path]:
    return {p for p in JCE_ROOT.rglob("*.h")} | {p for p in JCE_ROOT.rglob("*.hpp")}


def rel(p: Path) -> str:
    return str(p.relative_to(JCE_ROOT)).replace("\\", "/")


PRIVATE_INSTALL_CMAKE = REPO_ROOT / "engine" / "cmake" / "JCESDKInstall.cmake"

_IF_NOT_RE = re.compile(r"^\s*if\s*\(\s*NOT\s+(JCE_ENABLE_\w+)\s*\)", re.M)
_PATTERN_RE = re.compile(r'PATTERN\s+"([^"]+)"\s+EXCLUDE')
_ENDIF_RE = re.compile(r"^\s*endif\s*\(", re.M)


def private_modules() -> dict[str, str]:
    """{header-path-fragment: JCE_ENABLE_* option that must guard it}.

    Read from JCESDKInstall.cmake rather than hardcoded, so a SECOND private
    module gets policed the day someone adds it.  ai_dispatch is the only one
    today, and it is the reason this check exists: api.h grew an unconditional
    #include <jce/api_ai_dispatch.h>, which compiles perfectly in-tree and
    then breaks EVERY SDK consumer on the first line of api.h, because the
    installer excludes that header whenever the option is OFF -- the default.
    The in-tree build cannot see this; only a full SDK smoke can, and a smoke
    is far too slow to run on every header edit.
    """
    if not PRIVATE_INSTALL_CMAKE.is_file():
        return {}
    text = PRIVATE_INSTALL_CMAKE.read_text(encoding="utf-8", errors="replace")
    out: dict[str, str] = {}
    for m in _IF_NOT_RE.finditer(text):
        opt = m.group(1)
        end = _ENDIF_RE.search(text, m.end())
        block = text[m.end():end.start() if end else len(text)]
        for pat in _PATTERN_RE.findall(block):
            out[pat.replace("\\", "/").strip("/")] = opt
    return out


def _is_private(inc: str, frag: str) -> bool:
    inc = inc.strip("/")
    return inc == frag or inc.endswith("/" + frag) or (frag + "/") in (inc + "/")


def unguarded_private_includes(priv: dict[str, str]) -> list[str]:
    """Public headers that include a private-module header OUTSIDE its guard.

    Tracks #if/#endif depth and remembers at which depth a guard for the
    relevant option opened.  Deliberately simple: it only recognises the house
    form `#if defined(OPT) && OPT`, which is what api_middleware.h has always
    used.  A different spelling reads as unguarded -- that is the safe
    direction for a gate to be wrong in.
    """
    bad: list[str] = []
    for p in sorted(public_headers()):
        # A header that is ITSELF excluded from the SDK may include its own
        # siblings freely -- it never ships, so it can never break a consumer.
        # The invariant is one-directional: a header that DOES ship must not
        # include one that does not.
        if any(_is_private(rel(p), frag) for frag in priv):
            continue
        depth = 0
        open_at: dict[str, int] = {}
        for lineno, line in enumerate(
                p.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            st = line.strip()
            if st.startswith("#if"):
                depth += 1
                for opt in set(priv.values()):
                    if ("defined(%s)" % opt) in st and opt not in open_at:
                        open_at[opt] = depth
                continue
            if st.startswith("#endif"):
                for opt in [o for o, d in open_at.items() if d == depth]:
                    del open_at[opt]
                depth = max(0, depth - 1)
                continue
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            inc = m.group(1)
            if inc.startswith("jce/"):
                inc = inc[4:]
            for frag, opt in priv.items():
                if _is_private(inc, frag) and opt not in open_at:
                    bad.append("%s:%d: #include <jce/%s> is not inside "
                               "#if defined(%s) -- the SDK installer excludes "
                               "it when that option is OFF (the default), so "
                               "this breaks every SDK consumer"
                               % (rel(p), lineno, inc, opt))
    return bad


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    if not ENTRY.is_file():
        print(f"api-closure check: SKIPPED — {ENTRY} is not present.")
        return EXIT_SKIPPED

    allh = public_headers()
    reach = closure(ENTRY) & allh
    exempt = load_exempt()
    unreachable = sorted(rel(p) for p in (allh - reach))

    if args.list:
        print(f"api-closure: {len(reach)}/{len(allh)} public headers reachable "
              f"from <jce/api.h>")
        for r in sorted(rel(p) for p in reach):
            print(f"  {r}")
        return 0

    missing = [r for r in unreachable if r not in exempt]
    stale = [r for r in exempt if r not in unreachable]
    priv = private_modules()
    unguarded = unguarded_private_includes(priv)

    if not missing and not stale and not unguarded:
        print(f"api-closure check: OK — {len(reach)}/{len(allh)} public headers "
              f"reachable from <jce/api.h>; {len(exempt)} exempted with a "
              f"stated reason; {len(priv)} private-module path(s) correctly "
              f"guarded.")
        return 0

    print("api-closure check: FAILED")
    print()
    if missing:
        print(f"Public headers unreachable from <jce/api.h> ({len(missing)}):")
        for r in missing:
            print(f"  {r}")
        print()
        print("  §4 says a user gets the whole engine from <jce/api.h>. Add the")
        print("  header to the api_*.h umbrella that covers its directory, or")
        print("  add it to tools/lint/api_closure_exempt.txt with a reason on")
        print("  the same line saying why it must not be reachable.")
        print()
    if stale:
        # An exemption for a header that IS reachable is not harmless: it is a
        # standing claim that something must stay out, contradicted by the
        # tree, and the next person to read it will believe the claim.
        print(f"Exemptions that no longer apply ({len(stale)}) — these headers "
              f"ARE reachable now, so the exemption is stale:")
        for r in stale:
            print(f"  {r}   # {exempt[r]}")
        print()
    if unguarded:
        print(f"Private-module headers included without their guard "
              f"({len(unguarded)}):")
        for u in unguarded:
            print(f"  {u}")
        print()
        print("  Wrap it the way api_middleware.h does:")
        print("      #if defined(JCE_ENABLE_XXX) && JCE_ENABLE_XXX")
        print("      #include <jce/...>")
        print("      #endif")
        print()
    return 1


if __name__ == "__main__":
    sys.exit(main())
