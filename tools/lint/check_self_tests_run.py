#!/usr/bin/env python3
"""
check_self_tests_run.py — a self-test with no caller is a claim nobody checked.

On 2026-08-31 this repo had seven `jce_*_self_test` functions and ZERO call
sites.  515 lines of verification, written deliberately, never once executed.
Wiring the runnable ones found real defects in three of them on their first
ever run:

  * jce_rg_self_test          — passed NULL as the pass callback, which
                                jce_rg_add_pass rejects, so every pass was
                                silently dropped and compile() then reported
                                success on an empty graph.  The self-test was
                                broken; the render graph itself was fine.
  * jce_coroutine_self_test   — a REAL engine bug.  The handle is
                                (generation << 32) | slot and
                                JCE_COROUTINE_INVALID is 0, so the first
                                coroutine after init packed to 0: a successful
                                start indistinguishable from failure, reported
                                not-alive while running, and impossible to
                                cancel.  Caught on its first assertion.
  * jce_net_replication_self_test — a REAL engine bug.  The sweep that reaps
                                despawned objects sat inside
                                `if (role == SERVER && host)`, so with no host
                                every despawn leaked a table slot forever.

Two of those three were production bugs sitting behind verification that
already existed and only needed to be run.  That is what this gate protects.

THE RULE.  Every `jce_*_self_test` defined under engine/src/ has a call site
outside its own file, or is listed in self_test_exempt.txt WITH A REASON on
the same line.  A bare path with no reason is ignored, not honoured.

UNAVAILABLE, not a pass, when engine/src/ or tests/ is absent. Missing source
cannot certify that the self-tests have callers.

The maintained working tree is enumerated through lint_git_files, including
new untracked source and excluding ignored generated caches. One local index
reads each source and strips its comments once for all queried symbols. It is
never cached across invocations or repositories. This is a lexical caller
gate, not proof that a test target executes the matched call.

USAGE
    python tools/lint/check_self_tests_run.py
"""

from __future__ import annotations

import importlib.util
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ENGINE_SRC = REPO_ROOT / "engine" / "src"
EXEMPT_FILE = Path(__file__).resolve().parent / "self_test_exempt.txt"

# Where a call may live.  tests/ is the expected home; the others are here so
# that wiring a self-test into a tool or the editor also counts.
CALLER_TREES = ("tests", "editor/src", "tools", "engine/src")

SUFFIXES = (".c", ".h", ".cpp", ".hpp")
VENDOR = "/third_party/"

_spec = importlib.util.spec_from_file_location(
    "lint_git_files", Path(__file__).with_name("lint_git_files.py"))
_lgf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_lgf)

# A DEFINITION: `<type> [JCE_CALL] jce_x_self_test(void)` with no trailing ';'.
DEF_RE = re.compile(
    r"^[A-Za-z_][A-Za-z0-9_ *]*?\b(jce_[a-z0-9_]*_self_test)\s*\(\s*void\s*\)\s*$",
    re.M)

EXIT_SKIPPED = 2
CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")


class SourceIndex:
    """One maintained-source snapshot, owned by one checker invocation."""

    def __init__(self) -> None:
        self.definitions: dict[str, str] = {}
        self.caller_files: dict[str, list[str]] = {}
        self.unavailable_sources: list[str] = []


def strip_comments(text: str) -> str:
    text = re.sub("/[*].*?[*]/", " ", text, flags=re.S)
    return re.sub("//[^" + chr(92) + "n]*", " ", text)


def source_index() -> SourceIndex:
    """Read publication candidates once; ignored caches are not callers."""
    index = SourceIndex()
    prefixes = tuple(tree + "/" for tree in CALLER_TREES)
    for rel in sorted(_lgf.working_tree()):
        if not rel.startswith(prefixes) or Path(rel).suffix not in SUFFIXES:
            continue
        if VENDOR in "/" + rel or "/vendor/" in "/" + rel:
            continue
        path = REPO_ROOT / rel
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            index.unavailable_sources.append(rel)
            continue
        if rel.startswith("engine/src/"):
            for match in DEF_RE.finditer(text):
                index.definitions.setdefault(match.group(1), rel)
        for symbol in set(CALL_RE.findall(strip_comments(text))):
            index.caller_files.setdefault(symbol, []).append(rel)
    return index


def load_exempt() -> dict[str, str]:
    out: dict[str, str] = {}
    if not EXEMPT_FILE.is_file():
        return out
    for raw in EXEMPT_FILE.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "#" not in line:
            continue
        name, reason = line.split("#", 1)
        if name.strip() and reason.strip():
            out[name.strip()] = reason.strip()
    return out


def definitions(index: SourceIndex | None = None) -> dict[str, str]:
    """{symbol: repo-relative file that defines it}."""
    return (source_index() if index is None else index).definitions.copy()


def callers(symbol: str, own_file: str,
            index: SourceIndex | None = None) -> list[str]:
    """Files OTHER than the definition's own that name the symbol.

    A self-declaration next to the definition -- `void jce_x_self_test(void);`
    on the line above the body, which four of these had -- is in the same file
    and so is correctly not a caller.  That pattern exists to silence a
    missing-prototype warning, and silencing that warning is how a function
    with no caller stops looking like one.
    """
    # A CALL, not a mention.  The first version matched the bare name and
    # counted jce_session.c:760 -- a comment reading "jce_lan_discovery_
    # self_test in jce_lan_discovery.h." -- as a caller, reporting 6 of 7
    # wired when 5 were.  Comments are stripped during indexing for this reason:
    # the call form still matches inside "call jce_x_self_test() from a debug
    # entry".  That is the failure mode this whole gate exists to prevent,
    # one level up.
    snapshot = source_index() if index is None else index
    return [rel for rel in snapshot.caller_files.get(symbol, ())
            if rel != own_file]


def main() -> int:
    if not ENGINE_SRC.is_dir():
        print("self-test-run check: UNAVAILABLE — engine/src is not present.")
        return EXIT_SKIPPED
    if not (REPO_ROOT / "tests").is_dir():
        print("self-test-run check: UNAVAILABLE — tests/ is not present; "
              "caller coverage cannot be verified. This is NOT a pass.")
        return EXIT_SKIPPED

    index = source_index()
    if index.unavailable_sources:
        print("self-test-run check: UNAVAILABLE — could not read maintained "
              "source; caller coverage cannot be verified:")
        for rel in index.unavailable_sources:
            print("  " + rel)
        return EXIT_SKIPPED
    defs = definitions(index)
    if not defs:
        print("self-test-run check: UNAVAILABLE — found 0 *_self_test definitions; "
              "the pattern no longer matches, fix the extractor before "
              "trusting this gate.")
        return EXIT_SKIPPED

    exempt = load_exempt()
    unwired: list[str] = []
    wired = 0
    for sym in sorted(defs):
        if callers(sym, defs[sym], index):
            wired += 1
        elif sym not in exempt:
            unwired.append("%s  (defined in %s)" % (sym, defs[sym]))

    stale = [s for s in exempt if s not in defs]

    if not unwired and not stale:
        print("self-test-run check: OK — %d of %d self-test(s) have a caller; "
              "%d exempted with a stated reason."
              % (wired, len(defs), len(exempt)))
        return 0

    print("self-test-run check: FAILED")
    print()
    if unwired:
        print("Self-tests nothing calls (%d):" % len(unwired))
        for u in unwired:
            print("  %s" % u)
        print()
        print("  Wire it into tests/ — a self-test that never runs is a claim")
        print("  nobody has checked, and on 2026-08-31 three of the five that")
        print("  could be run failed the first time they were.  If it cannot")
        print("  run in this suite (it needs a socket, a GPU, a device), add")
        print("  it to tools/lint/self_test_exempt.txt with that reason on")
        print("  the same line.")
        print()
    if stale:
        print("Exemptions for self-tests that no longer exist (%d):"
              % len(stale))
        for s in stale:
            print("  %s   # %s" % (s, exempt[s]))
        print()
    return 1


if __name__ == "__main__":
    sys.exit(main())
