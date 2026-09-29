#!/usr/bin/env python3
"""
check_physics_debug_shipped.py — a shipped game must be able to draw a collider.

WHY THIS EXISTS.  On 2026-09-20 the whole capability was public on both sides
and reachable from neither.  jce_physics_debug.h had carried the flag set, the
line-sink installer and the flush since P3-C.5, and jce_debug_draw.h had
carried the sink itself -- but in the entire repository:

    jce_physics_debug_set_line_sink   1 caller, editor/src/panels/...
    jce_physics_debug_flush           1 caller, editor/src/scene/...

So a packaged game had no collider overlay at all.  A developer chasing a
physics bug that only reproduces in the shipped build had to write the
trampoline themselves -- in the one configuration where writing code is the
thing you cannot do.

It is the same shape as check_world_streaming_shipped.py guards: alive in the
editor, dead in the shipped exe, and invisible for the same reason -- the only
configuration in which it is broken is the one nobody opens while authoring.

WHAT THIS CHECKS.

  RULE 1  jce_physics_debug_set_line_sink has a caller outside editor/, i.e.
          on a path that exists without an editor.

  RULE 2  jce_physics_debug_flush likewise.  Installing a sink and never
          flushing is a sink that receives nothing: Bullet's debug buffer is
          walked by the flush, so without it the flags are authored, the sink
          is installed, and not one line is ever produced.

  RULE 3  That non-editor caller also reads jce_physics_debug_get_flags, so
          the flush is skipped while the overlay is off.  A flush on every
          frame of every shipped game, for a feature almost nobody enables,
          is a cost this gate should not be used to justify.

WHAT THIS DOES NOT CHECK, said out loud rather than implied: that the lines
reach a pixel.  That is jce_debug_draw_flush's half and needs a renderer and a
frame.  A text scan sees call sites.  The CPU half -- that the sink receives
geometry proportional to the bodies -- is covered by
tests/middleware/physics/test_jce_physics_debug_sink.c, which IS in the tree
-- this line said it was not, for the few hours between being written and the
discovery that tests/ is gitignored on `main` and tracked here.

Usage:  python tools/lint/check_physics_debug_shipped.py
        python tools/lint/check_physics_debug_shipped.py --self-check
Exit 0 clean, 1 on any finding.
"""
import re
import subprocess
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]

# The declarations live in this header; a "caller" must not be it.
DECL_FILE = "engine/include/jce/middleware/physics/jce_physics_debug.h"

SYMBOLS = {
    "jce_physics_debug_set_line_sink":
        "install a line sink, so the physics layer has somewhere to push to",
    "jce_physics_debug_flush":
        "walk Bullet's debug buffer, which is what produces the lines at all",
}
GUARD = "jce_physics_debug_get_flags"


def strip_comments_and_strings(text: str) -> str:
    """A mention in prose is not a call.

    This repository has had a gate permanently disarmed by the act of
    committing its own docstring, and another that counted a comment as a
    caller (it reported 6 of 7 where the truth was 5).  Newlines survive so
    line numbers stay meaningful.
    """
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in '"\'':
            q, i = c, i + 1
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


def tracked_sources() -> list:
    """Tracked first-party C/C++ outside third_party.

    THE RETURN CODE IS CHECKED.  A sibling gate written the same day reported
    "no sources" inside run_all.py while a standalone run listed 851: git had
    failed to start.  Reading .stdout alone turns "the tool did not run" into
    "the tree is empty", and an empty scan is silently green -- which is the
    very defect class these gates exist to catch, aimed at themselves.
    """
    r = subprocess.run(["git", "ls-files",
                        "engine/**", "editor/**", "scripting/**", "tools/**"],
                       cwd=REPO_ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("git ls-files exited %d -- the scan could not be "
                           "built, which is not the same as a clean tree. "
                           "stderr: %s" % (r.returncode,
                                           (r.stderr or "").strip()[:200]))
    return [p for p in r.stdout.splitlines()
            if p.endswith((".c", ".h", ".cpp", ".hpp", ".inc.h"))
            and "third_party" not in p]


def _is_decl_or_def(code: str, at: int, sym: str) -> bool:
    """Is the match at `at` the function's own DEFINITION or a re-DECLARATION,
    rather than a call?

    THIS IS NOT A REFINEMENT, IT IS THE GATE.  The first version of this file
    reported engine/src/middleware/physics/jce_physics.c as a non-editor
    "caller" of jce_physics_debug_flush -- it had matched the line that DEFINES
    it.  That one line would have satisfied RULE 2 forever, so the gate would
    have been green on the exact tree it was written to reject.  This
    repository has shipped three gates that could not fail; this is how.

    The discriminator is what follows the argument list, NOT indentation.  An
    indentation rule was tried first and its own fixtures killed it: written on
    one line, a perfectly good call reads as column-0 code.  After the closing
    parenthesis:

        `{`   a definition          void jce_physics_debug_flush(W *w) { ... }
        `;`   a call OR a re-declaration -- told apart by what precedes the
              name on its line: a declaration carries a return type before it
              at column 0, a call does not.
        else  a call in an expression: if (flush(w)) / f(flush(w), 1)
    """
    # Walk to the parenthesis that closes this argument list.
    i = code.find("(", at)
    if i < 0:
        return False
    depth, n = 0, len(code)
    while i < n:
        if code[i] == "(":
            depth += 1
        elif code[i] == ")":
            depth -= 1
            if depth == 0:
                break
        i += 1
    j = i + 1
    while j < n and code[j].isspace():
        j += 1
    if j >= n:
        return False
    if code[j] == "{":
        return True                      # a definition
    if code[j] != ";":
        return False                     # an expression -- a call

    # Followed by ';'.  A re-declaration has a return type ahead of the name
    # at column 0; a statement call is either indented or starts with the name.
    line_start = code.rfind("\n", 0, at) + 1
    before = code[line_start:at]
    return before.strip() != "" and not before[0].isspace()


def callers(files, read) -> dict:
    """{symbol: [files that CALL it, excluding editor/, the header and its
    own definition]}."""
    pats = {s: re.compile(r"\b" + re.escape(s) + r"\s*\(") for s in SYMBOLS}
    pats[GUARD] = re.compile(r"\b" + re.escape(GUARD) + r"\s*\(")
    hits = {s: [] for s in pats}
    for rel in files:
        if rel == DECL_FILE:
            continue
        raw = read(rel)
        if raw is None:
            continue
        code = strip_comments_and_strings(raw)
        for sym, pat in pats.items():
            if any(not _is_decl_or_def(code, m.start(), sym)
                   for m in pat.finditer(code)):
                hits[sym].append(rel)
    return hits


def non_editor(paths) -> list:
    return [p for p in paths if not p.startswith("editor/")]


def report(hits) -> list:
    failures = []
    for sym, why in SYMBOLS.items():
        outside = non_editor(hits.get(sym, []))
        if not outside:
            where = ", ".join(hits.get(sym, [])) or "nowhere at all"
            failures.append(
                f"FAIL {sym} has no caller outside editor/ (found in: {where}). "
                f"A shipped game cannot {why}, so it draws no colliders -- and "
                f"the developer who needs them is in the one configuration "
                f"where writing the wiring is not an option.")
    flushers = non_editor(hits.get("jce_physics_debug_flush", []))
    guards = non_editor(hits.get(GUARD, []))
    if flushers and not guards:
        failures.append(
            f"FAIL a non-editor file flushes physics debug ({', '.join(flushers)}) "
            f"but nothing outside editor/ reads {GUARD}. Every shipped game "
            f"would then walk Bullet's debug buffer every frame for an overlay "
            f"almost nobody enables.")
    return failures


# ---------------------------------------------------------------------------
#  Negative control.  A gate verified only against a green tree is not
#  verified: this repository has shipped three that could not fail.
# ---------------------------------------------------------------------------
FIXTURES = {
    "editor-only": ({
        "editor/src/panels/p.cpp":
            "void f(){\n    jce_physics_debug_set_line_sink(s,0);\n"
            "    jce_physics_debug_flush(w);\n    jce_physics_debug_get_flags();\n}",
    }, 2),
    "wired": ({
        "editor/src/panels/p.cpp":
            "void f(){\n    jce_physics_debug_set_line_sink(s,0);\n}",
        "engine/include/jce/application/m.inc.h":
            "void g(){\n    jce_physics_debug_set_line_sink(s,0);\n"
            "    if (jce_physics_debug_get_flags())\n"
            "        jce_physics_debug_flush(w);\n}",
    }, 0),
    "flush-without-guard": ({
        "engine/include/jce/application/m.inc.h":
            "void g(){\n    jce_physics_debug_set_line_sink(s,0);\n"
            "    jce_physics_debug_flush(w);\n}",
    }, 1),
    # THE FIXTURE FOR THIS FILE'S OWN FIRST DEFECT: the implementation is not
    # a caller, so an editor-only wiring must still be reported even though
    # engine/src carries the definition.
    "definition-is-not-a-caller": ({
        "editor/src/panels/p.cpp":
            "void f(){\n    jce_physics_debug_set_line_sink(s,0);\n"
            "    jce_physics_debug_flush(w);\n    jce_physics_debug_get_flags();\n}",
        "engine/src/middleware/physics/jce_physics.c":
            "void jce_physics_debug_flush(JcePhysicsWorld *w)\n{\n    (void)w;\n}\n"
            "void jce_physics_debug_set_line_sink(jce_debug_line_fn f, void *u)\n"
            "{\n    (void)f; (void)u;\n}\n",
    }, 2),
    # A forward declaration is not a call either.  Without this, one
    # `void jce_physics_debug_flush(JcePhysicsWorld *);` line pasted into any
    # engine .c file would satisfy RULE 2 for ever.
    "forward-declaration-is-not-a-caller": ({
        "editor/src/panels/p.cpp":
            "void f(){\n    jce_physics_debug_set_line_sink(s,0);\n"
            "    jce_physics_debug_flush(w);\n    jce_physics_debug_get_flags();\n}",
        "engine/src/x.c":
            "void jce_physics_debug_flush(JcePhysicsWorld *w);\n"
            "void jce_physics_debug_set_line_sink(jce_debug_line_fn f, void *u);\n",
    }, 2),
    "mention-in-prose-is-not-a-caller": ({
        "editor/src/panels/p.cpp":
            "void f(){\n    jce_physics_debug_set_line_sink(s,0);\n"
            "    jce_physics_debug_flush(w);\n    jce_physics_debug_get_flags();\n}",
        "engine/src/x.c":
            "/* someday: jce_physics_debug_flush(w) and"
            " jce_physics_debug_set_line_sink(s,0) */\n",
    }, 2),
}


def self_check() -> int:
    bad = 0
    for name, (files, want) in FIXTURES.items():
        hits = callers(list(files), lambda rel: files[rel])
        got = len(report(hits))
        mark = "ok " if got == want else "BAD"
        if got != want:
            bad += 1
        print(f"  [{mark}] {name}: {got} finding(s), expected {want}")
    if bad:
        print(f"check_physics_debug_shipped --self-check: {bad} fixture(s) wrong")
        return 1
    print("check_physics_debug_shipped --self-check: OK — the rules fire on an "
          "editor-only wiring and on a guardless flush, stay quiet on a correct "
          "one, and do not count a mention in a comment as a caller")
    return 0


def main() -> int:
    if "--self-check" in sys.argv:
        return self_check()

    try:
        files = tracked_sources()
    except RuntimeError as e:
        print(f"FAIL check_physics_debug_shipped: {e}")
        return 1
    if not files:
        print("FAIL check_physics_debug_shipped: git ls-files SUCCEEDED and "
              "listed no sources -- the scan found nothing to check, which is "
              "not the same as finding nothing wrong")
        return 1

    def read(rel):
        try:
            return (REPO_ROOT / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            return None

    hits = callers(files, read)
    failures = report(hits)
    for f in failures:
        print(f)
    if failures:
        return 1

    where = ", ".join(non_editor(hits["jce_physics_debug_flush"]))
    print(f"check_physics_debug_shipped: OK — the sink is installed and the "
          f"buffer is flushed outside editor/ ({where}), behind a "
          f"{GUARD} check; {len(files)} tracked source(s) scanned")
    return 0


if __name__ == "__main__":
    sys.exit(main())
