#!/usr/bin/env python3
"""Gate the patented-codec (AAC / H.264 / H.265) build contract.

The contract has three clauses:

  1. `dist` NEVER carries the patented codecs.
  2. `release` carries them BY DEFAULT.
  3. A release build can OPT OUT, and that choice sticks.

All three were once true and two of them drifted, over months, with nothing
watching:

  * Clause 3 died in 26379e69 (v-0.9.6) — a commit about fonts and cross-arch
    verification — which added `JCE_ENABLE_PATENTED_CODECS` to the
    `_desktop-base` preset's cacheVariables.  A preset cacheVariable is applied
    like `-D` on EVERY configure and `-D` overwrites the cache, so an opt-out
    survived exactly one configure.  `tools/build/jce.py` pinned the same value a
    second time.
  * Clause 2 was silently negotiable: engine/CMakeLists.txt downgraded the
    effective switch to OFF with only a `message(WARNING)` when the (untracked)
    vendor trees were missing, and CMake does not re-emit that warning on an
    incremental reconfigure.
  * Clause 1 leaks on the prebuilt-SDK path, where `add_subdirectory(engine)`
    never runs and the engine-tree asserts therefore never execute.

This is a source-level gate: it asserts the *mechanisms* that keep those
clauses true, so the next well-meaning edit trips here instead of shipping.
It reads files only — no configure, no build.

Exit 0 = contract intact, 1 = violated.
"""
from __future__ import annotations

import ast
import json
import re
import sys
from pathlib import Path

# Optional argument: a tree to check instead of this checkout. Only used by the
# gate's own mutation test, which reintroduces each historical regression into
# a scratch copy and requires this script to catch it — a gate nobody has ever
# seen fail is not evidence of anything.
ROOT = (Path(sys.argv[1]).resolve() if len(sys.argv) > 1
        else Path(__file__).resolve().parents[2])
OPTION = "JCE_ENABLE_PATENTED_CODECS"
EFFECTIVE = "JCE_PATENTED_CODECS_ENABLED"

failures: list[str] = []


def fail(clause: str, msg: str) -> None:
    failures.append(f"[{clause}] {msg}")


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8", errors="replace")


CPP_QUOTES = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
CPP_TOKENS = re.compile(r'//[^\n]*|/\*.*?\*/|' + CPP_QUOTES, re.S)


def cpp_code(text: str) -> str:
    """Strip comments while preserving the SDK stamp/path string literals."""
    return CPP_TOKENS.sub(
        lambda m: re.sub(r"[^\n]", " ", m.group())
        if m.group().startswith(("//", "/*")) else m.group(), text)


def cpp_mask(text: str) -> str:
    return re.sub(CPP_QUOTES, lambda m: " " * len(m.group()), text)


def cpp_body(code: str, signature: str) -> str:
    """One actual definition/branch body; comments and strings cannot add braces."""
    mask = cpp_mask(code)
    match = re.search(signature + r"\s*\{", mask)
    if not match:
        return ""
    start, depth = match.end() - 1, 0
    for pos in range(start, len(mask)):
        if mask[pos] == "{":
            depth += 1
        elif mask[pos] == "}":
            depth -= 1
            if not depth:
                return code[start + 1:pos]
    return ""


def cmake_direct_set(text: str, condition: str, variable: str, value: str) -> bool:
    """A set directly in this condition, never borrowed from a later branch."""
    stack = []
    commands = re.finditer(r"(?m)^[ \t]*(if|elseif|else|endif|set)\s*\(([^\n)]*)\)", text)
    for match in commands:
        command, argument = match.group(1), " ".join(match.group(2).split())
        if command == "if":
            stack.append(argument)
        elif command in ("elseif", "else"):
            if stack:
                stack[-1] = None
        elif command == "endif":
            if stack:
                stack.pop()
        elif stack and stack[-1] == condition and argument == variable + " " + value:
            return True
    return False


# ── Clause 3: the preference must be writable exactly once, by the user ──
#
# Nothing that runs on every configure may stamp the option, or the opt-out is
# reverted behind the user's back.
presets = json.loads(read("CMakePresets.json"))
pinning = [p["name"] for p in presets.get("configurePresets", [])
           if OPTION in (p.get("cacheVariables") or {})]
if pinning:
    fail("opt-out",
         f"CMakePresets.json presets pin {OPTION} as a cacheVariable: "
         f"{', '.join(pinning)}. A preset cacheVariable is re-applied like -D "
         f"on every configure and overwrites the cache, so a release opt-out "
         f"is silently reverted by the next build. Pin JCE_BUILD_VARIANT "
         f"instead and let the root CMakeLists derive the effective switch.")

jce_py = read("tools/build/jce.py")
# Any code that emits -D<OPTION>=... is the same bug wearing a different hat
# unless it sits in codec_overrides(), which returns [] unless
# --patented-codecs was actually passed. Match on the AST rather than on text
# so prose about the bug (comments, docstrings — this file is full of it) is
# not mistaken for the bug.
tree = ast.parse(jce_py, filename="jce.py")
docstrings = set()
for node in ast.walk(tree):
    if isinstance(node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef,
                         ast.ClassDef)):
        body = getattr(node, "body", None)
        if (body and isinstance(body[0], ast.Expr)
                and isinstance(body[0].value, ast.Constant)
                and isinstance(body[0].value.value, str)):
            docstrings.add(id(body[0].value))

owner: dict[int, str] = {}
for fn in [n for n in ast.walk(tree)
           if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]:
    for sub in ast.walk(fn):
        owner.setdefault(id(sub), fn.name)

for node in ast.walk(tree):
    if not (isinstance(node, ast.Constant) and isinstance(node.value, str)):
        continue
    if id(node) in docstrings or ("-D" + OPTION) not in node.value:
        continue
    where = owner.get(id(node), "<module level>")
    if where == "codec_overrides":
        continue
    fail("opt-out",
         f"tools/build/jce.py emits -D{OPTION} from {where}() "
         f"(line {node.lineno}). Writing the preference on every configure is "
         f"what made the release opt-out unreachable; route it through "
         f"codec_overrides(), which emits nothing unless the user asked.")
if "def codec_overrides(" not in jce_py:
    fail("opt-out", "tools/build/jce.py lost codec_overrides() — the guarded, "
                    "opt-in-only emitter for the codec preference.")

# ── Clause 2: a release build may not quietly lose the codecs ────────────
engine_cml = read("engine/CMakeLists.txt")
# Original sources now come from fixed upstream downloads, not local vendor
# directories. Gate acquisition and fatal failure, with the same opt-out rule.
code = re.sub(r"(?m)^\s*#.*$", "", engine_cml)
if not re.search(r'option\(\s*' + OPTION + r'\s+"(?:\\.|[^"\\])*"\s+ON\s*\)',
                 code, re.S):
    fail("release-default", "the codec preference no longer defaults ON")
if re.search(r"set\(\s*" + EFFECTIVE + r"\s+OFF\b", code):
    fail("release-default", "engine/CMakeLists.txt silently downgrades the effective switch")
acquisition = re.search(r"if\(\s*" + EFFECTIVE + r"\s*\)(.*?)endif\(\)",
                        code, re.S)
for name, target, output in (("libhevc", "libhevcdec", "JCE_LIBHEVC_SOURCE_DIR"),
                             ("openh264", "openh264dec", "JCE_OPENH264_SOURCE_DIR"),
                             ("fdk-aac", "fdk-aac", "JCE_FDKAAC_SOURCE_DIR")):
    if not acquisition or not re.search(r"jce_vendor_source\(\s*" + name
                                        + r"\s+" + output + r"\s*\)",
                                        acquisition.group(1)):
        fail("release-default", f"missing guarded acquisition of pinned {name}")
    port = code if name == "fdk-aac" else read(f"engine/cmake/codec_ports/{name}/CMakeLists.txt")
    port = re.sub(r"(?m)^\s*#.*$", "", port)
    if not re.search(r"add_dependencies\(\s*" + target + r"\s+jce_vendor_verify_"
                     + name + r"\s*\)", port):
        fail("release-default", f"{target} lost its incremental source-verification dependency")
if not re.search(r'include\(\s*"\$\{CMAKE_CURRENT_SOURCE_DIR\}/cmake/JCEVendorSources\.cmake"\s*\)', code):
    fail("release-default", "engine/CMakeLists.txt lost the pinned-source provider")
provider = re.sub(r"(?m)^\s*#.*$", "", read("engine/cmake/JCEVendorSources.cmake"))
if not re.search(r"if\(NOT _result EQUAL 0\).*?message\(FATAL_ERROR", provider, re.S):
    fail("release-default", "pinned-source acquisition no longer fails hard")
if not re.search(r"add_custom_target\(.*?--verify-only", provider, re.S):
    fail("release-default", "pinned-source provider lost build-time verification")
pins = json.loads(read("contracts/vendor-sources.json"))["sources"]
for name in ("libhevc", "openh264", "fdk-aac"):
    pin = pins.get(name, {})
    if (not re.fullmatch(r"[0-9a-f]{64}", pin.get("sha256", ""))
            or not re.fullmatch(r"[0-9a-f]{40}", pin.get("version", ""))
            or not pin.get("url", "").endswith("/" + pin.get("version", ""))):
        fail("release-default", f"{name} lost its fixed commit or archive hash")

# ── Clause 1: dist forcing + its asserts must stay in the root ───────────
root_cml = read("CMakeLists.txt")
if not cmake_direct_set(root_cml, 'JCE_BUILD_VARIANT STREQUAL "dist"',
                        EFFECTIVE, "OFF"):
    fail("dist-forced",
         f"CMakeLists.txt no longer forces {EFFECTIVE} OFF for the dist "
         f"variant.")
if not re.search(r'JCE_BUILD_VARIANT STREQUAL "dist" AND ' + EFFECTIVE
                 + r'[\s\S]{0,200}?FATAL_ERROR', root_cml):
    fail("dist-forced",
         "CMakeLists.txt lost the assert that dist actually derived the "
         "switch OFF.")
if not re.search(r"JCE_PLATFORM_WEB AND " + EFFECTIVE
                 + r"[\s\S]{0,200}?FATAL_ERROR", root_cml):
    fail("dist-forced",
         "CMakeLists.txt lost the assert that Web derived the switch OFF.")

# ── Clause 1, prebuilt SDK: actual per-archive metadata admission ────────
# VERSION.txt is explanatory; the archive metadata is the variant authority.
try:
    driver_ast = ast.parse(jce_py)
except SyntaxError:
    driver_ast = ast.Module(body=[], type_ignores=[])
resolve = next((node for node in driver_ast.body
                if isinstance(node, ast.FunctionDef) and node.name == "resolve_sdk"), None)
calls = [] if resolve is None else [node for node in ast.walk(resolve)
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
    and node.func.attr == "require_sdk_target"]
if (len(calls) != 1 or len(calls[0].args) != 6
    or [ast.unparse(node) for node in calls[0].args][-2:] != ["variant", "delivery"]):
    fail("dist-forced", "resolve_sdk() lost actual per-archive SDK target/variant admission")

variant_owner = "editor/src/core/jce_project_build_target.cpp"
variant_code = cpp_code(read(variant_owner)) if (ROOT / variant_owner).is_file() else ""
variant_body = cpp_body(variant_code,
    r"\bstd::string\s+jce_project_build_sdk_target_error\s*\([^;{}]*\)")
variant_mask = cpp_mask(variant_body)
if (not variant_body or '"/jce-sdk-target.json"' not in variant_body
        or not re.search(r"\bjce_fs_host_read_capped\s*\([^;]*\b16385\b", variant_mask)
        or not re.search(r"\bjce_fs_buffer_free\s*\(", variant_mask)
        or not re.search(r"\bjce_json_parse_strict\s*\(", variant_mask)
        or not re.search(r'\{\s*"variant"\s*,\s*variant\.c_str\s*\(\s*\)\s*\}', variant_body)
        or not re.search(r"\bstd::strcmp\s*\(\s*text\(expected\.first\),\s*expected\.second\)", variant_mask)):
    fail("dist-forced", f"{variant_owner} lost actual SDK metadata variant comparison")

bm = cpp_code(read("editor/src/core/jce_build_manager.cpp"))
admission = cpp_body(bm,
    r"\bbool\s+jce_build_manager_start_project_build\s*\([^;{}]*\)")
mismatch = cpp_body(admission,
    r"\bif\s*\(\s*const\s+auto\s+error\s*=\s*"
    r"jce_project_build_sdk_target_error\s*\(\s*sdk\s*,\s*selected_arch\s*,\s*"
    r"build_platform\s*,\s*variant\s*\)\s*;\s*!error\.empty\s*\(\s*\)\s*\)")
if (not mismatch or not re.search(r"\breturn\s+false\s*;", cpp_mask(mismatch))
        or not re.search(r"\bset_error\s*\(\s*error\s*\)", cpp_mask(mismatch))):
    fail("dist-forced", "jce_build_manager_start_project_build() lost SDK target/variant rejection")

# ── Report ──────────────────────────────────────────────────────────────
if failures:
    print("check_patented_codec_gate: FAIL")
    for f in failures:
        print("  " + f)
    print("\nContract: dist is always royalty-free; release carries AAC / "
          "H.264 / H.265 by default; a release build can opt out and that "
          "choice persists.")
    sys.exit(1)

print("check_patented_codec_gate: OK (dist forced royalty-free, release "
      "defaults on, opt-out persists)")
sys.exit(0)
