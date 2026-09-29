#!/usr/bin/env python3
"""
check_sdk_scripting_export.py — every library scripting/ builds must either
SHIP in the SDK or say, by name and with a reason, why it does not.

WHY THIS EXISTS
---------------
The SDK a consumer project builds against installed the engine and NOTHING
from scripting/.  Measured on the SDK this repository produced on 2026-08-14
(dist/sdk/win32-x86_64, commit d3ecc452): find_package(JCE) declared JCE::JCE,
and no JCE::ScriptApi, no JCE::ScriptVm*, and no JCE_SCRIPT_* variable at all.
An out-of-tree project therefore could not enable Python, Java or C++
scripting whatever it wrote in its own CMakeLists — and got no error saying
so, because there was nothing to error about.  `if(TARGET
jce_script_vm_python)` was simply FALSE, which is also what it says on a
machine that merely has no CPython.

That failure is not interesting once.  It is interesting because it is the
DEFAULT: scripting/CMakeLists.txt discovers its subdirectories by glob, so a
backend added tomorrow appears in the build with no edit to any shared file —
and would ship in the SDK only if somebody remembered.  This gate is the
"somebody".

WHAT IT CHECKS
--------------
1. INSTALL COVERAGE.  Every `add_library(<name> ...)` under scripting/ is
   either named by an `install(TARGETS <name> ...)` in
   scripting/cmake/JCEScriptingInstall.cmake, or carries an explicit
   `# SDK-EXEMPT: <name> — <reason>` line there.  An empty reason is not a
   reason.

2. EXPORT PAIRING, BOTH DIRECTIONS.  A file that ships and that no consumer
   can name is not shipped; a target a consumer can name and that does not
   ship is worse — it configures and then fails at link with a path nobody
   wrote.  So every installed target must appear in
   scripting/cmake/JCEScripting.cmake.in, and every target the template
   declares an alias for must be installed.

3. REACHABILITY.  cmake/JCEConfig.cmake.in must include JCEScripting.cmake.
   Without that line the fragment installs and is never read: every check
   above stays green and every consumer still sees nothing.

4. DIRECTION.  The install rules must NOT live under engine/.  This is the
   same rule tools/audit/check_dependency_boundaries.py enforces for source
   and CMake files; it is repeated here because the tempting place to write
   `install(TARGETS jce_script_api ...)` is engine/cmake/JCESDKInstall.cmake,
   next to every other install rule in the tree.

Usage:
  python tools/audit/check_sdk_scripting_export.py           # human report
  python tools/audit/check_sdk_scripting_export.py --json    # machine report
  # exit 0 = clean, 1 = violations, 2 = a file this gate reads is missing.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

SCRIPTING_DIR = REPO_ROOT / "scripting"
INSTALL_RULES = SCRIPTING_DIR / "cmake" / "JCEScriptingInstall.cmake"
CONFIG_TEMPLATE = SCRIPTING_DIR / "cmake" / "JCEScripting.cmake.in"
ENGINE_SDK_INSTALL = REPO_ROOT / "engine" / "cmake" / "JCESDKInstall.cmake"
PACKAGE_CONFIG = REPO_ROOT / "cmake" / "JCEConfig.cmake.in"

_ADD_LIBRARY = re.compile(r"^\s*add_library\(\s*([A-Za-z0-9_]+)")
_INSTALL_TARGETS = re.compile(r"install\(\s*TARGETS\s+([A-Za-z0-9_]+)")
# "— " and "-- " and "-" all read as a dash to a human; accept any of them and
# require TEXT after it.  A gate that accepts `# SDK-EXEMPT: foo —` teaches
# people to write that.
_EXEMPT = re.compile(r"#\s*SDK-EXEMPT:\s*([A-Za-z0-9_]+)\s*[-—–]+\s*(\S.*)$")


def _read(path: Path, failures: list[str]) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        failures.append(f"cannot read {path.relative_to(REPO_ROOT).as_posix()}: {exc}")
        return ""


def declared_targets() -> dict[str, str]:
    """Every library scripting/ declares -> the file that declares it.

    Read from the tree and not from a list here, for the same reason
    scripting/CMakeLists.txt globs its subdirectories: a list is a thing
    somebody has to remember, and the failure of forgetting is silence.
    """
    out: dict[str, str] = {}
    for cml in sorted(SCRIPTING_DIR.rglob("CMakeLists.txt")):
        rel = cml.relative_to(REPO_ROOT).as_posix()
        for line in cml.read_text(encoding="utf-8", errors="replace").splitlines():
            m = _ADD_LIBRARY.match(line)
            if m:
                out.setdefault(m.group(1), rel)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", action="store_true", help="machine-readable report")
    args = ap.parse_args()

    failures: list[str] = []

    if not SCRIPTING_DIR.is_dir():
        print("check_sdk_scripting_export: no scripting/ directory — nothing "
              "to check.")
        return 0

    for required in (INSTALL_RULES, CONFIG_TEMPLATE, PACKAGE_CONFIG):
        if not required.is_file():
            failures.append(
                f"{required.relative_to(REPO_ROOT).as_posix()} is missing — the "
                f"SDK cannot ship the scripting layer without it")
    if failures:
        for f in failures:
            print(f"check_sdk_scripting_export: FAILED — {f}", file=sys.stderr)
        return 2

    rules_text = _read(INSTALL_RULES, failures)
    template_text = _read(CONFIG_TEMPLATE, failures)
    config_text = _read(PACKAGE_CONFIG, failures)

    installed = set(_INSTALL_TARGETS.findall(rules_text))
    exempt: dict[str, str] = {}
    for line in rules_text.splitlines():
        m = _EXEMPT.search(line)
        if m:
            exempt[m.group(1)] = m.group(2).strip()

    declared = declared_targets()

    # ---- 1. install coverage --------------------------------------- #
    for name, where in sorted(declared.items()):
        if name in installed or name in exempt:
            continue
        failures.append(
            f"{name} (declared in {where}) is neither installed by "
            f"{INSTALL_RULES.relative_to(REPO_ROOT).as_posix()} nor exempted "
            f"there. Add install(TARGETS {name} ...), or a line reading "
            f"'# SDK-EXEMPT: {name} — <why it must not ship>'.")

    # An exemption for a target that no longer exists is a stale promise; it
    # would silently cover a future target that happened to reuse the name.
    for name in sorted(exempt):
        if name not in declared:
            failures.append(
                f"SDK-EXEMPT names {name}, which scripting/ no longer declares. "
                f"Remove the exemption.")

    # ---- 2. export pairing, both directions ------------------------- #
    #
    # The test is the ALIAS and not "the name appears somewhere in the
    # template", which is what this check did first and which a mutation
    # walked straight through: deleting `add_library(jce_script_api ALIAS
    # JCE::ScriptApi)` left the string "jce_script_api" in the template
    # anyway — it is half of the library's FILE NAME two lines above.  The
    # alias is the property that matters: it is what makes
    # `if(TARGET jce_script_vm_python)` answer the same question in an
    # SDK consumer as it does in this tree.
    alias_re = re.compile(r"add_library\(\s*([A-Za-z0-9_]+)\s+ALIAS")
    aliases = set(alias_re.findall(template_text))

    for name in sorted(installed):
        if name not in aliases:
            failures.append(
                f"{name} is installed into the SDK but "
                f"{CONFIG_TEMPLATE.relative_to(REPO_ROOT).as_posix()} declares "
                f"no `add_library({name} ALIAS ...)` — the file ships and a "
                f"consumer's if(TARGET {name}) is FALSE, which is the exact "
                f"silence this gate exists to break.")

    for name in sorted(aliases):
        if name not in declared:
            # An alias whose in-tree twin does not exist means the two
            # spellings have drifted, which is the thing the aliases exist to
            # prevent.
            failures.append(
                f"{CONFIG_TEMPLATE.relative_to(REPO_ROOT).as_posix()} aliases "
                f"{name}, which scripting/ does not declare. The SDK name and "
                f"the in-tree name have drifted apart.")
        elif name not in installed and name not in exempt:
            failures.append(
                f"{CONFIG_TEMPLATE.relative_to(REPO_ROOT).as_posix()} aliases "
                f"{name} but nothing installs it — a consumer's "
                f"if(TARGET {name}) would be TRUE against a file that is not "
                f"in the SDK.")

    # ---- 3. reachability -------------------------------------------- #
    if "JCEScripting.cmake" not in config_text:
        failures.append(
            f"{PACKAGE_CONFIG.relative_to(REPO_ROOT).as_posix()} does not "
            f"include JCEScripting.cmake. The fragment would install and never "
            f"be read: every other check here stays green and every consumer "
            f"still sees no scripting targets.")
    elif not re.search(r'include\([^)]*JCEScripting\.cmake"?\s+OPTIONAL',
                       config_text):
        failures.append(
            f"{PACKAGE_CONFIG.relative_to(REPO_ROOT).as_posix()} includes "
            f"JCEScripting.cmake without OPTIONAL. An engine-only SDK does not "
            f"install that file, and find_package(JCE) would fail on it.")

    # ---- 4. direction ----------------------------------------------- #
    if ENGINE_SDK_INSTALL.is_file():
        engine_text = ENGINE_SDK_INSTALL.read_text(encoding="utf-8",
                                                   errors="replace")
        for name in sorted(declared):
            if re.search(rf"install\(\s*TARGETS\s+{re.escape(name)}\b",
                         engine_text):
                failures.append(
                    f"{ENGINE_SDK_INSTALL.relative_to(REPO_ROOT).as_posix()} "
                    f"installs {name}. Nothing under engine/ may name a target "
                    f"scripting/ declares — the install rules belong in "
                    f"{INSTALL_RULES.relative_to(REPO_ROOT).as_posix()}.")

    if args.json:
        print(json.dumps({
            "declared": declared,
            "installed": sorted(installed),
            "exempt": exempt,
            "failures": failures,
        }, indent=2, ensure_ascii=False))
        return 1 if failures else 0

    print(f"check_sdk_scripting_export: {len(declared)} scripting target(s), "
          f"{len(installed)} installed, {len(exempt)} exempt")
    for name, where in sorted(declared.items()):
        if name in installed:
            state = "SHIPS"
        elif name in exempt:
            state = f"exempt: {exempt[name]}"
        else:
            state = "NOT SHIPPED"
        print(f"  {name:<24} {state}")

    if failures:
        print(f"\ncheck_sdk_scripting_export: FAILED — {len(failures)} "
              f"problem(s)", file=sys.stderr)
        for f in failures:
            print(f"  * {f}", file=sys.stderr)
        return 1

    print("check_sdk_scripting_export: OK — everything scripting/ builds "
          "either ships or says why not.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
