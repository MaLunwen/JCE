#!/usr/bin/env python3
"""
check_build_script_layout.py — AGENTS.md §11's eighth invariant, as a gate.

"新增构建脚本只能落 `scripts/`".  Taken literally that is not decidable: a
dozen files under tools/ mention cmake because they READ CMakeLists, and
scripting/java/build_java.py and scripting/csharp/build_csharp.py compile a
managed half deliberately colocated with the language they belong to.  A gate
built on "does the word cmake appear" would flag eighteen files, none of them
the thing the rule is about.

§9 says what the rule is about, in its own words:

    桌面构建逻辑只在 `jce.py`；`scripts/<os>/` 是薄壳（同名同参,零重复）。
    非桌面（android/web/ios/universal）保持原样。

So the invariant is not "no scripts elsewhere".  It is "there is ONE desktop
build entry point, and the per-OS files are shells over it" -- and that IS
decidable:

  1. No build-entry script sits at the repository root.  conanfile.py is
     exempt because conan requires that exact name at that exact place.
  2. Every desktop shell under scripts/{windows,linux,macos}/ forwards to
     jce.py.
  3. No desktop shell invokes cmake / ninja / msbuild itself.  A shell that
     does is no longer a shell -- it is a second copy of the build logic, and
     the failure mode is the one §9 names: the two copies drift, and a bug
     fixed in jce.py stays alive on one platform.

§9 exempts the non-desktop targets by name, so files matching android / web /
ios / universal are held to rule 1 only.  scripts/macos/build-ios-arm64.sh
invokes cmake three times and is correct to.

Measured 2026-08-31: the tree satisfies all three.  This gate is here to keep
that true, not to fix anything.

USAGE
    python tools/lint/check_build_script_layout.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

SCRIPT_SUFFIXES = (".py", ".ps1", ".sh", ".bat", ".cmd")

# conan resolves the recipe by this exact name at the project root; it cannot
# live under scripts/.  That is a tool constraint, not a style exception.
ROOT_ALLOWED = {"conanfile.py"}

DESKTOP_SHELL_DIRS = ("windows", "linux", "macos")

# §9: "非桌面（android/web/ios/universal）保持原样".
NON_DESKTOP = re.compile(r"(android|web|ios|universal|wasm|emscripten)",
                         re.IGNORECASE)

# A build INVOCATION, not a mention: the word at the start of a command, after
# a shell operator, or as the program in a call.  `# see cmake docs` and
# `CMakeLists.txt` must not match.
INVOKES_BUILD = re.compile(
    r"(?:^|[|&;`$(]|\bexec\s+|\bcall\s+)\s*\"?(cmake|ninja|msbuild)\b",
    re.IGNORECASE | re.MULTILINE)

FORWARDS = re.compile(r"jce\.py")

EXIT_SKIPPED = 2


def root_build_scripts() -> list[str]:
    out = []
    for p in REPO_ROOT.iterdir():
        if p.is_file() and p.suffix in SCRIPT_SUFFIXES \
                and p.name not in ROOT_ALLOWED:
            out.append(p.name)
    return sorted(out)


def shell_problems() -> list[str]:
    problems: list[str] = []
    for d in DESKTOP_SHELL_DIRS:
        base = REPO_ROOT / "scripts" / d
        if not base.is_dir():
            continue
        for p in sorted(base.iterdir()):
            if not p.is_file() or p.suffix not in SCRIPT_SUFFIXES:
                continue
            rel = p.relative_to(REPO_ROOT).as_posix()
            if NON_DESKTOP.search(p.name):
                continue                      # §9 exempts these by name
            try:
                text = p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if not FORWARDS.search(text):
                problems.append("%s: a desktop shell that does not forward to "
                                "tools/build/jce.py — §9 puts the desktop build "
                                "logic there and nowhere else" % rel)
            m = INVOKES_BUILD.search(text)
            if m:
                line = text[:m.start()].count("\n") + 1
                problems.append("%s:%d: invokes %s directly — that makes it a "
                                "second copy of jce.py's build logic, which is "
                                "what §9's 零重复 forbids"
                                % (rel, line, m.group(1)))
    return problems


def main() -> int:
    if not (REPO_ROOT / "scripts").is_dir():
        print("build-script-layout check: SKIPPED — scripts/ is not present.")
        return EXIT_SKIPPED

    stray = root_build_scripts()
    problems = shell_problems()

    if not stray and not problems:
        print("build-script-layout check: OK — one desktop build entry point "
              "(tools/build/jce.py); every desktop shell forwards to it and none "
              "invokes a build tool itself; the repository root carries no "
              "build script but conanfile.py.")
        return 0

    print("build-script-layout check: FAILED")
    print()
    if stray:
        print("Build scripts at the repository root (%d):" % len(stray))
        for s in stray:
            print("  %s" % s)
        print()
        print("  §11: 新增构建脚本只能落 scripts/.  conanfile.py is the one")
        print("  exception, because conan resolves the recipe by that name at")
        print("  the project root.")
        print()
    if problems:
        print("Desktop shells that are not shells (%d):" % len(problems))
        for p in problems:
            print("  %s" % p)
        print()
    return 1


if __name__ == "__main__":
    sys.exit(main())
