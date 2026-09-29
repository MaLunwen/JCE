#!/usr/bin/env python3
"""
check_editor_consumption.py - prove the editor consumes the whole engine.

`check_editor_consumer_purity.py` already guards ONE direction: the editor must
not reach past `<jce/...>` into a third-party library.  It cannot see the other
direction, which is the one that rots quietly:

    the engine grows a capability, nothing in the editor ever calls it, and the
    feature ships "built but unwired" - present in the ABI, absent from every
    surface a human can reach.

This walks the public API from `<jce/api.h>`, collects every `JCE_API` function
it can reach, and asks whether any first-party consumer names it:

    consumed      some file under editor/src or editor/resources names it
    exempt        listed in editor_consumption_exempt.txt WITH a reason
    unconsumed    no editor file names it

`unconsumed` is what the ratchet trips on, because "the editor does not expose
this" is the thing this gate exists to notice.  But that phrase was being read
as "dead code", which it is not, so the report splits it five ways:

    userproj       a dogfooding user project names it - alive, but not
                   evidence that the EDITOR consumes it.  Empty by default;
                   driven by user_project_trees.txt, which ships with no
                   entries so this gate answers about the general surface
    engine         called from engine/src outside its own TU - internal, alive
    test/tool      named only under tests/ or tools/ - exercised, but reached
                   by no product
    via-macro      the call site names a public macro that expands to it, so no
                   identifier scan can see the call - alive
    NOWHERE        named by nothing on the GENERAL surface

Only NOWHERE is unqualified built-but-unwired debt.  The split matters more
than it sounds: repo-wide, `unconsumed` is 1612 and NOWHERE is 293.

"General surface" is literal: user-project trees are NOT scanned by default
(user_project_trees.txt ships empty), so a symbol only a game names lands in
NOWHERE.  That is the answer this gate is asked for -- whether the EDITOR
consumes the engine -- and a game naming it is not evidence that it does.

Each of those states was added because the previous headline was wrong: the
count went 1697 -> 1612 (exemptions) -> 467 (ck + engine) -> 252 (tests/tools)
-> 245 (macro wrappers) -> 293 (user projects left the scan surface, so the 94
they carried were re-bucketed most-alive-first and 48 landed here).  Every step was a question nobody had asked yet, not a
re-measurement.  A count of what is ABSENT is only as good as the list of
places you looked, so if you add a state, say why here.

WHY A RATCHET AND NOT A THRESHOLD.  The debt is real and large today; failing
on the absolute count would mean a permanently red gate, which is the same as
no gate.  The baseline file records today's per-umbrella unconsumed count, and
the gate fails only when an umbrella gets WORSE.  Deleting the baseline is not
a way to pass: a missing baseline is written, reported, and the run is marked
SKIPPED so nobody mistakes the first run for a verdict.

A header that `<jce/api.h>` cannot reach is reported separately as an API
CLOSURE gap: users are told `#include <jce/api.h>` is enough, so a public
header outside that closure is unreachable by the contract's own terms.

Usage:
    python tools/lint/check_editor_consumption.py            # gate
    python tools/lint/check_editor_consumption.py --report   # + write report
    python tools/lint/check_editor_consumption.py --update-baseline
Exit 0 clean / 1 regression / 2 preconditions missing (SKIPPED, never silent).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PUBLIC_ROOT = REPO_ROOT / "engine" / "include" / "jce"
UMBRELLA = PUBLIC_ROOT / "api.h"
BASELINE = Path(__file__).resolve().parent / "editor_consumption_baseline.json"
EXEMPT = Path(__file__).resolve().parent / "editor_consumption_exempt.txt"

# Consumers that count as "the editor exercises this".  The bundled game
# modules ship inside the editor and are written against <jce/api.h>, so a
# symbol only they reach is still reached through the public contract.
CONSUMER_DIRS = [
    REPO_ROOT / "editor" / "src",
    REPO_ROOT / "editor" / "resources",
]

# "Unconsumed by the editor" is not the same claim as "unconsumed".  Read
# alone it printed a game-facing runtime API as though it were dead code, and
# an internal facility the renderer calls every frame as though nobody wanted
# it.  Measured on two umbrellas: of api_ui.h's 67 editor-unconsumed symbols,
# 45 are called by caged_kingdom; of api_animation.h's 62, 55 have a caller
# inside engine/src.  Reading either set as debt would have been wrong by
# roughly 8x.  So the report separates four states and only the last one --
# referenced by nobody, anywhere -- is evidence of built-but-unwired debt.
#
# User-project trees are DATA, not code: the list lives in
# user_project_trees.txt and ships empty (owner decision, 2026-08-27 --
# general engine + editor only).  Keeping the mechanism costs nothing and
# keeps the 8x-misreading lesson recoverable; hardcoding a game's path here
# would put a user project inside a general gate.
USER_PROJECT_TREES = Path(__file__).resolve().parent / "user_project_trees.txt"


def load_user_project_dirs():
    if not USER_PROJECT_TREES.exists():
        return []
    out = []
    for line in USER_PROJECT_TREES.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.append(REPO_ROOT / Path(line))
    return out


USER_PROJECT_DIRS = load_user_project_dirs()

# The first version of the four-state split scanned neither tests/ nor tools/,
# and reported 467 symbols as referenced nowhere.  Measured afterwards: 215 of
# those 467 (46%) are named by tests/, and one by tools/.  Left uncorrected the
# headline number would have been wrong by 1.9x -- the same class of error the
# split was introduced to fix, one level down.
#
# Test coverage is not product consumption, so these do not become "consumed";
# they get their own state.  "Exercised by a test but reached by no product" is
# real debt, just a milder kind than "nothing anywhere names this".
SECONDARY_DIRS = [
    REPO_ROOT / "tests",
    REPO_ROOT / "tools",
]

SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".hpp", ".cc", ".inc", ".mm", ".m"}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]jce/([^">]+)[">]', re.M)
# JCE_API <return type ...> jce_name(   -- return type may span pointers/space.
API_FN_RE = re.compile(r'\bJCE_API\b[^;(){}]*?\b(jce_[A-Za-z0-9_]+)\s*\(', re.S)
IDENT_RE = re.compile(r'\b(jce_[A-Za-z0-9_]+)\b')
# #define NAME(args) <body>   -- body scanned for jce_* it forwards to.
MACRO_DEF_RE = re.compile(
    r'^[ \t]*#[ \t]*define[ \t]+([A-Z_][A-Z0-9_]*)[ \t]*(?:\([^)]*\))?(.*)$', re.M)


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def api_closure():
    """Return (header_relpath -> owning umbrella, ordered umbrella list)."""
    owner = {}
    umbrellas = []
    if not UMBRELLA.exists():
        return owner, umbrellas

    # api.h includes umbrellas AND a few headers directly (jce_compat.h).  Seed
    # from BOTH: skipping the direct ones reported them as unreachable orphans,
    # which is the gate accusing the contract of a gap the contract does not
    # have.  A direct include is attributed to "api.h" itself.
    for rel in INCLUDE_RE.findall(read(UMBRELLA)):
        umbrellas.append(rel)
        stack = [rel]
        while stack:
            cur = stack.pop()
            if cur in owner:
                continue
            owner[cur] = rel
            path = PUBLIC_ROOT / cur
            if not path.exists():
                continue
            for nxt in INCLUDE_RE.findall(read(path)):
                if nxt not in owner:
                    stack.append(nxt)
    return owner, umbrellas


def public_symbols(owner):
    """symbol -> (defining header, owning umbrella)."""
    out = {}
    for rel, umbrella in owner.items():
        path = PUBLIC_ROOT / rel
        if not path.exists():
            continue
        for name in API_FN_RE.findall(read(path)):
            out.setdefault(name, (rel, umbrella))
    return out


def consumer_identifiers():
    """Every jce_* identifier that appears anywhere in the consumer trees.

    Read once into a set rather than grepping per symbol: 3k symbols x a tree
    walk each is minutes, the set intersection is milliseconds.
    """
    seen = set()
    for root in CONSUMER_DIRS:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if path.suffix.lower() in SOURCE_SUFFIXES and path.is_file():
                seen.update(IDENT_RE.findall(read(path)))
    return seen


def identifiers_under(roots):
    """Every jce_* identifier named anywhere under the given roots."""
    seen = set()
    for root in roots:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if path.suffix.lower() not in SOURCE_SUFFIXES or not path.is_file():
                continue
            if "third_party" in path.as_posix():
                continue
            seen.update(IDENT_RE.findall(read(path)))
    return seen


def macro_reached(candidates):
    """Subset of `candidates` reached only through a public macro wrapper.

    An identifier scan cannot see through `#define JCE_PROFILE_PLOT(n,v)
    jce_profile_plot(...)`: the call site names the macro, never the function.
    The whole jce_profile_* family read as dead for exactly this reason.  Small
    class today (7 symbols) but it recurs whenever a wrapper is added, so it is
    checked rather than remembered.
    """
    macro_body = {}
    for path in PUBLIC_ROOT.rglob("*"):
        if path.suffix.lower() not in {".h", ".hpp", ".inc"} or not path.is_file():
            continue
        text = read(path).replace("\\\n", " ")      # join line continuations
        for name, body in MACRO_DEF_RE.findall(text):
            named = set(IDENT_RE.findall(body)) & candidates
            if named:
                macro_body.setdefault(name, set()).update(named)
    if not macro_body:
        return set()

    roots = (CONSUMER_DIRS + USER_PROJECT_DIRS + SECONDARY_DIRS
             + [REPO_ROOT / "engine" / "src"])
    reached = set()
    for root in roots:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if path.suffix.lower() not in SOURCE_SUFFIXES or not path.is_file():
                continue
            if "third_party" in path.as_posix():
                continue
            text = read(path)
            for name, syms in macro_body.items():
                if syms - reached and re.search(r"\b%s\b" % name, text):
                    reached |= syms
    return reached


def engine_identifier_files():
    """jce_* identifier -> set of engine/src basenames that name it.

    Basenames, not paths, so a symbol can be told apart from its own defining
    translation unit: jce_foo_bar declared in middleware/x/jce_x.h is expected
    to appear in jce_x.c, and that self-reference is not a caller.
    """
    out = {}
    root = REPO_ROOT / "engine" / "src"
    if not root.exists():
        return out
    for path in root.rglob("*"):
        if path.suffix.lower() not in SOURCE_SUFFIXES or not path.is_file():
            continue
        if "third_party" in path.as_posix():
            continue
        base = path.name
        for ident in IDENT_RE.findall(read(path)):
            out.setdefault(ident, set()).add(base)
    return out


def where_used(sym, header, project_idents, engine_files, test_idents):
    """Classify an editor-unconsumed symbol.

    Returns 'userproj' | 'engine' | 'test' | 'none', most-alive first.  'none' is the
    only one that means nothing anywhere names it.
    """
    if sym in project_idents:
        return "userproj"
    own = Path(header).name.replace(".h", ".c")
    if engine_files.get(sym, set()) - {own}:
        return "engine"
    if sym in test_idents:
        return "test"
    return "none"


def load_exempt():
    out = {}
    if not EXEMPT.exists():
        return out
    for line in read(EXEMPT).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        sym, _, reason = line.partition("#")
        sym = sym.strip()
        reason = reason.strip()
        if sym and reason:   # a bare symbol with no reason is NOT an exemption
            out[sym] = reason
    return out


def orphan_public_headers(owner):
    """Public headers that <jce/api.h> cannot reach."""
    orphans = []
    for path in sorted(PUBLIC_ROOT.rglob("*.h")):
        rel = path.relative_to(PUBLIC_ROOT).as_posix()
        if rel.startswith("api") or rel in owner:
            continue
        if "third_party" in rel or "internal" in rel:
            continue
        orphans.append(rel)
    return orphans


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", action="store_true", help="write reports/ markdown")
    ap.add_argument("--update-baseline", action="store_true")
    # An EPHEMERAL override for user_project_trees.txt.  That file ships empty
    # by owner decision (2026-08-27) and is TRACKED, so the documented way to
    # ask "is this symbol dead, or does my game call it?" was to edit a tracked
    # file -- in a repo where eleven worktrees share one .git, and where the
    # answer is a local question that must never become a commit.  This flag
    # asks the same question and leaves nothing behind.
    #
    # It cannot move the gate: see the comment on `unconsumed` below.  A symbol
    # is counted unconsumed BEFORE the user-project pass runs, and that pass
    # only chooses which sub-bucket it is displayed in.
    ap.add_argument("--user-projects", metavar="DIR[,DIR...]",
                    help="repo-relative user-project trees to classify "
                         "against, for THIS RUN only; overrides "
                         "user_project_trees.txt without touching it")
    args = ap.parse_args()

    if not UMBRELLA.exists():
        print("SKIPPED check_editor_consumption: %s not found" % UMBRELLA)
        return 2
    if not any(d.exists() for d in CONSUMER_DIRS):
        print("SKIPPED check_editor_consumption: no consumer tree "
              "(editor/src) in this worktree")
        return 2

    owner, umbrellas = api_closure()
    symbols = public_symbols(owner)
    if not symbols:
        print("SKIPPED check_editor_consumption: extracted 0 public symbols - "
              "the JCE_API pattern no longer matches; fix the extractor before "
              "trusting this gate")
        return 2

    used = consumer_identifiers()
    exempt = load_exempt()

    # An exemption for a symbol that is no longer on the public surface is a
    # standing claim about the tree that the tree contradicts, and it silently
    # re-authorises whatever symbol later takes that name.  Measured clean
    # (0 of 115) when this was added on 2026-08-31, right after removing one
    # that went stale the same day -- jce_rg_set_base_view_id left the public
    # API when the render graph header was demoted to engine/src.
    stale_exempt = sorted(e for e in exempt if e not in symbols)
    if stale_exempt:
        print("check_editor_consumption: FAILED - %d exemption(s) name a "
              "symbol that is no longer public:" % len(stale_exempt))
        for e in stale_exempt:
            print("  %s   # %s" % (e, exempt[e]))
        print()
        print("  Remove the line, or restore the symbol.  Either is fine; a "
              "claim about a symbol that does not exist is not.")
        return 1

    global USER_PROJECT_DIRS
    if args.user_projects:
        USER_PROJECT_DIRS = [REPO_ROOT / p.strip()
                             for p in args.user_projects.split(",")
                             if p.strip()]
        missing = [d for d in USER_PROJECT_DIRS if not d.exists()]
        if missing:
            print("check_editor_consumption: --user-projects names %d "
                  "director%s that do%s not exist:"
                  % (len(missing), "y" if len(missing) == 1 else "ies",
                     "es" if len(missing) == 1 else ""))
            for d in missing:
                print("  %s" % d.relative_to(REPO_ROOT).as_posix())
            print("  A typo here would silently answer 'nothing names it', "
                  "which is the exact wrong answer this flag exists to avoid.")
            return 1
        print("check_editor_consumption: --user-projects %s — THIS RUN ONLY; "
              "user_project_trees.txt is untouched and the per-umbrella "
              "unconsumed counts the baseline gates on are unaffected."
              % ", ".join(d.relative_to(REPO_ROOT).as_posix()
                          for d in USER_PROJECT_DIRS))
    project_idents = identifiers_under(USER_PROJECT_DIRS)
    test_idents = identifiers_under(SECONDARY_DIRS)
    engine_files = engine_identifier_files()

    per_umbrella = {}
    orphans_anywhere = []          # named by nobody: editor, ck, or engine
    for sym in sorted(symbols):
        header, umbrella = symbols[sym]
        bucket = per_umbrella.setdefault(
            umbrella, {"consumed": [], "exempt": [], "unconsumed": [],
                       "userproj": [], "engine": [], "test": [],
                       "macro": [],
                       "nowhere": []})
        if sym in used:
            bucket["consumed"].append(sym)
        elif sym in exempt:
            bucket["exempt"].append(sym)
        else:
            bucket["unconsumed"].append(sym)
            where = where_used(sym, header, project_idents, engine_files,
                               test_idents)
            bucket[where if where != "none" else "nowhere"].append(sym)
            if where == "none":
                orphans_anywhere.append((sym, header, umbrella))

    # Second pass: anything still "nowhere" may be reached through a macro.
    via_macro = macro_reached({s for s, _, _ in orphans_anywhere})
    if via_macro:
        for v in per_umbrella.values():
            moved = [s for s in v["nowhere"] if s in via_macro]
            if moved:
                v["nowhere"] = [s for s in v["nowhere"] if s not in via_macro]
                v["macro"].extend(moved)
        orphans_anywhere = [(s, h, u) for (s, h, u) in orphans_anywhere
                            if s not in via_macro]

    current = {u: len(v["unconsumed"]) for u, v in per_umbrella.items()}
    total = len(symbols)
    tot_unconsumed = sum(current.values())

    print("check_editor_consumption: %d public JCE_API symbols reachable from "
          "<jce/api.h> across %d umbrellas" % (total, len(umbrellas)))
    print("  consumed   : %d" % sum(len(v["consumed"]) for v in per_umbrella.values()))
    print("  exempt     : %d" % sum(len(v["exempt"]) for v in per_umbrella.values()))
    print("  unconsumed : %d  (user-project %d | engine-internal %d "
          "| test/tool-only %d "
          "| via-macro %d | NOWHERE %d)"
          % (tot_unconsumed,
             sum(len(v["userproj"]) for v in per_umbrella.values()),
             sum(len(v["engine"]) for v in per_umbrella.values()),
             sum(len(v["test"]) for v in per_umbrella.values()),
             sum(len(v["macro"]) for v in per_umbrella.values()),
             len(orphans_anywhere)))
    # The scope sentence has to follow the scope.  Printed unconditionally it
    # said "user projects are not scanned" on a run that had just scanned one.
    if USER_PROJECT_DIRS:
        scanned = ", ".join(d.relative_to(REPO_ROOT).as_posix()
                            for d in USER_PROJECT_DIRS)
        print("  ^ user-project and engine-internal are alive.  test/tool-only "
              "is exercised but reached by no product.  NOWHERE here means "
              "named by nothing in the general surface OR in %s." % scanned)
    else:
        print("  ^ user-project and engine-internal are alive.  test/tool-only "
              "is exercised but reached by no product.  NOWHERE is named by "
              "nothing on the GENERAL surface -- NO user project was scanned "
              "(user_project_trees.txt ships empty by owner decision), so a "
              "symbol only a game calls lands in NOWHERE too.  To ask that "
              "question without touching the tracked list: "
              "--user-projects caged_kingdom")

    # The <jce/api.h> closure is now OWNED and ENFORCED by
    # tools/lint/check_api_closure.py.  This block used to print the gap
    # here and fail on nothing, which is how 111 unreachable headers sat
    # unchanged from 2026-08-28 to 08-31 with run_all.py green.  It is
    # reduced to a pointer on purpose: two independent measurers of one
    # quantity is how this repo ended up with four different numbers for a
    # single fact in circulation at once.
    orphans = orphan_public_headers(owner)
    if orphans:
        print("  api.h closure: %d unreachable -- enforced by "
              "check_api_closure.py, run it for the list." % len(orphans))

    if args.report:
        out = REPO_ROOT / "reports"
        out.mkdir(exist_ok=True)
        dst = out / "editor_consumption_matrix.md"
        lines = ["# editor <-> engine consumption matrix", ""]
        lines.append("Generated by `tools/lint/check_editor_consumption.py`. "
                     "Method: extract `JCE_API` functions from the public headers "
                     "reachable from `<jce/api.h>`, then ask whether the identifier "
                     "appears anywhere under `editor/src` or `editor/resources`.")
        lines.append("")
        lines.append("An identifier match is a WEAK signal: it proves the name is "
                     "mentioned, not that the capability is reachable by a user. "
                     "Treat `consumed` as an upper bound and `unconsumed` as hard "
                     "evidence of a gap.")
        lines.append("")
        # Two percentages, because one of them lies on its own.  `consumed%` on
        # an umbrella whose symbols are ALL legitimately exempt reads 0.0% and
        # looks like total failure; `accounted%` on an umbrella nobody has
        # looked at reads 0.0% and looks like nothing was decided.  Together
        # they separate "not reached" from "deliberately not reached".
        lines.append("| umbrella | total | consumed | exempt | unconsumed "
                     "| …userproj | …engine | …test/tool | …macro | **…NOWHERE** "
                     "| consumed% | accounted% |")
        lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for u in sorted(per_umbrella):
            v = per_umbrella[u]
            n = len(v["consumed"]) + len(v["exempt"]) + len(v["unconsumed"])
            pct = 100.0 * len(v["consumed"]) / n if n else 0.0
            acc = (100.0 * (len(v["consumed"]) + len(v["exempt"])) / n
                   if n else 0.0)
            lines.append("| `%s` | %d | %d | %d | %d | %d | %d | %d | %d "
                         "| **%d** | %.1f%% | %.1f%% |"
                         % (u, n, len(v["consumed"]), len(v["exempt"]),
                            len(v["unconsumed"]), len(v["userproj"]),
                            len(v["engine"]), len(v["test"]), len(v["macro"]),
                            len(v["nowhere"]), pct, acc))
        lines.append("")
        lines.append("`userproj` = named by a dogfooding user project listed "
                     "in user_project_trees.txt (empty by default, so this "
                     "column is normally 0).  `engine` = called from "
                     "engine/src outside its own translation unit -- an "
                     "internal facility, alive.  `test/tool` = named only under "
                     "tests/ or tools/ -- exercised, but reached by no product. "
                     "**`NOWHERE` = named by nothing at all. Only that column "
                     "is unqualified built-but-unwired debt.**")
        lines.append("")
        if orphans:
            # Since 2026-08-31 the closure is complete and OWNED by
            # check_api_closure.py, which allows exactly the headers listed in
            # api_closure_exempt.txt WITH a stated reason.  Printing those two
            # under a heading that says "gap" would report a deliberate,
            # argued decision as an outstanding defect -- and a report that
            # cries gap at a closed gap teaches its readers to skip the
            # section.  Read the same file the gate reads, and say which it is.
            exempt_reasons = {}
            exempt_file = (Path(__file__).resolve().parent
                           / "api_closure_exempt.txt")
            if exempt_file.is_file():
                for raw in exempt_file.read_text(encoding="utf-8").splitlines():
                    ln = raw.strip()
                    if not ln or ln.startswith("#") or "#" not in ln:
                        continue
                    p, why = ln.split("#", 1)
                    if p.strip() and why.strip():
                        exempt_reasons[p.strip()] = why.strip()
            unexplained = [r for r in orphans if r not in exempt_reasons]
            if unexplained:
                lines.append("## API closure gap - public headers "
                             "`<jce/api.h>` cannot reach")
                lines.append("")
                lines.append("Users are told `#include <jce/api.h>` is enough. "
                             "A public header outside that closure is "
                             "unreachable by the contract's own terms. "
                             "`check_api_closure.py` fails on each of these.")
                lines.append("")
                for rel in unexplained:
                    lines.append("- `%s`" % rel)
                lines.append("")
            explained = [r for r in orphans if r in exempt_reasons]
            if explained:
                lines.append("## Outside `<jce/api.h>` on purpose - %d header(s)"
                             % len(explained))
                lines.append("")
                lines.append("Not a gap: each is listed in "
                             "`tools/lint/api_closure_exempt.txt` with the "
                             "reason it must not be reachable.")
                lines.append("")
                for rel in explained:
                    lines.append("- `%s` — %s" % (rel, exempt_reasons[rel]))
                lines.append("")
        # WHOLE-HEADER ORPHANS, surfaced separately because the flat NOWHERE
        # list buries them.  A header where EVERY public symbol is unused is a
        # different animal from a header that merely has an unused setter: it
        # is a subsystem nothing reaches, and on 2026-08-31 the one that was
        # investigated that way (jce_render_graph.h) turned out to be public
        # API that CANNOT be used -- no accessor from its resource handle, no
        # framebuffer ever bound.  Finding that required computing this ratio
        # by hand; it should not have.
        if orphans_anywhere:
            by_hdr_nowhere = {}
            for sym, hdr, _u in orphans_anywhere:
                by_hdr_nowhere.setdefault(hdr, []).append(sym)
            by_hdr_total = {}
            for hdr, _u in symbols.values():
                by_hdr_total[hdr] = by_hdr_total.get(hdr, 0) + 1
            whole = sorted(
                ((h, len(v)) for h, v in by_hdr_nowhere.items()
                 if by_hdr_total.get(h) == len(v)),
                key=lambda t: -t[1])
            if whole:
                lines.append("## Whole-header orphans - %d header(s), %d symbols"
                             % (len(whole), sum(n for _h, n in whole)))
                lines.append("")
                lines.append("EVERY public symbol these headers declare is "
                             "referenced by nothing. That is a subsystem "
                             "nothing reaches, not an unused setter, and it is "
                             "the shape worth investigating first: read the "
                             "implementation and ask whether it CAN be used, "
                             "not just whether it is.")
                lines.append("")
                for h, n in whole:
                    lines.append("- `%s` — all %d" % (h, n))
                lines.append("")

            lines.append("## Referenced NOWHERE - %d symbols"
                         % len(orphans_anywhere))
            lines.append("")
            lines.append("Nothing on the general surface names these: no "
                         "editor file, no engine/src file outside the "
                         "symbol's own translation unit, and nothing under "
                         "tests/ or tools/ either.")
            lines.append("")
            if USER_PROJECT_DIRS:
                lines.append("**Scope of this run:** %s was scanned too, so "
                             "these are named by nothing in the general "
                             "surface OR there."
                             % ", ".join(d.relative_to(REPO_ROOT).as_posix()
                                         for d in USER_PROJECT_DIRS))
            else:
                # Without this the number reads as "dead", and it is not.  The
                # comment at the top of this file records the same class of
                # error twice already -- reading editor-unconsumed as debt was
                # wrong by 8x, and omitting tests/ was wrong by 1.9x.  This is
                # the third instance, one level down, and it is stated rather
                # than fixed because the empty list is an owner decision.
                lines.append("**Scope of this run: NO user project was "
                             "scanned.** `user_project_trees.txt` ships empty "
                             "by owner decision (2026-08-27, general engine + "
                             "editor only), so a symbol that only a game calls "
                             "lands in this list too.  Measured 2026-08-31 "
                             "against the main user project: 405 -> 340, i.e. "
                             "**65 of these are called by caged_kingdom** (and "
                             "138 symbols in total move into the user-project "
                             "bucket).  Re-measure without touching the "
                             "tracked list:")
                lines.append("")
                lines.append("```")
                lines.append("python tools/lint/check_editor_consumption.py "
                             "--user-projects examples/caged_kingdom --report")
                lines.append("```")
                lines.append("")
                lines.append("That flag cannot move the gate: a symbol is "
                             "counted `unconsumed` before the user-project "
                             "pass runs, so the per-umbrella counts the "
                             "baseline ratchets on are identical either way "
                             "(2251 both ways, measured).")
            lines.append("")
            lines.append("This is the actionable list: wire it, delete it, or "
                         "exempt it with a reason.")
            lines.append("")
            for sym, header, umbrella in orphans_anywhere:
                lines.append("- `%s`  (`%s`, via `%s`)" % (sym, header, umbrella))
            lines.append("")

        for u in sorted(per_umbrella):
            un = per_umbrella[u]["unconsumed"]
            if not un:
                continue
            lines.append("## `%s` - %d unconsumed" % (u, len(un)))
            lines.append("")
            for sym in un:
                lines.append("- `%s`  (`%s`)" % (sym, symbols[sym][0]))
            lines.append("")
        with open(dst, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(lines) + "\n")
        print("  report -> %s" % dst.relative_to(REPO_ROOT).as_posix())

    if args.update_baseline or not BASELINE.exists():
        # Carry forward any "_"-prefixed metadata key.  JSON has no
        # comments, and a re-baseline that silently erased the note
        # explaining the last jump would leave the next reader with a
        # number and no story -- which is how a ratchet stops being
        # auditable.  The gate loop below only ever does
        # base.get(<umbrella name>), so these keys are inert data.
        keep = {k: v for k, v in json.loads(read(BASELINE) or "{}").items()
                if k.startswith("_")}
        with open(BASELINE, "w", encoding="utf-8", newline="\n") as fh:
            json.dump({**keep, **current}, fh, indent=2, sort_keys=True)
            fh.write("\n")
        if args.update_baseline:
            print("  baseline updated (%d umbrellas)" % len(current))
            return 0
        print("SKIPPED check_editor_consumption: no baseline existed; wrote "
              "today's counts to %s. This run is NOT a verdict - re-run to gate."
              % BASELINE.relative_to(REPO_ROOT).as_posix())
        return 2

    base = json.loads(read(BASELINE) or "{}")
    regressions = []
    for u in sorted(current):
        n = current[u]
        was = base.get(u)
        if was is None:
            regressions.append("%s: new umbrella with %d unconsumed symbols "
                               "(not in baseline)" % (u, n))
        elif n > was:
            regressions.append("%s: unconsumed %d -> %d (+%d)"
                               % (u, was, n, n - was))
        elif n < was:
            # THE OTHER DIRECTION, and it is not a courtesy.  A baseline left
            # above reality is slack: every symbol of the difference can stop
            # being consumed -- a capability can fall out of the editor --
            # with this gate green the whole way.  Six umbrellas were carrying
            # that slack when this branch was added, one of them by eleven.
            regressions.append("%s: unconsumed %d -> %d (-%d) -- IMPROVED, and "
                               "the baseline still says %d.  Re-run with "
                               "--update-baseline in the same commit: until "
                               "you do, %d symbols can stop being consumed "
                               "without this gate noticing."
                               % (u, was, n, was - n, was, was - n))
    if regressions:
        print("")
        print("check_editor_consumption: FAIL - the editor and the "
              "baseline disagree:")
        for r in regressions:
            print("  %s" % r)
        print("")
        print("Fix by exposing the capability in the editor, or add the symbol "
              "to %s WITH a reason on the same line."
              % EXEMPT.relative_to(REPO_ROOT).as_posix())
        return 1

    print("check_editor_consumption: OK (no umbrella got worse)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
