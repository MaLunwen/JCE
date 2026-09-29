#!/usr/bin/env python3
"""A TRACKED FILE MUST NOT SAY IT IS NOT IN THE TREE.

On 2026-09-20, 79 files under tests/ ended their header comment with

    NOT IN THE TREE: tests/ is gitignored on this branch.

73 of them were TRACKED.  Each asserted, from inside the repository, that it
was not in the repository.  The sentence is true of `main`, where
`git ls-files tests/` really does return nothing; it was copied onto a branch
that tracks tests/ and propagated file to file, because a header comment is
the cheapest thing to copy and the most expensive thing to re-verify.

IT WAS NOT HARMLESS.  The six files that were NOT tracked were the six
written by the one agent that BELIEVED the sentence: having read "this does
not go in the tree", it did not commit them, and they sat untracked on a
worktree eleven branches share -- exactly where a concurrent session removes
a file and nobody notices.  Four commit messages that day also carried the
claim, telling reviewers that evidence they could have run was unreproducible.

So the sentence was self-refuting in 73 places and load-bearing in 6, and
nothing in the build could tell the difference.  This checker can.

TWO RULES, and the split is the whole design:

  1. SELF-CLAIMS -- a comment line that OPENS with "NOT IN THE TREE:" or
     "NOTE ON PROVENANCE:".  Anchored, because that is the header idiom the
     defect lived in and because an unanchored search reads prose about other
     artifacts as a claim about this file.  Measured: the first draft flagged
     scripting/cpp/CMakeLists.txt for "C++ wrapper is not in the tree" -- a
     TRUE statement about a GENERATED file, inside the fatal-error message
     telling you to regenerate it.  A checker whose first finding is a
     correct sentence is a checker nobody will run twice.

  2. NAMED-PATH CLAIMS -- "<some/path.c> ... not in the tree", where that
     path IS tracked.  Just as wrong when the path belongs to someone else,
     and the form that rots fastest: the file gets committed later and the
     sentence about it does not.  git settles this one outright, so there is
     no heuristic left to get wrong.  Measured: this rule caught
     tools/lint/check_physics_debug_shipped.py, whose docstring said its own
     evidence file was unreachable -- written that morning, false by that
     afternoon.

WHAT IT DELIBERATELY DOES NOT DO: decide whether a path *should* be ignored.
That is per-branch, it is what `git check-ignore -v` is for, and a checker
that guessed would be reintroducing the original mistake in a new place.  It
only catches contradictions, which need no judgement.

A file that genuinely is untracked is free to say so, and saying so is
useful; untracked files are not scanned at all.

  python tools/lint/check_provenance_claims.py
  python tools/lint/check_provenance_claims.py --self-check
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

EXTS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inc", ".py", ".md", ".txt")

SELF_CLAIM = re.compile(
    r"^\s*(?:[*#]|//|/\*)?\s*"
    r"(?:NOT\s+IN\s+THE\s+TREE|NOTE\s+ON\s+PROVENANCE)\s*:",
    re.IGNORECASE,
)

SELF_CLAIM_PROSE = (
    re.compile(
        r"\bthis\s+file\s+is\s+(?:not\s+in\s+the\s+tree|untracked|gitignored)\b",
        re.IGNORECASE,
    ),
    re.compile(
        r"\ba\s+clean\s+clone\s+does\s+not\s+have\s+this\s+file\b",
        re.IGNORECASE,
    ),
)

NAMED_CLAIM = re.compile(
    r"([\w./+-]+\.(?:c|cc|cpp|cxx|h|hpp|inc|py|json|txt|md))"
    r"[^\n]{0,60}?\b(?:not\s+in\s+the\s+tree"
    r"|is\s+gitignored"
    r"|does\s+not\s+enter\s+the\s+repository)\b",
    re.IGNORECASE,
)


def tracked_files() -> list:
    """Every tracked path, from git.

    RETURNCODE IS CHECKED AND AN EMPTY RESULT IS A FAILURE, because this tree
    has already paid for both: a scanner that read "git did not start" as
    "the tree is empty" reported a clean sweep over nothing at all.
    """
    out = subprocess.run(
        ["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, text=True
    )
    if out.returncode != 0:
        raise RuntimeError(
            "git ls-files failed (exit %d): %s" % (out.returncode, out.stderr.strip())
        )
    paths = [p for p in out.stdout.split("\0") if p]
    if not paths:
        raise RuntimeError(
            "git ls-files returned nothing -- refusing to report a clean scan "
            "over an empty file list"
        )
    return paths


def scan(paths) -> list:
    """(path, lineno, line, why) for each contradiction.

    THE FILE THAT DEFINES THE PATTERN CANNOT BE SCANNED BY IT.  This module's
    docstring quotes the sentence it detects, and describes the false positive
    it avoids BY NAMING scripting/cpp/CMakeLists.txt -- so on the commit that
    added it, both rules fired on their own documentation.  The gate was green
    when it ran, because the file was still untracked and `git ls-files` does
    not see untracked files; it went red the moment committing made it
    visible to itself.  Same pre-commit blind spot CLAUDE.md records for
    find_duplicate_symbols.py, arriving through a checker written that hour.

    Skipping it is not an exemption -- an exemption is a rule declining to
    look at something it could judge.  A detector's description of its own
    pattern is not an instance of that pattern, and the only claims this file
    can make are about its own examples.  Keyed on __file__ so it cannot be
    widened later into a list.
    """
    tracked = set(paths)
    try:
        SELF = Path(__file__).resolve().relative_to(ROOT).as_posix()
    except ValueError:
        SELF = None
    found = []
    for rel in paths:
        if not rel.endswith(EXTS) or rel == SELF:
            continue
        try:
            text = (ROOT / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue  # deleted in the working tree; git still lists it
        for lineno, line in enumerate(text.split("\n"), 1):
            if SELF_CLAIM.match(line) or any(p.search(line) for p in SELF_CLAIM_PROSE):
                found.append((rel, lineno, line.strip(), "says THIS file is not tracked"))
                continue
            m = NAMED_CLAIM.search(line)
            if m and m.group(1) in tracked:
                found.append(
                    (rel, lineno, line.strip(), "says %s is not tracked" % m.group(1))
                )
    return found


def self_check() -> int:
    """Both directions, on real files, in a throwaway git repository.

    A checker that only proves it stays quiet has proved nothing -- this tree
    has shipped three gates that could not fail.  So every fixture here is
    paired: the two defects must be CAUGHT, and the three correct sentences
    must not be, because each of those is a true statement someone had a
    reason to write.
    """
    failures = []
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)

        def run(*a):
            return subprocess.run(a, cwd=d, capture_output=True, text=True)

        run("git", "init", "-q")
        run("git", "config", "user.email", "t@t")
        run("git", "config", "user.name", "t")

        claim = "/* NOT IN THE TREE: tests/ is gitignored on this branch. */\n"
        body = "int main(void){return 0;}\n"

        (d / "self_claim.c").write_text(claim + body)
        (d / "untracked.c").write_text(claim + body)
        (d / "assets.c").write_text(
            "/* Fixture media is gitignored -- the test skips when absent. */\n" + body
        )
        (d / "names_tracked.py").write_text(
            '"""Covered by self_claim.c (not in the tree; tests/ is gitignored)."""\n'
        )
        (d / "names_generated.txt").write_text(
            "the generated wrapper.c is not in the tree. Regenerate with: ...\n"
        )
        run("git", "add", "self_claim.c", "assets.c",
            "names_tracked.py", "names_generated.txt")
        run("git", "commit", "-qm", "x")

        out = subprocess.run(
            ["git", "ls-files", "-z"], cwd=d, capture_output=True, text=True
        )
        rels = [p for p in out.stdout.split("\0") if p]

        global ROOT
        real_root, ROOT = ROOT, d
        try:
            hits = {r for r, _, _, _ in scan(rels)}
        finally:
            ROOT = real_root

        if "self_claim.c" not in hits:
            failures.append(
                "POSITIVE CONTROL 1 FAILED: a tracked file opening with the "
                "claim was not caught -- the checker cannot fail on the defect "
                "it exists for"
            )
        if "names_tracked.py" not in hits:
            failures.append(
                "POSITIVE CONTROL 2 FAILED: a file calling a TRACKED path "
                "'not in the tree' was not caught -- rule 2 is inert"
            )
        if "untracked.c" in hits:
            failures.append(
                "an UNTRACKED file was reported; it is not scanned at all, so "
                "the scan set is wrong"
            )
        if "assets.c" in hits:
            failures.append(
                "a line about gitignored FIXTURE MEDIA was read as a claim "
                "about the file itself -- rule 1 is too broad"
            )
        if "names_generated.txt" in hits:
            failures.append(
                "a true statement about an UNTRACKED generated file was "
                "reported -- rule 2 is not checking git"
            )

    for f in failures:
        print("  self-check: " + f)
    print(
        "check_provenance_claims --self-check: %s (5 fixtures: 2 defects, 3 "
        "true sentences)" % ("FAIL" if failures else "OK")
    )
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        return self_check()

    paths = tracked_files()
    found = scan(paths)
    if found:
        print(
            "check_provenance_claims: FAIL - %d tracked file(s) contradict git:"
            % len({f for f, _, _, _ in found})
        )
        for rel, lineno, line, why in found[:40]:
            print("  %s:%d  [%s]" % (rel, lineno, why))
            print("      %s" % line[:110])
        if len(found) > 40:
            print("  ... and %d more" % (len(found) - 40))
        print(
            "\n  git already knows.  Either the sentence is wrong, or the file "
            "should not be\n  committed -- settle it with\n"
            "      git check-ignore -v <path>\n"
            "  which is per-branch, unlike a remembered rule about `main`."
        )
        return 1

    print(
        "check_provenance_claims: OK (%d tracked files scanned; none claims a "
        "tracked path is untracked)"
        % len([p for p in paths if p.endswith(EXTS)])
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
