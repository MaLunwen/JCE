#!/usr/bin/env python3
"""
check_jce_tests_closure.py — the `jce_tests` aggregate target must build
EVERY test carrying the "unit" ctest label, not a subset that happens to
look complete.

`jce_tests` exists so CI can build the exact dependency closure of the test
suite (`cmake --build <dir> --target jce_tests`) instead of the editor and
every vendored codec, then run `ctest -L unit`.  `jce_add_unit_test` and
`jce_add_cxx_test` call `add_dependencies(jce_tests ${NAME})` themselves, so
every test registered through either helper is covered by construction.

`tests/middleware/ai_dispatch/CMakeLists.txt` registers `aid_demo` with a
raw `add_executable` + `add_test(NAME aid_demo_selftest COMMAND aid_demo)` —
it bypasses both helpers (it is a multi-mode CLI demo, not a Unity test, and
its ctest name deliberately differs from its target name), so it never got
the helper's automatic `add_dependencies`.  `jce_tests` silently built every
test except that one.  `ctest -L unit -N` still lists `aid_demo_selftest`
because ctest only knows about `add_test`, not `jce_tests`'s dependency
list — so on a from-scratch build, `ctest -L unit` reports "Unable to find
executable" for it, indistinguishable from a real test failure, while
`jce_tests` itself reports success.

This check has two parts:

  1. Both helper functions must still call
     `add_dependencies(jce_tests ${NAME})` — the mechanism every normal test
     relies on.  If a future edit drops that line, every test registered
     through that helper silently falls out of `jce_tests` at once.

  2. Every *raw* `add_test(NAME <literal> COMMAND <target>)` anywhere under
     tests/ (i.e. one that does not go through either helper) whose test
     carries the "unit" label must have a matching
     `add_dependencies(jce_tests <target>)` somewhere in the tree.  This is
     deliberately NOT a check for "aid_demo" by name: the next raw
     `add_test` with a "unit" label reopens the identical hole, and this
     rule catches it the same way regardless of what it is called.

     WITH ONE EXCLUSION, and it is about what the rule can mean rather than
     about convenience.  The rule reads a COMMAND token as a BUILD TARGET —
     that is the whole premise, since the failure it prevents is `jce_tests`
     not building something ctest will later try to run.  A COMMAND that is
     a CMake variable reference is not a target name: an interpreter-driven
     suite registers `COMMAND "${Python3_EXECUTABLE}"`, which `find_package`
     resolved to a system interpreter that no build produces and no
     `add_dependencies` can name.  Requiring
     `add_dependencies(jce_tests "${Python3_EXECUTABLE}")` there would be
     asking for a line that is false — and satisfying a gate with a false
     statement is worse than the gap it closes.

     Such suites keep the real closure guarantee in the form that actually
     means something: they name their genuine build inputs.  Both
     tests/scripting/python and tests/scripting/java do, for the runner, the
     mock host and the shared library their driver loads.  Those lines are
     covered by this rule wherever they appear, because they name targets.

     The exclusion is deliberately narrow — a variable reference and nothing
     else.  A raw literal target still has to be declared, so `aid_demo`,
     the registration this check was written for, is unaffected.

Usage:
  python tools/lint/check_jce_tests_closure.py
  # exits 0 if clean, 1 with details on violation.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TESTS_DIR = REPO_ROOT / "tests"
TESTS_ROOT_CMAKE = TESTS_DIR / "CMakeLists.txt"

HELPER_NAMES = ("jce_add_unit_test", "jce_add_cxx_test")

ADD_TEST_RE = re.compile(
    r"add_test\s*\(\s*NAME\s+(\S+)\s+COMMAND\s+([^\s)]+)", re.MULTILINE
)
SET_PROPS_LABELS_RE_TMPL = (
    r"set_tests_properties\s*\(\s*{name}\s+PROPERTIES"
    r"(?:(?!add_test|set_tests_properties).)*?"
    r"LABELS\s+\"([^\"]*)\""
)
ADD_DEPS_RE_TMPL = r"add_dependencies\s*\(\s*jce_tests\s+{target}\s*\)"

# A COMMAND token that is a bare CMake variable reference, with or without the
# surrounding quotes: "${Python3_EXECUTABLE}" or ${SOMETHING}.  Anchored at both
# ends so `${PFX}_target` or `foo${X}` — which do name a target once expanded —
# are NOT excluded and still have to be wired.
VARIABLE_REF_RE = re.compile(r'^"?\$\{[A-Za-z0-9_]+\}"?$')


def _function_body(text: str, fn_name: str) -> str | None:
    """Return the body text of `function(<fn_name> ...) ... endfunction()`."""
    m = re.search(r"function\s*\(\s*" + re.escape(fn_name) + r"\b", text)
    if not m:
        return None
    end = re.search(r"\nendfunction\s*\(", text[m.end():])
    if not end:
        return None
    return text[m.end(): m.end() + end.start()]


def check_helpers_still_wired(root_text: str) -> list[str]:
    problems = []
    for fn in HELPER_NAMES:
        body = _function_body(root_text, fn)
        if body is None:
            problems.append(f"tests/CMakeLists.txt: function {fn}() not found")
            continue
        if "add_dependencies(jce_tests ${NAME})" not in body:
            problems.append(
                f"tests/CMakeLists.txt: {fn}() no longer calls "
                f"add_dependencies(jce_tests ${{NAME}}) — every test registered "
                f"through this helper just silently fell out of jce_tests"
            )
    return problems


def check_raw_registrations_wired(all_text_by_file: dict[Path, str]) -> list[str]:
    problems = []
    combined = "\n".join(all_text_by_file.values())

    for path, text in all_text_by_file.items():
        for m in ADD_TEST_RE.finditer(text):
            test_name, target = m.group(1), m.group(2)
            if test_name == "${NAME}":
                # Inside jce_add_unit_test / jce_add_cxx_test themselves —
                # covered by check_helpers_still_wired above.
                continue

            if VARIABLE_REF_RE.match(target):
                # Not a build target, so "does jce_tests build it?" has no
                # answer to demand.  See part 2 of the module docstring: the
                # only honest closure such a suite can offer is a dependency
                # on its real build inputs, and those are named as literal
                # targets elsewhere in the same file, where this rule sees
                # them.  Narrow on purpose: a literal target name still has
                # to be declared.
                continue

            labels_re = re.compile(
                SET_PROPS_LABELS_RE_TMPL.format(name=re.escape(test_name)),
                re.DOTALL,
            )
            labels_m = labels_re.search(text)
            if not labels_m:
                # No LABELS found for this raw test at all -- can't be "unit".
                continue
            labels = [s.strip() for s in labels_m.group(1).split(";")]
            if "unit" not in labels:
                continue

            deps_re = re.compile(ADD_DEPS_RE_TMPL.format(target=re.escape(target)))
            if not deps_re.search(combined):
                rel = path.relative_to(REPO_ROOT).as_posix()
                problems.append(
                    f"{rel}: add_test(NAME {test_name} COMMAND {target}) carries "
                    f"LABELS \"unit\" but no add_dependencies(jce_tests {target}) "
                    f"exists anywhere under tests/ — jce_tests will build "
                    f"successfully while silently skipping {target}, and ctest "
                    f"will report {test_name} as \"Unable to find executable\" "
                    f"on a from-scratch build"
                )
    return problems


def main() -> int:
    if not TESTS_ROOT_CMAKE.is_file():
        print(f"check_jce_tests_closure: {TESTS_ROOT_CMAKE} not found", file=sys.stderr)
        return 1

    root_text = TESTS_ROOT_CMAKE.read_text(encoding="utf-8", errors="replace")
    problems = check_helpers_still_wired(root_text)

    all_text_by_file: dict[Path, str] = {}
    for cmake_file in TESTS_DIR.rglob("CMakeLists.txt"):
        try:
            all_text_by_file[cmake_file] = cmake_file.read_text(
                encoding="utf-8", errors="replace"
            )
        except OSError as e:
            print(f"warn: cannot read {cmake_file}: {e}", file=sys.stderr)

    problems += check_raw_registrations_wired(all_text_by_file)

    if not problems:
        print("jce_tests closure check: OK (every unit-labeled test is wired "
              "into jce_tests)")
        return 0

    print("jce_tests closure check: FAILED")
    print()
    for p in problems:
        print(f"  {p}")
    print()
    print(f"{len(problems)} violation(s).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
