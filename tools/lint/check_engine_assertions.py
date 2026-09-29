#!/usr/bin/env python3
"""Invariants must hold in the configuration that ships.

THE DEFECT THIS EXISTS FOR, found 2026-09-20 and measured rather than
imagined.  The tree's only invariant check was bare <assert.h>.  CMake's
default Release flags carry /DNDEBUG, so every one of them compiled to
nothing in the build that reaches a player.  That much was known.  What was
not is the second-order damage: whole SELF-TESTS sat inside `#ifndef NDEBUG`,
so in a Release build their functions did not EXIST as symbols, and the tests
that ran them could only be registered under

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")

Five of them were in that state.  Both CMakeLists said out loud that a
Release registration "would be a test that passes without checking anything"
-- honest, and still a hole, because the invariants then ran in exactly one
configuration: the one nobody ships.  Two of the P0s this tree has found came
out of one of those self-tests on its first ever run.

MEASURED, with the mutation control both ways: three deliberately-false
assertions inside the net self-tests gave 452/452 GREEN before the repair
(the tests were not registered at all) and three RED afterwards, in Release.

TWO RULES, and the second is the one that is easy to lose again:

  A. No bare assert( in first-party engine code.  Use JCE_ASSERT / JCE_ENSURE
     / JCE_VERIFY from <jce/os/core/jce_assert.h>, which NDEBUG does not
     strip.  Comments and string literals are stripped before matching, so a
     checker cannot be satisfied -- or tripped -- by prose.

  B. A file that defines a *_self_test must not contain an NDEBUG
     conditional.  NOT a blanket ban on NDEBUG: asking "is this a debug
     build?" is legitimate for a diagnostic default or a tracker that costs
     real memory, and two such uses are baselined below.  What is banned is
     the COMBINATION -- tying whether a self-test exists to the optimisation
     level, which is exactly how five of them ended up unrunnable.

Run with --self-check to prove the rules can still fail; a gate only verified
against a green tree has not been verified.
"""
import io
import json
import re
import subprocess
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]
BASELINE = LINT_DIR / "engine_assertions_baseline.json"

SCAN_ROOTS = ("engine/src", "engine/include")
EXTS = (".c", ".h", ".cpp", ".hpp", ".inc.h")

BARE_ASSERT = re.compile(r"(?<![_A-Za-z0-9])assert\s*\(")
NDEBUG_COND = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*\bNDEBUG\b", re.M)
SELF_TEST = re.compile(r"\b\w+_self_test\s*\(")


def strip_comments_and_strings(text: str) -> str:
    """Comments are prose and string literals are data; neither is a call.

    A gate that matches raw text gets satisfied by its own docstring -- this
    repository has already had one permanently disarmed that way, by the act
    of committing it.  Newlines are preserved so line numbers survive.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                if text[i] == q:
                    i += 1
                    break
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def first_party_sources() -> list:
    """Every tracked first-party engine source.

    THE RETURN CODE IS CHECKED, and that is not defensive boilerplate: the
    first run of this checker inside run_all.py reported "no first-party
    engine sources" while a standalone run of the same command listed 851.
    git had failed to start (this machine exhausts its Windows commit charge
    under a parallel lint, and a failed fork is one of the shapes that takes).
    Reading .stdout alone turns "the tool did not run" into "the tree is
    empty", and an empty scan is silently green -- which is the exact defect
    class this file exists to catch, aimed at itself.
    """
    try:
        r = subprocess.run(["git", "ls-files"] + [f"{x}/**" for x in SCAN_ROOTS],
                           cwd=REPO_ROOT, capture_output=True, text=True)
    except OSError as e:
        # git absent, or the fork itself failed.  Same conclusion as a non-zero
        # exit and it has to reach the same place, because the default for an
        # uncaught OSError here would be a traceback that run_all.py prints as
        # a failure with no sentence saying what it means.
        raise RuntimeError("git ls-files could not be started (%s) -- the scan "
                           "could not be built, which is not the same as a "
                           "clean tree" % e)
    if r.returncode != 0:
        raise RuntimeError(
            "git ls-files exited %d -- the scan could not be built, which is "
            "not the same as a clean tree. stderr: %s"
            % (r.returncode, (r.stderr or "").strip()[:200]))
    files = []
    for rel in r.stdout.splitlines():
        if "third_party" in rel:
            continue
        if not rel.endswith(EXTS):
            continue
        files.append(rel)
    return sorted(files)


def load_baseline() -> dict:
    if not BASELINE.exists():
        return {"bare_assert": {}, "ndebug_in_self_test_file": {}}
    return json.loads(BASELINE.read_text(encoding="utf-8"))


def scan(files, read) -> tuple:
    """(bare-assert hits, ndebug-in-self-test-file hits) as {rel: [lines]}."""
    bare, tangled = {}, {}
    for rel in files:
        raw = read(rel)
        if raw is None:
            continue
        code = strip_comments_and_strings(raw)

        lines = [code[:m.start()].count("\n") + 1 for m in BARE_ASSERT.finditer(code)]
        if lines:
            bare[rel] = lines

        if SELF_TEST.search(code):
            nd = [code[:m.start()].count("\n") + 1 for m in NDEBUG_COND.finditer(code)]
            if nd:
                tangled[rel] = nd
    return bare, tangled


def report(bare, tangled, base) -> list:
    failures = []
    for rel, lines in sorted(bare.items()):
        if rel in base.get("bare_assert", {}):
            continue
        failures.append(
            f"FAIL bare assert( at {rel}:{','.join(map(str, lines))} -- NDEBUG "
            f"compiles it out of the shipped build.  Use JCE_ASSERT / "
            f"JCE_ENSURE from <jce/os/core/jce_assert.h>, or baseline it in "
            f"{BASELINE.name} with a reason.")
    for rel, lines in sorted(tangled.items()):
        if rel in base.get("ndebug_in_self_test_file", {}):
            continue
        failures.append(
            f"FAIL {rel}:{','.join(map(str, lines))} gates a *_self_test on "
            f"NDEBUG.  That ties whether the invariant EXISTS to the "
            f"optimisation level: in Release the symbol is gone and the test "
            f"that runs it cannot even be registered.  Gate it on "
            f"JCE_SELF_TESTS instead (the root CMakeLists defines it whenever "
            f"JCE_BUILD_TESTS is ON).")
    return failures


# ---------------------------------------------------------------------------
#  Negative control.  A gate verified only against a green tree is not
#  verified: this repository has shipped three that could not fail.
# ---------------------------------------------------------------------------
FIXTURES = {
    "clean.c": (
        '#include <jce/os/core/jce_assert.h>\n'
        '/* a comment that says assert( and must NOT trip rule A */\n'
        'static const char *s = "assert(";\n'
        'void thing(void) { JCE_ASSERT(1); }\n', 0),
    "bare.c": (
        '#include <assert.h>\n'
        'void thing(void) { assert(1); }\n', 1),
    "tangled.c": (
        '#ifndef NDEBUG\n'
        'void mod_self_test(void) { }\n'
        '#endif\n', 1),
    "self_test_without_ndebug.c": (
        '#ifdef JCE_SELF_TESTS\n'
        'void mod_self_test(void) { }\n'
        '#endif\n', 0),
}


def self_check() -> int:
    base = {"bare_assert": {}, "ndebug_in_self_test_file": {}}
    bad = 0
    for name, (body, want) in FIXTURES.items():
        bare, tangled = scan([name], lambda _rel, b=body: b)
        got = len(report(bare, tangled, base))
        mark = "ok " if got == want else "BAD"
        if got != want:
            bad += 1
        print(f"  [{mark}] {name}: {got} finding(s), expected {want}")
    if bad:
        print(f"check_engine_assertions --self-check: {bad} fixture(s) wrong -- "
              f"the rules do not do what they claim")
        return 1
    print("check_engine_assertions --self-check: OK — both rules fire on a "
          "planted defect and stay quiet on prose, a string literal and the "
          "correct JCE_SELF_TESTS form")
    return 0


def main() -> int:
    if "--self-check" in sys.argv:
        return self_check()

    try:
        files = first_party_sources()
    except RuntimeError as e:
        print(f"FAIL check_engine_assertions: {e}")
        return 1
    if not files:
        print("FAIL check_engine_assertions: git ls-files SUCCEEDED and listed "
              "no first-party engine sources -- the scan found nothing to "
              "check, which is not the same as finding nothing wrong")
        return 1

    def read(rel):
        try:
            return (REPO_ROOT / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            return None

    base = load_baseline()
    bare, tangled = scan(files, read)
    failures = report(bare, tangled, base)
    for f in failures:
        print(f)
    if failures:
        return 1

    n_base = len(base.get("bare_assert", {})) + \
             len(base.get("ndebug_in_self_test_file", {}))
    print(f"check_engine_assertions: OK — {len(files)} first-party engine file(s) "
          f"scanned, no bare assert( outside the baseline and no *_self_test "
          f"gated on NDEBUG; {n_base} baselined entr(y/ies), each with a reason")
    return 0


if __name__ == "__main__":
    sys.exit(main())
