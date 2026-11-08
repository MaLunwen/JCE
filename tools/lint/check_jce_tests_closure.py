#!/usr/bin/env python3
"""Verify test build-input routing through the shared domain metadata owner.

Helpers and raw executable/script providers use jce_test_build_dependencies.
Its call-site domain authority wires both jce_tests and jce_tests_<domain>;
CMake's deferred audit verifies actual dependencies equal those routed inputs.
This source gate checks helper wiring and literal raw unit executable providers.
Interpreter COMMANDs are not build targets: their actual runner/library inputs
are declared at the registering script call site. Actual CMake NONE controls
verify domain isolation, shared providers, missing edges and unchanged CTest
inventory; neither source parsing nor those controls certify application/UI use.
"""

from __future__ import annotations

import importlib.util
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TESTS_DIR = REPO_ROOT / "tests"
TESTS_ROOT_CMAKE = TESTS_DIR / "CMakeLists.txt"
METADATA_CMAKE = TESTS_DIR / "cmake/JceTestMetadata.cmake"

_spec = importlib.util.spec_from_file_location(
    "lint_git_files", Path(__file__).with_name("lint_git_files.py"))
_lgf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_lgf)


def source_cmake_files() -> list[Path]:
    """Actual publication candidates, never ignored generated/frozen copies."""
    return [REPO_ROOT / rel for rel in _lgf.working_tree()
            if rel.startswith("tests/") and Path(rel).name == "CMakeLists.txt"]


HELPER_NAMES = ("jce_add_unit_test", "jce_add_cxx_test")

ADD_TEST_RE = re.compile(
    r"add_test\s*\(\s*NAME\s+(\S+)\s+COMMAND\s+([^\s)]+)", re.MULTILINE
)
SET_PROPS_LABELS_RE_TMPL = (
    r"set_tests_properties\s*\(\s*{name}\s+PROPERTIES"
    r"(?:(?!add_test|set_tests_properties).)*?"
    r"LABELS\s+\"([^\"]*)\""
)
BUILD_INPUTS_RE = re.compile(r"\bjce_test_build_dependencies\s*\(([^)]*)\)", re.MULTILINE)

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
    root_text = re.sub(r"(?m)#.*$", "", root_text)
    problems = []
    for fn in HELPER_NAMES:
        body = _function_body(root_text, fn)
        if body is None:
            problems.append(f"tests/CMakeLists.txt: function {fn}() not found")
            continue
        if "jce_test_build_dependencies(${NAME})" not in body:
            problems.append(
                f"tests/CMakeLists.txt: {fn}() no longer calls "
                f"jce_test_build_dependencies(${{NAME}}) — every test registered "
                f"through this helper just silently fell out of jce_tests"
            )
    return problems


def check_build_dependency_authority(metadata_text: str) -> list[str]:
    """Actual CMake guards also fail when either required edge is removed."""
    text = re.sub(r"(?m)#.*$", "", metadata_text)
    body = _function_body(text, "jce_test_build_dependencies")
    if body is None:
        return ["JceTestMetadata: build-input routing function is missing"]
    required = (
        '_jce_test_owner_domain("${CMAKE_CURRENT_SOURCE_DIR}" _domain)',
        'set_property(TARGET jce_tests_${_domain} APPEND PROPERTY JCE_TEST_BUILD_INPUTS ${ARGN})',
        'add_dependencies(jce_tests ${ARGN})',
        'add_dependencies(jce_tests_${_domain} ${ARGN})',
    )
    problems = [f"JceTestMetadata: build-input routing lost {wire}"
                for wire in required if wire not in body]
    audit = _function_body(text, "jce_assert_test_metadata") or ""
    if "_jce_assert_test_build_closure()" not in audit:
        problems.append("JceTestMetadata: actual aggregate closure audit is missing")
    return problems


def check_raw_registrations_wired(all_text_by_file: dict[Path, str]) -> list[str]:
    problems = []
    # Helpers are proven above to add their literal NAME to the aggregate.
    # Raw extra test cases may reuse those already-built executable targets.
    source_text = {path: re.sub(r"(?m)#.*$", "", text)
                   for path, text in all_text_by_file.items()}
    combined = "\n".join(source_text.values())
    routed_inputs = {token for match in BUILD_INPUTS_RE.finditer(combined)
                     for token in match.group(1).split()}
    helper_targets = set(re.findall(
        r"\b(?:" + "|".join(HELPER_NAMES) + r")\s*\(\s*([A-Za-z_][A-Za-z0-9_.+-]*)",
        combined))

    for path, text in source_text.items():
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

            if target not in helper_targets and target not in routed_inputs:
                rel = path.relative_to(REPO_ROOT).as_posix()
                problems.append(
                    f"{rel}: add_test(NAME {test_name} COMMAND {target}) carries "
                    f"LABELS \"unit\" but no jce_test_build_dependencies({target}) "
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
    if not METADATA_CMAKE.is_file():
        problems.append("test domain/build metadata owner is missing")
    else:
        problems += check_build_dependency_authority(METADATA_CMAKE.read_text(encoding="utf-8"))
    if "jce_create_test_domain_targets()" not in re.sub(r"(?m)#.*$", "", root_text):
        problems.append("tests/CMakeLists.txt: domain build aggregates are not created")

    all_text_by_file: dict[Path, str] = {}
    cmake_files = source_cmake_files()
    if TESTS_ROOT_CMAKE not in cmake_files:
        problems.append("owned source enumeration did not include tests/CMakeLists.txt")
    for cmake_file in cmake_files:
        try:
            all_text_by_file[cmake_file] = cmake_file.read_text(
                encoding="utf-8", errors="replace"
            )
        except OSError as e:
            problems.append(f"cannot read owned CMake source {cmake_file}: {e}")

    problems += check_raw_registrations_wired(all_text_by_file)

    if not problems:
        print("jce_tests closure check: OK (shared all/domain routing and raw executable inputs)")
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
