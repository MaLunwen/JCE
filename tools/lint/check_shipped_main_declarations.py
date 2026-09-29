#!/usr/bin/env python3
"""
check_shipped_main_declarations.py — the shipped game's main must compile.

WHY THIS EXISTS.  engine/include/jce/application/jce_default_main.inc.h IS the
shipped game: a project's main.c is a ten-line shim that includes it, so every
deployed exe built by this editor compiles this file.  NOTHING IN THIS
REPOSITORY COMPILES IT.  Not a CMake target, not a test, not a sample -- it is
a header that only a user project instantiates, so a declaration error in it
travels all the way to a customer's build and no gate here says a word.

MEASURED WHEN THIS GATE LANDED (2026-09-05): two.  A scene-transition fade quad
called jce_draw_filled_rect() and jce_rgba() without including
<jce/renderer/jce_primitives.h>.  MSVC at its default warning level compiles an
implicit declaration silently (C4013 needs /W3 and did not fire at /W1), so the
defect was invisible on the machine that wrote it -- but C99 removed implicit
declarations, and clang 15+ and GCC 14 make them a HARD ERROR.  Any user
project on a modern clang would have failed to build the engine's own default
main, with the error pointing inside an SDK header they never wrote.

Found by hand-compiling the SDK copy with /we4013 while checking an unrelated
change.  A defect that needs someone to think of writing a throwaway .bat is a
defect that ships; this is that .bat, without the compiler.

WHAT THIS CHECKS.  Every `jce_*(` CALL in the shipped main resolves to a
declaration reachable through the file's own transitive include closure under
engine/include/.  That is the property a compiler enforces and the property
that was violated.

WHAT THIS DOES NOT CHECK.  Types, arity, or anything else a compiler does --
this is not a compiler.  It catches "you called something no included header
declares", which is the failure that actually happened and the one that is
silent on this toolchain.

Usage:  python tools/lint/check_shipped_main_declarations.py
Exit 0 clean, 1 on an unresolved call.
"""

import re
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]
INCLUDE_ROOT = REPO_ROOT / "engine" / "include"
SHIPPED = INCLUDE_ROOT / "jce" / "application" / "jce_default_main.inc.h"

# `#include <jce/...>` — the angle form is the only one this file uses, and the
# only one that resolves against the SDK include root a user project gets.
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*<\s*(jce/[^>]+)\s*>', re.M)

# A CALL: an identifier followed by `(`.  Deliberately not a general C parser --
# a declaration is `... name(` too, so declarations are excluded by only ever
# scanning the shipped main, which declares nothing named jce_*.
CALL_RE = re.compile(r'\b(jce_[A-Za-z0-9_]+)\s*\(')

# Anything that DECLARES the name in a header: a prototype, a macro, a function
# pointer typedef member, or an enum/struct tag.  Broad on purpose -- this gate
# answers "is this name known here", and a false PASS from over-breadth is
# better than a false FAIL that teaches people to add exemptions.
def declares(text: str, name: str) -> bool:
    return re.search(r'\b' + re.escape(name) + r'\b', text) is not None


def strip_comments_and_strings(src: str) -> str:
    """Calls inside a comment or a string literal are not calls.

    The shipped main is full of prose naming functions ("jce_audio_load(...)
    does PAK decompress + decode"), and counting those would make this gate
    report on documentation."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
            out.append(' ')
        elif c == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i)
            i = n if j < 0 else j
            out.append(' ')
        elif c in '"\'':
            q, j = c, i + 1
            while j < n and src[j] != q:
                j += 2 if src[j] == '\\' else 1
            i = j + 1
            out.append(' ')
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def closure(entry: Path) -> tuple[list[Path], list[str]]:
    """Every engine/include header reachable from `entry`, and what is missing.

    A header that does not resolve is REPORTED, not skipped: a closure that
    silently shrinks is how "everything is declared" gets said about a search
    that looked at less."""
    seen, order, missing = set(), [], []
    stack = [entry]
    while stack:
        p = stack.pop()
        rp = p.resolve()
        if rp in seen or not p.is_file():
            continue
        seen.add(rp)
        order.append(p)
        text = p.read_text(encoding="utf-8", errors="replace")
        for rel in INCLUDE_RE.findall(text):
            nxt = INCLUDE_ROOT / rel
            if nxt.is_file():
                stack.append(nxt)
            elif rel not in missing:
                missing.append(rel)
    return order, missing


def main() -> int:
    if not SHIPPED.is_file():
        print("check_shipped_main_declarations: FAIL - %s not found"
              % SHIPPED.relative_to(REPO_ROOT).as_posix(), file=sys.stderr)
        return 1

    src = strip_comments_and_strings(
        SHIPPED.read_text(encoding="utf-8", errors="replace"))
    called = sorted(set(CALL_RE.findall(src)))
    if not called:
        print("check_shipped_main_declarations: FAIL - parsed zero jce_* calls "
              "from the shipped main; the scan is blind", file=sys.stderr)
        return 1

    headers, missing = closure(SHIPPED)
    # The file itself is NOT in the haystack: if it were, every call would
    # resolve by matching its own call site and the gate could never fail.
    haystack = "\n".join(
        h.read_text(encoding="utf-8", errors="replace")
        for h in headers if h.resolve() != SHIPPED.resolve())

    # ...but it does DEFINE some of what it calls -- six jce_default_* statics.
    # A definition counts as a declaration; a call does not, and that
    # distinction is the whole gate.  So match `static <type> name(` only,
    # never a bare `name(` -- the defect this was written for was a bare call.
    own = set(re.findall(
        r'^\s*static\s+[^;()]*?\b(jce_[A-Za-z0-9_]+)\s*\(', src, re.M))

    problems = []
    for rel in missing:
        problems.append(
            "#include <%s> does not resolve under engine/include/ -- this gate "
            "cannot see what it declares, so its symbols would pass unchecked"
            % rel)

    for name in called:
        if name in own:
            continue
        if not declares(haystack, name):
            line = next((i for i, ln in enumerate(
                SHIPPED.read_text(encoding="utf-8",
                                  errors="replace").splitlines(), 1)
                if re.search(r'\b' + re.escape(name) + r'\s*\(', ln)), 0)
            problems.append(
                "%s:%d calls %s(), which NO header this file includes "
                "declares.  A user project's main.c instantiates this header, "
                "so this is an implicit declaration in every shipped game -- "
                "silent on MSVC at its default warning level, a hard error on "
                "clang 15+ and GCC 14.  Add the #include that declares it."
                % (SHIPPED.relative_to(REPO_ROOT).as_posix(), line, name))

    if problems:
        for p in problems:
            print("  " + p, file=sys.stderr)
        print("check_shipped_main_declarations: FAIL - %d problem(s)."
              % len(problems), file=sys.stderr)
        return 1

    print("check_shipped_main_declarations: OK (%d jce_* call(s) in the shipped "
          "main; %d defined in the file itself, the rest declared across %d "
          "header(s) in its include closure)"
          % (len(called), len(own & set(called)), len(headers)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
