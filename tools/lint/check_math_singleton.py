#!/usr/bin/env python3
"""
check_math_singleton.py — AGENTS.md §11's fourth invariant, as a gate.

"不引入 cglm 或其它数学库（已统一 `jce_math`）".  One math library, so that a
vec3 means the same thing in every translation unit and there is one place to
fix a precision bug.

WHY THIS EXISTS EVEN THOUGH THE TREE IS CLEAN.  It is: there is no cglm here
-- no header, no conan package, no cmake reference.  The invariant holds on
its own today, which is exactly the moment to gate it, because the way a
second math library arrives is never a decision.  It arrives as a transitive
conan dependency, or as one `#include <glm/glm.hpp>` in a file somebody
adapted from a tutorial, and by the time it is load-bearing the choice has
already been made for everyone.

It nearly arrived as documentation.  Until 2026-08-31 jce_imgui.hpp described
"cglm interop" in two comments -- ImVec2/ImVec4 implicitly constructing from
cglm vec2/vec4 -- for a library that has never been in this tree.  Naming a
forbidden library as if it were on hand reads as permission.

WHAT IT DOES NOT CATCH.  A first-party file that reimplements a matrix inverse
instead of calling jce_math.  That is a code-review question; this gate
answers the mechanical half -- did a SECOND library get in.

USAGE
    python tools/lint/check_math_singleton.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# First-party trees only.  Vendored upstream code brings its own maths and is
# not this invariant's business -- fdk-aac and openh264 are not going to be
# rewritten against jce_math.
#
# User projects are out of scope for the same reason every other general gate
# excludes them (owner decision, 2026-08-27): a dogfooding consumer must not be
# able to turn the engine's gate red.
SCAN_TREES = [
    REPO_ROOT / "engine" / "src",
    REPO_ROOT / "engine" / "include",
    REPO_ROOT / "engine" / "tools_include",
    REPO_ROOT / "editor" / "src",
    REPO_ROOT / "tools",
    REPO_ROOT / "scripting",
]

VENDOR_MARKERS = ("/third_party/",)
SUFFIXES = (".c", ".h", ".cpp", ".hpp", ".cc", ".hh", ".inl")

# Include forms, not bare names: `cglm` in prose is a documentation problem,
# `#include <cglm/...>` is a dependency.  Matching prose would also make the
# comment in jce_imgui.hpp that explains cglm's ABSENCE trip the gate that
# exists because of it.
BANNED_INCLUDES: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r'#\s*include\s*[<"]cglm/'),        "cglm"),
    (re.compile(r'#\s*include\s*[<"]glm/'),         "glm"),
    (re.compile(r'#\s*include\s*[<"]Eigen/'),       "Eigen"),
    (re.compile(r'#\s*include\s*[<"]mathfu/'),      "mathfu"),
    (re.compile(r'#\s*include\s*[<"]linmath\.h[>"]'), "linmath"),
    (re.compile(r'#\s*include\s*[<"]DirectXMath\.h[>"]'), "DirectXMath"),
    (re.compile(r'#\s*include\s*[<"]glm\.hpp[>"]'), "glm"),
]

# The other door.  A math library that arrives through conanfile.py never needs
# an #include in first-party code to become a dependency of the build.
CONANFILE = REPO_ROOT / "conanfile.py"
BANNED_PACKAGES = ("cglm", "glm/", "eigen", "mathfu", "linmath", "directxmath")


def scan_sources() -> list[str]:
    hits: list[str] = []
    for tree in SCAN_TREES:
        if not tree.is_dir():
            continue
        for p in tree.rglob("*"):
            if p.suffix not in SUFFIXES or not p.is_file():
                continue
            rel = p.relative_to(REPO_ROOT).as_posix()
            if any(m in "/" + rel for m in VENDOR_MARKERS):
                continue
            try:
                text = p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for lineno, line in enumerate(text.splitlines(), 1):
                for pat, lib in BANNED_INCLUDES:
                    if pat.search(line):
                        hits.append("%s:%d: includes %s — jce_math is the one "
                                    "math library (§11)" % (rel, lineno, lib))
    return hits


def scan_conanfile() -> list[str]:
    if not CONANFILE.is_file():
        return []
    hits = []
    text = CONANFILE.read_text(encoding="utf-8", errors="replace")
    for lineno, line in enumerate(text.splitlines(), 1):
        low = line.lower()
        if "requires" not in low and "self.requires" not in low:
            continue
        # First match only: "cglm/0.9.4" contains both "cglm" and "glm/", and
        # reporting one line twice makes a reader hunt for a second problem.
        for pkg in BANNED_PACKAGES:
            if pkg in low:
                hits.append("conanfile.py:%d: requires %s — jce_math is the "
                            "one math library (§11)" % (lineno, pkg))
                break
    return hits


def main() -> int:
    hits = scan_sources() + scan_conanfile()
    if not hits:
        print("math-singleton check: OK — jce_math is the only math library; "
              "no cglm / glm / Eigen / mathfu / linmath / DirectXMath include "
              "or conan requirement in first-party code.")
        return 0
    print("math-singleton check: FAILED")
    print()
    for h in hits:
        print("  %s" % h)
    print()
    print("  §11: 已统一 jce_math.  If jce_math is missing something you need,")
    print("  add it there — a second vec3 type is how two of them end up")
    print("  meaning different things in different translation units.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
