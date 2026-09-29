#!/usr/bin/env python3
"""
check_script_vm_parity.py — JceScriptVM must stay the SAME lifecycle as the
public jce_script_* surface, not a second one that drifts.

WHAT PROBLEM THIS SOLVES
------------------------
The engine now calls UP into a language runtime through JceScriptVM
(engine/include/jce/middleware/script/jce_script_vm.h).  Its slots are the
public jce_script_* lifecycle functions with the signatures copied verbatim,
which is only true for as long as somebody keeps them equal.

Two failure modes, and neither one is visible from any test that exists:

  * A 20th lifecycle function is added to jce_script.h and implemented
    directly in jce_script.c, the way the first 19 used to be.  Lua gets it.
    Python, Java and C++ silently do not, forever, and nothing is red — the
    new function works perfectly in every test, because every test runs Lua.

  * A slot's parameter list drifts from the public one.  The public signature
    pin in jce_script_vm.c catches a TYPE change (the initialiser stops
    compiling).  It cannot catch a slot that was never added, because an
    initialiser cannot mention a member that does not exist.

So: extract both lists, require a bijection, and require every public entry
point to be a forwarder rather than an implementation.

WHAT COUNTS AS BREAKAGE
-----------------------
  MISSING SLOT       a public lifecycle function with no JceScriptVM slot
  ORPHAN SLOT        a slot with no public lifecycle function
  SIGNATURE DRIFT    same name, different normalised parameter list
  NOT A FORWARDER    a public lifecycle symbol defined in jce_script.c with
                     external linkage (i.e. bypassing the vtable)
  MISSING FORWARDER  a public lifecycle function with no definition in
                     jce_script_vm.c
  STALE EXCLUSION    an entry in EXCLUDED that no longer exists in the header

Every one of them exits 1 and names the function.

Usage:
  python tools/audit/check_script_vm_parity.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

PUBLIC_HEADER = REPO_ROOT / "engine/include/jce/middleware/script/jce_script.h"
VM_HEADER = REPO_ROOT / "engine/include/jce/middleware/script/jce_script_vm.h"
LUA_IMPL = REPO_ROOT / "engine/src/middleware/script/jce_script.c"
FORWARDERS = REPO_ROOT / "engine/src/middleware/script/jce_script_vm.c"

# Public lifecycle functions that deliberately have NO slot.  Each entry
# carries the reason, and the reason has to be a real one: a name listed here
# that no longer exists in the header is itself a failure, so this list cannot
# quietly outlive what it excuses.
EXCLUDED = {
    "jce_script_create":
        "the size-less spelling of create_sized. A slot for it would let a "
        "backend supply the size-less form and NOT the sized one, losing the "
        "short-host clamp that test_jce_script_host_abi.c guarantees. It "
        "stays an engine-side wrapper that calls create_sized with "
        "sizeof(JceScriptHost).",
}


def normalise_params(params: str) -> str:
    """A parameter list reduced to its TYPES, so `float dt` and `float x`
    compare equal and `float dt` and `double dt` do not."""
    params = re.sub(r"/\*.*?\*/", " ", params, flags=re.S)
    params = re.sub(r"\s+", " ", params).strip()
    if params in ("", "void"):
        return "void"
    out = []
    for raw in params.split(","):
        p = raw.strip()
        # Drop the parameter NAME: the trailing identifier, but only when
        # something recognisable as a type is left behind.
        m = re.match(r"^(.*?)([A-Za-z_]\w*)$", p)
        if m:
            head = m.group(1).strip()
            if head and (re.search(r"[A-Za-z_]", head) or "*" in head):
                p = head
        p = re.sub(r"\s*\*\s*", " *", p)
        out.append(re.sub(r"\s+", " ", p).strip())
    return ", ".join(out)


def parse_public(text: str) -> dict[str, tuple[str, str]]:
    """name -> (return type, normalised params) for every JCE_API function
    declared after the JceScript handle typedef (i.e. the lifecycle block)."""
    start = text.find("typedef struct JceScript JceScript;")
    if start < 0:
        sys.exit("check_script_vm_parity: could not find the JceScript "
                 "typedef in jce_script.h — the lifecycle block moved")
    body = text[start:]
    out = {}
    for m in re.finditer(
            r"^JCE_API\s+(.+?)\b(jce_script_\w+)\s*\((.*?)\)\s*;",
            body, re.M | re.S):
        ret, name, params = m.group(1), m.group(2), m.group(3)
        out[name] = (re.sub(r"\s+", " ", ret).strip().rstrip("*").strip()
                     + ("*" if ret.rstrip().endswith("*") else ""),
                     normalise_params(params))
    return out


def parse_slots(text: str) -> dict[str, tuple[str, str]]:
    """slot name -> (return type, normalised params) from struct JceScriptVM."""
    m = re.search(r"struct JceScriptVM\s*\{(.*?)\n\};", text, re.S)
    if not m:
        sys.exit("check_script_vm_parity: could not find struct JceScriptVM "
                 "in jce_script_vm.h")
    body = re.sub(r"/\*.*?\*/", " ", m.group(1), flags=re.S)
    out = {}
    for d in re.finditer(
            r"(\w[\w\s]*?)\s*\(\s*\*\s*(\w+)\s*\)\s*\((.*?)\)\s*;",
            body, re.S):
        ret, name, params = d.group(1), d.group(2), d.group(3)
        ret = re.sub(r"\s+", " ", ret).strip()
        # `JceScript *(*create_sized)` puts the star with the return type.
        star = "*" if ret.endswith("*") else ""
        out[name] = (ret.rstrip("*").strip() + star, normalise_params(params))
    # `JceScript *(*create_sized)(...)` is not matched by the \w-only return
    # pattern above when the star binds to the type; catch that form too.
    for d in re.finditer(
            r"(\w[\w\s]*?)\s*\*\s*\(\s*\*\s*(\w+)\s*\)\s*\((.*?)\)\s*;",
            body, re.S):
        ret, name, params = d.group(1), d.group(2), d.group(3)
        out[name] = (re.sub(r"\s+", " ", ret).strip() + "*",
                     normalise_params(params))
    return out


def defined_at_top_level(text: str, name: str) -> bool:
    """A definition of `name` with EXTERNAL linkage, at column 0."""
    return re.search(r"^(?!static\b)[A-Za-z_][\w \t*]*?\b%s\s*\(" %
                     re.escape(name), text, re.M) is not None


def main() -> int:
    pub_text = PUBLIC_HEADER.read_text(encoding="utf-8")
    vm_text = VM_HEADER.read_text(encoding="utf-8")
    lua_text = LUA_IMPL.read_text(encoding="utf-8")
    fwd_text = FORWARDERS.read_text(encoding="utf-8")

    public = parse_public(pub_text)
    slots = parse_slots(vm_text)
    failures: list[str] = []

    if not public:
        failures.append("parsed ZERO public lifecycle functions out of "
                        "jce_script.h — the extractor is broken, and an empty "
                        "list would make every check below vacuously pass")
    if not slots:
        failures.append("parsed ZERO slots out of struct JceScriptVM — same "
                        "problem in the other direction")
    if failures:
        for f in failures:
            print(f"  FAIL  {f}")
        print("\ncheck_script_vm_parity: FAILED")
        return 1

    for name, why in sorted(EXCLUDED.items()):
        if name not in public:
            failures.append(
                f"STALE EXCLUSION  {name} is excluded from the vtable but no "
                f"longer exists in jce_script.h. Drop the entry.  (Reason on "
                f"record: {why})")

    expected = {n: v for n, v in public.items() if n not in EXCLUDED}

    for name, (ret, params) in sorted(expected.items()):
        slot = name[len("jce_script_"):]
        if slot not in slots:
            failures.append(
                f"MISSING SLOT  {name} has no JceScriptVM slot '{slot}'. "
                f"Every language runtime reaches the engine's lifecycle "
                f"through that struct; a function that is not in it works in "
                f"Lua and silently does nothing in Python, Java and C++. Add "
                f"the slot (APPEND ONLY, at the end), the forwarder in "
                f"jce_script_vm.c, and fill it in every implementation.")
            continue
        sret, sparams = slots[slot]
        if sparams != params:
            failures.append(
                f"SIGNATURE DRIFT  {name}\n"
                f"        header: ({params})\n"
                f"        slot:   ({sparams})")
        if sret != ret:
            failures.append(
                f"SIGNATURE DRIFT  {name} returns '{ret}' but slot '{slot}' "
                f"returns '{sret}'")

        if defined_at_top_level(lua_text, name):
            failures.append(
                f"NOT A FORWARDER  {name} is defined with external linkage in "
                f"{LUA_IMPL.relative_to(REPO_ROOT)}. The public symbol must be "
                f"the forwarder in {FORWARDERS.relative_to(REPO_ROOT)}; the "
                f"Lua body belongs behind `static` and reaches callers only "
                f"through its vtable slot.")
        if not defined_at_top_level(fwd_text, name):
            failures.append(
                f"MISSING FORWARDER  {name} has no definition in "
                f"{FORWARDERS.relative_to(REPO_ROOT)}.")

    for slot in sorted(slots):
        if "jce_script_" + slot not in public:
            failures.append(
                f"ORPHAN SLOT  JceScriptVM.{slot} has no public "
                f"jce_script_{slot}. The vtable is the public lifecycle, not "
                f"a second one — if the engine needs this, it needs a public "
                f"entry point too.")

    print(f"check_script_vm_parity: {len(public)} public lifecycle "
          f"function(s), {len(slots)} slot(s), "
          f"{len(EXCLUDED)} documented exclusion(s).")

    if failures:
        print()
        for f in failures:
            print(f"  FAIL  {f}")
        print(f"\ncheck_script_vm_parity: FAILED — {len(failures)} "
              f"disagreement(s) between the public lifecycle and JceScriptVM.")
        return 1

    print("check_script_vm_parity: OK — the vtable is the public lifecycle.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
