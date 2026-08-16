#!/usr/bin/env python3
"""
emit_java.py — the Java backend.  One of N; see scriptgen_core.py's docstring
for how a backend registers itself (a sorted glob over emit_*.py, no central
list, nothing else edited).

WHAT JAVA BINDS, AND WHY IT IS NOT WHAT LUA BINDS
-------------------------------------------------
The Lua backend emits `static int l_jce_<name>(lua_State *L)` INSIDE the
engine.  Java cannot do that: it has no lua_State and does not link the engine.
It binds `scripting/c_abi` — the shared library that exports each manifest
entry's JceScriptHost member flattened to plain C, with `void *user` replaced
by an opaque `JceScriptApi *`.  So the call chain is

    Java method  ->  JNI shim  ->  jce_script_api_<name>  ->  host member

and the JNI shim is the only C this backend owns.

THE C ABI IS A TRANSPORT, NOT THE SURFACE.  Its own generated header says so
three times, and the difference is not cosmetic:

  * `index_base` — the ABI "passes the host's own 0-based index straight
    through"; the SURFACE is 1-based.
  * `out_capacity` — the ABI takes `max` as a caller parameter; the SURFACE
    has a fixed capacity the binding supplies.
  * `owned_string_release` — the ABI copies into a caller buffer and returns a
    length; the SURFACE yields a string or an absence.
  * `miss_value` — the ABI returns false and zero-fills; the SURFACE renders
    the miss as the manifest's own value.

This backend re-applies every one of those manifest decisions on top of the
transport, so that the Java surface IS the surface Lua exposes.  That is what
makes the cross-language differential a comparison of two renderings of one
contract rather than a comparison of two different contracts.

NAMING — one rule, derived, never hand-listed
---------------------------------------------
    method / parameter / record component   snake_case -> lowerCamelCase
    result record type                      snake_case -> UpperCamelCase + "Result"
    constant                                snake_case -> UPPER_SNAKE_CASE
    JNI native                              "n" + UpperCamelCase of the entry
    exported JNI symbol                     Java_com_jce_script_JceScript_<native>

`_split_snake` is the ONE implementation; every name above goes through it.
Three ways the rule can fail are conditions of this backend, not comments:
an empty path component (`get__x`), a Java reserved word, and two entries whose
camelCase forms collide.  All three fail by name in `validate()`; see
test_script_java_gate.py.

Its tests: test_script_java_gate.py  (the emitter)
           tests/scripting/java/                       (the differential)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from scriptgen_core import (                               # noqa: E402
    Artefact,
    HEADER,
    HostMember,
    MODIFIERS,
    Param,
    REPO_ROOT,
    ScriptBackend,
    flatten_pod,
    out_params,
    script_in_params,
)

# ── The two trees this backend owns ──────────────────────────────────────────
JAVA_DIR = REPO_ROOT / "scripting/java"
TEST_DIR = REPO_ROOT / "tests/scripting/java"

JAVA_PKG = "com.jce.script"
JAVA_CLASS = "JceScript"
JAVA_SRC = JAVA_DIR / "src/main/java/com/jce/script/JceScript.java"
JNI_C = JAVA_DIR / "native/jce_script_jni.gen.c"

DIFF_PKG = "com.jce.script.diff"
DIFF_HOST_H = TEST_DIR / "jce_java_diff_host.gen.h"
DIFF_HOST_C = TEST_DIR / "jce_java_diff_host.gen.c"
DIFF_LUA_C = TEST_DIR / "jce_java_diff_lua_reference.gen.c"
DIFF_JNI_C = TEST_DIR / "jce_java_diff_jni.gen.c"
DIFF_HOST_JAVA = TEST_DIR / "src/com/jce/script/diff/JceDiffHost.java"
DIFF_MAIN_JAVA = TEST_DIR / "src/com/jce/script/diff/JceDifferential.java"

JNI_PREFIX = "Java_" + JAVA_PKG.replace(".", "_") + "_" + JAVA_CLASS + "_"
DIFF_JNI_PREFIX = "Java_" + DIFF_PKG.replace(".", "_") + "_JceDiffHost_"

# The shared library the JNI shim is linked against and the Java class loads.
JAVA_LIB_NAME = "jce_script_java"
DIFF_LIB_NAME = "jce_script_java_diff"


# ── Names ────────────────────────────────────────────────────────────────────

# Reserved in the Java language (JLS 3.9), plus the three literals, which are
# also not usable as identifiers.  `record`, `var`, `yield` and `sealed` are
# CONTEXTUAL keywords and are legal method names, so they are deliberately not
# here -- rejecting them would refuse a name Java accepts.
_JAVA_RESERVED = frozenset("""
abstract assert boolean break byte case catch char class const continue default
do double else enum extends final finally float for goto if implements import
instanceof int interface long native new package private protected public
return short static strictfp super switch synchronized this throw throws
transient try void volatile while _ true false null
""".split())

_SNAKE_RE = re.compile(r"^[a-z][a-z0-9]*(_[a-z0-9]+)*$")


def _split_snake(name: str) -> list[str]:
    """snake_case -> its parts.  The ONE place a name is decomposed.

    An empty part (`get__x`, `_x`, `x_`) is a hard failure rather than a
    silently dropped segment: `get__position` and `get_position` would
    otherwise both camel to `getPosition` and one would overwrite the other in
    the emitted class with no diagnostic at all."""
    parts = name.split("_")
    if any(p == "" for p in parts):
        raise SystemExit(
            f"error: emit_java: manifest name {name!r} has an empty path "
            f"component -- its camelCase form would silently collide with "
            f"another entry's. Rename the entry.")
    return parts


def camel(name: str) -> str:
    p = _split_snake(name)
    return p[0] + "".join(w[:1].upper() + w[1:] for w in p[1:])


def pascal(name: str) -> str:
    return "".join(w[:1].upper() + w[1:] for w in _split_snake(name))


def constant_name(name: str) -> str:
    return "_".join(_split_snake(name)).upper()


def native_name(entry_name: str) -> str:
    return "n" + pascal(entry_name)


def result_type(entry_name: str) -> str:
    return pascal(entry_name) + "Result"


# ── Types ────────────────────────────────────────────────────────────────────
#
# Mapped by WIDTH AND KIND, never by signedness: Java has no unsigned integer
# type, and widening uint32_t to `long` to "keep the range" would make the
# Java surface disagree with the C ABI about the type of a parameter.  The 32
# bits round-trip exactly; Integer.toUnsignedLong recovers the value.  The
# generated Javadoc says so on every affected entry, derived from this table.
JAVA_SCALAR = {
    "bool": "boolean",
    "int": "int",
    "uint32_t": "int",
    "uint64_t": "long",
    "JceScriptEntity": "long",
    "float": "float",
    "double": "double",
    "const char *": "String",
}
UNSIGNED_C = frozenset({"uint32_t", "uint64_t", "JceScriptEntity"})

JNI_TYPE = {"boolean": "jboolean", "int": "jint", "long": "jlong",
            "float": "jfloat", "double": "jdouble", "String": "jstring"}
JNI_ARRAY = {"boolean": "jbooleanArray", "int": "jintArray",
             "long": "jlongArray", "float": "jfloatArray",
             "double": "jdoubleArray"}
JNI_REGION = {"boolean": "Boolean", "int": "Int", "long": "Long",
              "float": "Float", "double": "Double"}
JAVA_BOX = {"boolean": "Boolean", "int": "Integer", "long": "Long",
            "float": "Float", "double": "Double"}

# The order buckets are declared in, so the native signature is a function of
# the manifest and not of dict iteration order.
_BUCKET_ORDER = ("boolean", "int", "long", "float", "double")


def _java_scalar(c_type: str, where: str) -> str:
    t = JAVA_SCALAR.get(c_type)
    if t is None:
        raise SystemExit(
            f"error: emit_java: {where}: no Java mapping for C type "
            f"{c_type!r}. Mark the binding hand_written rather than adding a "
            f"mapping for a type the other backends have not agreed on.")
    return t


def _elem_c_type(c_type: str) -> str:
    """`const float` (an array input) -> `float`."""
    return c_type[len("const "):] if c_type.startswith("const ") else c_type


# ── Modifiers this backend claims to handle ──────────────────────────────────
#
# Every one of the nine is rendered somewhere below.  The set is checked
# against the core's MODIFIERS in validate() so that a TENTH modifier added to
# the shared vocabulary fails here by name instead of being silently ignored
# in emitted Java -- which is exactly what `.get()` on an unknown key does.
_HANDLED_MODIFIERS = frozenset({
    "bind_args",      # the C ABI binds the argument; Java has no parameter
    "out_capacity",   # the wrapper allocates it; the ABI takes it as `max`
    "index_base",     # the wrapper subtracts it and early-returns below it
    "optional",       # convenience overloads over the trailing optional run
    "release",        # the ABI releases before returning; Java owns nothing
    "absent_value",   # the ABI returns it; Java forwards
    "miss_value",     # Java renders absence as null; the differential maps it
    "clamp_min",      # the ABI clamps; Java forwards
    "strict",         # Java's type system; there is no runtime check to emit
})


# ── Out parameters, decomposed ───────────────────────────────────────────────

_POD_ELEM_RE = re.compile(r"^(?P<base>\w+)\[(?P<i>\d+)\]$")

# A component is one value the SURFACE yields:
#   c_decl   how the JNI shim declares the local the ABI fills
#   c_pass   what it passes to the ABI
#   slots    [(java_elem_type, C expression for that slot)]
Component = dict


def out_components(m: HostMember, ent: dict, header_text: str) -> list[Component]:
    """The out parameters of one entry, as SURFACE components.

    flatten_pod is the neutral primitive and it flattens to SCALARS --
    raycast's JceScriptRaycastHit becomes eight of them.  Java wants the
    struct's own fields back (`float[] point`, not `point0, point1, point2`),
    so consecutive `name[i]` scalars are regrouped here.  Regrouped, not
    re-parsed: nothing below reads the header a second time, so there is no
    second opinion about the struct's layout."""
    comps: list[Component] = []
    for p in out_params(m):
        if p.arity > 0:                       # `T name[N]`
            elem = _java_scalar(_elem_c_type(p.c_type), f"{ent['name']}.{p.name}")
            comps.append({
                "c_name": p.name, "java_name": camel(p.name), "elem": elem,
                "count": p.arity, "is_array": True,
                "c_decl": f"{_elem_c_type(p.c_type)} {p.name}[{p.arity}]",
                "c_init": f"memset({p.name}, 0, sizeof {p.name});",
                "c_pass": p.name,
                "slots": [f"{p.name}[{i}]" for i in range(p.arity)],
            })
        elif p.c_type in JAVA_SCALAR:         # `T *name`
            elem = _java_scalar(p.c_type, f"{ent['name']}.{p.name}")
            comps.append({
                "c_name": p.name, "java_name": camel(p.name), "elem": elem,
                "count": 1, "is_array": False,
                "c_decl": f"{p.c_type} {p.name}",
                "c_init": f"memset(&{p.name}, 0, sizeof {p.name});",
                "c_pass": f"&{p.name}",
                "slots": [p.name],
            })
        else:                                 # a POD out struct
            fields: list[tuple[str, str, int]] = []   # (base, c elem type, n)
            for expr, ct in flatten_pod(header_text, p.c_type):
                mm = _POD_ELEM_RE.match(expr)
                base = mm.group("base") if mm else expr
                if fields and fields[-1][0] == base:
                    fields[-1] = (base, ct, fields[-1][2] + 1)
                else:
                    fields.append((base, ct, 1))
            for base, ct, n in fields:
                elem = _java_scalar(ct, f"{ent['name']}.{p.name}.{base}")
                comps.append({
                    "c_name": base, "java_name": camel(base), "elem": elem,
                    "count": n, "is_array": n > 1,
                    "c_decl": None,           # the struct itself is declared
                    "c_init": None,
                    "c_pass": None,
                    "slots": ([f"{p.name}.{base}[{i}]" for i in range(n)]
                              if n > 1 else [f"{p.name}.{base}"]),
                })
            comps.append({                    # the struct local, no component
                "c_name": p.name, "java_name": None, "elem": None,
                "count": 0, "is_array": False,
                "c_decl": f"{p.c_type} {p.name}",
                "c_init": f"memset(&{p.name}, 0, sizeof {p.name});",
                "c_pass": f"&{p.name}",
                "slots": [],
            })
    return comps


def _value_components(comps: list[Component]) -> list[Component]:
    return [c for c in comps if c["java_name"] is not None]


def slot_count(comps: list[Component]) -> int:
    return sum(len(c["slots"]) for c in comps)


def buckets_of(comps: list[Component]) -> list[tuple[str, list[str]]]:
    """[(java elem type, [C slot expressions])] in _BUCKET_ORDER.

    One caller-supplied Java array per PRIMITIVE TYPE rather than one per out
    parameter, so `get_touch`'s (uint64, float, float, float) crosses in two
    arrays instead of four.  Fixed order, so the native's signature is derived
    from the manifest and not from iteration order."""
    by: dict[str, list[str]] = {}
    for c in _value_components(comps):
        by.setdefault(c["elem"], []).extend(c["slots"])
    return [(t, by[t]) for t in _BUCKET_ORDER if t in by]


def bucket_index(comps: list[Component]) -> dict[str, list[tuple[str, int]]]:
    """component c_name -> [(bucket java type, index within that bucket)]."""
    pos: dict[str, int] = {}
    out: dict[str, list[tuple[str, int]]] = {}
    for c in _value_components(comps):
        t = c["elem"]
        idx = []
        for _ in c["slots"]:
            idx.append((t, pos.get(t, 0)))
            pos[t] = pos.get(t, 0) + 1
        out[c["c_name"]] = idx
    return out


# ── The Java-visible shape of one entry ──────────────────────────────────────

def java_in_params(m: HostMember, ent: dict) -> list[Param]:
    """The parameters the JAVA CALLER supplies.

    script_in_params already drops the out-capacity `max`; bind_args are
    dropped here for the same reason the C ABI drops them: jump_pressed,
    sprint and attack_pressed are three entries over one host member, and
    passing the button through would make them one function with a wider
    surface than the scripting surface."""
    bind = ent.get("bind_args") or {}
    return [p for p in script_in_params(m, ent) if p.name not in bind]


def java_param_type(p: Param, where: str) -> str:
    if p.arity > 0:
        return _java_scalar(_elem_c_type(p.c_type), where) + "[]"
    return _java_scalar(p.c_type, where)


def optional_tail(m: HostMember, ent: dict) -> int:
    """How many TRAILING parameters carry a manifest default.

    Java overloads can only drop a suffix.  gas_apply's `op` is optional and
    is followed by a REQUIRED `magnitude`, so its tail is 1, not 2 -- dropping
    `op` would slide `magnitude` into its slot.  The full-arity method always
    exists, so a caller that wants a non-trailing default passes it."""
    opt = ent.get("optional") or {}
    ps = java_in_params(m, ent)
    n = 0
    for p in reversed(ps):
        if p.name in opt:
            n += 1
        else:
            break
    return n


def java_return(m: HostMember, ent: dict, comps: list[Component]) -> str:
    """The Java type of one entry, one rule per shape.

    fallible_out and first_and_count reduce to a SINGLE component in seven of
    the ten cases, and wrapping `float[3]` in a one-field record would make
    `getPosition(e).outXyz()[0]` the only way to read a position.  So: one
    component yields that component's type directly (boxed when it is a
    scalar, because absence has to be representable); more than one yields a
    generated record.  Uniform, derived, and it never produces a record whose
    only job is to hold one array."""
    shape = ent["shape"]
    if shape == "void_call":
        return "void"
    if shape == "value_return":
        return _java_scalar(m.ret, ent["name"])
    if shape == "void_out_array":
        c = _value_components(comps)[0]
        return c["elem"] + "[]"
    if shape == "entity_table":
        return "long[]"
    if shape == "owned_string_release":
        return "String"
    if shape in ("fallible_out", "first_and_count"):
        vc = (_value_components(comps) if shape == "fallible_out"
              else [{"elem": "long", "is_array": False}, {"elem": "int"}])
        if shape == "fallible_out" and len(vc) == 1:
            c = vc[0]
            return c["elem"] + "[]" if c["is_array"] else JAVA_BOX[c["elem"]]
        return result_type(ent["name"])
    raise SystemExit(f"error: emit_java: {ent['name']}: unknown shape "
                     f"{shape!r} -- the seven are closed (scriptgen_core.SHAPES)")


def needs_record(m: HostMember, ent: dict, comps: list[Component]) -> bool:
    return java_return(m, ent, comps) == result_type(ent["name"])


# ── Literals ─────────────────────────────────────────────────────────────────

def java_literal(java_type: str, v) -> str:
    if v is None:
        return "null"
    if java_type == "boolean":
        return "true" if v else "false"
    if java_type == "String":
        return '"' + str(v) + '"'
    if java_type == "float":
        return f"{float(v)!r}f"
    if java_type == "double":
        return repr(float(v))
    if java_type == "long":
        return f"{int(v)}L"
    return str(int(v))


def lua_literal(java_type: str, v) -> str:
    if v is None:
        return "nil"
    if java_type == "boolean":
        return "true" if v else "false"
    if java_type == "String":
        return "'" + str(v) + "'"
    if java_type in ("float", "double"):
        return repr(float(v))
    return str(int(v))


# ═════════════════════════════════════════════════════════════════════════════
#  Artefact 1 — the Java surface
# ═════════════════════════════════════════════════════════════════════════════

_JAVA_BANNER = '''\
/* JceScript.java -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * The scripting surface for Java, bound through JNI to the C ABI shared
 * library (scripting/c_abi).  Sources of truth:
 *   struct JceScriptHost   engine/include/jce/middleware/script/jce_script.h
 *   the decisions          engine/src/middleware/script/script_exposure.json
 *
 * THE SURFACE, NOT THE TRANSPORT.  The C ABI this class calls is deliberately
 * a different thing from the scripting surface: it passes 0-based indices,
 * takes an out-capacity as a parameter, copies owned strings into a caller
 * buffer, and reports a miss as `false` with zeroed outputs.  Every one of
 * those is re-applied here from the manifest, so that a Java script and a Lua
 * script see the SAME contract.  That equivalence is what
 * tests/scripting/java/ compares, entry by entry, against the Lua bindings.
 *
 * ABSENCE.  A host callback may be NULL and a shorter host leaves later
 * members NULL, so every entry has an absent answer:
 *   fallible_out / owned_string_release  -> null
 *   value_return of a C string           -> "" (never null: the Lua binding
 *                                          pushes the empty string, and
 *                                          `tr` echoes its own key)
 *   value_return of a number / boolean   -> 0 / false, or the manifest's
 *                                          absent_value where it has one
 *   void_out_array                       -> zeros
 *
 * UNSIGNED.  Java has no unsigned integer type.  uint32_t crosses as `int`
 * and uint64_t / JceScriptEntity as `long`, bit for bit; Integer.toUnsignedLong
 * and Long.toUnsignedString recover the value.  Widening them instead would
 * make this class disagree with the C ABI about a parameter's type.
 *
@EXCLUDED@ */
package com.jce.script;

'''


def _wrap_c(text: str, prefix: str, width: int = 78) -> list[str]:
    out: list[str] = []
    cur = prefix
    for word in text.split():
        cand = cur + word if cur == prefix else cur + " " + word
        if len(cand) > width and cur != prefix:
            out.append(cur)
            cur = prefix + word
        else:
            cur = cand
    if cur != prefix:
        out.append(cur)
    return out


def _excluded_block(man: dict) -> str:
    """The seven hand-written entries and the one constant, with the
    manifest's OWN reason text -- the same text the C ABI header prints.

    Emitted rather than written out, so that an entry which loses its reason,
    or an eighth that appears, cannot leave this class quietly claiming to be
    the whole surface."""
    L = [" * NOT ON THIS SURFACE.",
         " *",
         " * Seven entries are hand-written in the engine and excluded from the C",
         " * ABI as a class -- three are Lua-VM machinery and two carry sandbox",
         " * policy that lives in static functions in jce_script.c. Copying that",
         " * policy here would be a second implementation of a security decision.",
         " * The manifest's own reasons:",
         " *"]
    for e in man["hand_written"]:
        L.append(f" *   {e['name']}")
        L += _wrap_c(e["reason"], " *       ")
    L.append(" *")
    for c in man["constants"]:
        L += _wrap_c(
            f"{c['name']} ({c['kind']}) has no Java form: a lightuserdata "
            f"sentinel is a Lua VM object compared by identity, and its only "
            f"producer is asset_read_json, which is one of the seven above. A "
            f"Java constant here could never be compared against anything.",
            " *   ")
    return "\n".join(L)


def _javadoc(m: HostMember, ent: dict, comps: list[Component]) -> list[str]:
    L = ["    /**"]
    if ent.get("doc"):
        L += _wrap_c(ent["doc"], "     * ")
    else:
        L.append(f"     * jce.{ent['name']}")
    L.append(f"     * <p>Shape {ent['shape']}, since {ent['since']};"
             f" host member {ent['vtable']}.")
    if ent.get("bind_args"):
        b = ", ".join(f"{k}={v}" for k, v in ent["bind_args"].items())
        L += _wrap_c(f"The binding supplies {b} (manifest bind_args), so it is "
                     f"not a parameter here.", "     * ")
    if ent.get("index_base"):
        L += _wrap_c(f"Indices are {ent['index_base']}-based, as on the Lua "
                     f"surface; anything below {ent['index_base']} answers "
                     f"absent without calling the host.", "     * ")
    if ent.get("out_capacity"):
        L += _wrap_c(f"At most {ent['out_capacity']} results (manifest "
                     f"out_capacity).", "     * ")
    if isinstance(ent.get("absent_value"), dict):
        L += _wrap_c(f"With no host this returns {ent['absent_value']['param']} "
                     f"itself.", "     * ")
    elif ent.get("absent_value") is not None:
        L += _wrap_c(f"With no host this returns {ent['absent_value']}, not 0.",
                     "     * ")
    if "clamp_min" in ent:
        L.append(f"     * Clamped to a minimum of {ent['clamp_min']}.")
    if ent.get("strict"):
        L += _wrap_c(
            f"The Lua binding type-checks {', '.join(ent['strict'])} at run "
            f"time; here the Java compiler does it, which is strictly "
            f"stronger and leaves nothing to emit.", "     * ")
    if ent["shape"] == "owned_string_release":
        L += _wrap_c(
            "The host's string is released by the C ABI before it returns, so "
            "nothing here owns native memory.", "     * ")
    if ent.get("miss_value") is not None:
        L += _wrap_c(
            f"A miss is null. (The Lua binding renders the same miss as the "
            f"number {ent['miss_value']}; absence is uniformly null here.)",
            "     * ")
    for p in java_in_params(m, ent):
        if p.c_type in UNSIGNED_C or _elem_c_type(p.c_type) in UNSIGNED_C:
            L.append(f"     * @param {camel(p.name)} {p.c_type}, unsigned; "
                     f"the bits round-trip exactly")
        else:
            L.append(f"     * @param {camel(p.name)} {p.c_type}")
    L.append("     */")
    return L


def _record_decl(m: HostMember, ent: dict, comps: list[Component]) -> list[str]:
    """One result type: a final class with public final fields.

    A `record` would be shorter and this deliberately is not one.  Records need
    Java 16, and engine/java/AGENTS.md fixes this repository's Java surface at
    **Java 11**.  Owner decision 8 rejected the Panama FFM API on exactly that
    ground -- "it needs a recent JDK and would raise the engine's minimum
    supported Java runtime ... a decision about who can ship a game" -- so
    raising it here for four lines of syntax would spend the same budget the
    binding strategy was chosen to protect.  build_java.py passes
    `--release 11`, which is what fails if a `record` reappears."""
    if ent["shape"] == "first_and_count":
        fields = [("Long", "first",
                   "the first match, or null when count is 0"),
                  ("int", "count", "how many the host found")]
    else:
        fields = []
        for c in _value_components(comps):
            t = c["elem"] + "[]" if c["is_array"] else c["elem"]
            fields.append((t, c["java_name"], f"out parameter {c['c_name']}"))
    rt = result_type(ent["name"])
    L = [f"    /** {ent['name']}: the host's answer when it has one. */",
         f"    public static final class {rt} {{"]
    for t, n, doc in fields:
        L.append(f"        /** {doc}. */")
        L.append(f"        public final {t} {n};")
    L.append("")
    L.append(f"        {rt}({', '.join(f'{t} {n}' for t, n, _ in fields)}) {{")
    for _, n, _ in fields:
        L.append(f"            this.{n} = {n};")
    L += ["        }", "    }", ""]
    return L


def _native_decl(m: HostMember, ent: dict, comps: list[Component]) -> str:
    """The private native declaration: primitives and caller-owned arrays only.

    NOTHING here returns an object except a String, and no native creates an
    object it does not immediately return.  That is the local-reference
    discipline, and it is a property of the SIGNATURES, not of the bodies:
    with out values crossing in arrays the caller already owns, the shim has
    nothing to allocate.

    WHAT FAILS IF THAT STOPS BEING TRUE is
    test_script_java_gate.py's
    test_no_native_creates_more_than_one_object (with
    test_the_shim_contains_no_loop, which is what keeps that count STATIC).
    NOT -Xcheck:jni, which this docstring used to name: measured, a shim
    mutated to leak 200,000 local references inside one native frame runs the
    entire differential GREEN under it.  -Xcheck:jni catches reference MISUSE
    -- a double DeleteLocalRef aborts the JVM with "Bad global or local ref
    passed to JNI", also measured -- and that is why it stays in the run; it
    does not report accumulation."""
    shape = ent["shape"]
    args = ["long api"]
    for p in java_in_params(m, ent):
        args.append(f"{java_param_type(p, ent['name'])} {camel(p.name)}")
    if shape in ("fallible_out", "void_out_array"):
        for t, slots in buckets_of(comps):
            args.append(f"{t}[] out{t[:1].upper() + t[1:]}")
        ret = "boolean" if shape == "fallible_out" else "void"
    elif shape in ("first_and_count", "entity_table"):
        args.append("long[] out")
        ret = "int"
    elif shape == "owned_string_release":
        ret = "String"
    elif shape == "value_return":
        ret = _java_scalar(m.ret, ent["name"])
    else:
        ret = "void"
    return (f"    private static native {ret} {native_name(ent['name'])}"
            f"({', '.join(args)});")


def _java_method(m: HostMember, ent: dict, comps: list[Component],
                 drop: int) -> list[str]:
    """One public method.  `drop` trailing optional parameters are supplied
    from the manifest instead of by the caller."""
    name = camel(ent["name"])
    shape = ent["shape"]
    ins = java_in_params(m, ent)
    kept = ins[:len(ins) - drop] if drop else ins
    opt = ent.get("optional") or {}
    sig = ", ".join(f"{java_param_type(p, ent['name'])} {camel(p.name)}"
                    for p in kept)
    ret = java_return(m, ent, comps)
    L: list[str] = []
    if drop:
        supplied = ", ".join(
            f"{camel(p.name)}={opt[p.name]!r}" for p in ins[len(ins) - drop:])
        L += [f"    /** {name} with the manifest defaults ({supplied}). */"]
    else:
        L += _javadoc(m, ent, comps)
    L.append(f"    public {ret} {name}({sig}) {{")
    if drop:
        call = [camel(p.name) for p in kept]
        for p in ins[len(ins) - drop:]:
            call.append(java_literal(java_param_type(p, ent["name"]),
                                     opt[p.name]))
        stmt = f"{name}({', '.join(call)})"
        L.append(f"        {'' if ret == 'void' else 'return '}{stmt};")
        L += ["    }", ""]
        return L

    nat = native_name(ent["name"])
    call_args = ["handle"]
    for p in kept:
        if ent.get("index_base") and p is ins[0]:
            call_args.append(f"{camel(p.name)} - {ent['index_base']}")
        else:
            call_args.append(camel(p.name))

    if ent.get("index_base"):
        absent = "" if ret == "void" else " null"
        L.append(f"        if ({camel(ins[0].name)} < {ent['index_base']})")
        L.append(f"            return{absent};")

    if shape == "void_call":
        L.append(f"        {nat}({', '.join(call_args)});")
    elif shape == "value_return":
        if ret == "String":
            # The Lua binding pushes "" for a NULL host string; matching it
            # here is what keeps `get_locale` the same entry in both languages.
            L.append(f"        String v = {nat}({', '.join(call_args)});")
            L.append('        return v == null ? "" : v;')
        else:
            L.append(f"        return {nat}({', '.join(call_args)});")
    elif shape == "owned_string_release":
        L.append(f"        return {nat}({', '.join(call_args)});")
    elif shape == "void_out_array":
        c = _value_components(comps)[0]
        L.append(f"        {c['elem']}[] out = new {c['elem']}[{c['count']}];")
        L.append(f"        {nat}({', '.join(call_args + ['out'])});")
        L.append("        return out;")
    elif shape == "entity_table":
        cap = ent["out_capacity"]
        L.append(f"        long[] buf = new long[{cap}];")
        L.append(f"        int n = {nat}({', '.join(call_args + ['buf'])});")
        L.append("        if (n <= 0) return EMPTY_ENTITIES;")
        L.append(f"        if (n > {cap}) n = {cap};")
        L.append("        return java.util.Arrays.copyOf(buf, n);")
    elif shape == "first_and_count":
        cap = ent["out_capacity"]
        rt = result_type(ent["name"])
        L.append(f"        long[] buf = new long[{cap}];")
        L.append(f"        int n = {nat}({', '.join(call_args + ['buf'])});")
        L.append(f"        return new {rt}(n > 0 ? Long.valueOf(buf[0]) : null,"
                 f" n);")
    elif shape == "fallible_out":
        bl = buckets_of(comps)
        names = []
        for t, slots in bl:
            v = "out" + t[:1].upper() + t[1:]
            names.append(v)
            L.append(f"        {t}[] {v} = new {t}[{len(slots)}];")
        L.append(f"        if (!{nat}({', '.join(call_args + names)}))")
        L.append("            return null;")
        idx = bucket_index(comps)
        vc = _value_components(comps)
        if len(vc) == 1:
            c = vc[0]
            t, i0 = idx[c["c_name"]][0]
            v = "out" + t[:1].upper() + t[1:]
            if c["is_array"]:
                L.append(f"        return {v};")
            else:
                L.append(f"        return {JAVA_BOX[t]}.valueOf({v}[{i0}]);")
        else:
            parts = []
            for c in vc:
                if c["is_array"]:
                    t = c["elem"]
                    v = "out" + t[:1].upper() + t[1:]
                    lo = idx[c["c_name"]][0][1]
                    hi = idx[c["c_name"]][-1][1] + 1
                    parts.append(
                        f"java.util.Arrays.copyOfRange({v}, {lo}, {hi})")
                else:
                    t, i0 = idx[c["c_name"]][0]
                    v = "out" + t[:1].upper() + t[1:]
                    parts.append(f"{v}[{i0}]")
            L.append(f"        return new {result_type(ent['name'])}"
                     f"({', '.join(parts)});")
    L += ["    }", ""]
    return L


def emit_java_surface(members: list[HostMember], man: dict) -> str:
    by = {m.name: m for m in members}
    header_text = HEADER.read_text(encoding="utf-8")
    out = [_JAVA_BANNER.replace("@EXCLUDED@", _excluded_block(man))]
    A = out.append
    A(f"""\
/** The scripting surface: {len(man['expose'])} entries over the C ABI. */
public final class {JAVA_CLASS} implements AutoCloseable {{

    /** The manifest's script_api_version this class was generated from. */
    public static final int SCRIPT_API_MIN = {man['script_api_version']};

    /** Shared empty result, so an empty query allocates nothing. */
    private static final long[] EMPTY_ENTITIES = new long[0];

    /* The native library carrying the JNI shim. Set -D{'jce.script.library'}
     * to an absolute path to load a specific build; otherwise the platform
     * library path is searched for "{JAVA_LIB_NAME}".
     *
     * THE VERSION HANDSHAKE IS MANDATORY, the same rule engine/java's
     * JceRuntime follows -- and it is the SCRIPTING surface's number
     * (script_api_version), not the engine C ABI's jce_api_version(), which
     * this class never calls. A newer binding on an older library cannot
     * degrade: it would call entries that do not exist. Refusing at class
     * load names both numbers; open() refuses again in C, and the two compare
     * the same pair so they cannot disagree. */
    static {{
        String explicit = System.getProperty("jce.script.library");
        if (explicit != null && !explicit.isEmpty()) {{
            System.load(explicit);
        }} else {{
            System.loadLibrary("{JAVA_LIB_NAME}");
        }}
        int have = nativeApiVersion();
        if (have < SCRIPT_API_MIN) {{
            throw new IllegalStateException(
                "{JAVA_LIB_NAME} implements script_api_version " + have
                + " but " + {JAVA_CLASS}.class.getName()
                + " was generated from " + SCRIPT_API_MIN
                + " -- a newer binding cannot run against an older library");
        }}
    }}

    private long handle;

    private {JAVA_CLASS}(long handle) {{
        this.handle = handle;
    }}

    /** The script_api_version the loaded library implements. */
    public static int libraryApiVersion() {{
        return nativeApiVersion();
    }}

    /**
     * Binds to a host the engine already built.
     *
     * @param hostPointer address of a JceScriptHost the caller keeps alive
     *                    for the call (the library copies it)
     * @param hostSize    the CALLER's sizeof(JceScriptHost), always from
     *                    sizeof and never summed
     * @return a handle, or null when the library is older than
     *         SCRIPT_API_MIN or the arguments are unusable
     */
    public static {JAVA_CLASS} open(long hostPointer, long hostSize) {{
        long h = nativeOpen(hostPointer, hostSize, SCRIPT_API_MIN);
        return h == 0L ? null : new {JAVA_CLASS}(h);
    }}

    /** Releases the handle. Idempotent. */
    @Override
    public void close() {{
        long h = handle;
        handle = 0L;
        if (h != 0L) {{
            nativeClose(h);
        }}
    }}

    /** The raw JceScriptApi* -- for a native embedder, not for scripts. */
    public long nativeHandle() {{
        return handle;
    }}

    private static native int nativeApiVersion();
    private static native long nativeOpen(long hostPointer, long hostSize,
                                          int scriptApiMin);
    private static native void nativeClose(long api);
""")

    records: list[str] = []
    for e in man["expose"]:
        m = by[e["vtable"]]
        comps = out_components(m, e, header_text)
        if needs_record(m, e, comps):
            records += _record_decl(m, e, comps)
    if records:
        A("    /* ---- Result types, one per entry whose answer has more than"
          "\n     * one component. An entry with a single component yields that"
          "\n     * component directly. ---- */\n")
        A("\n".join(records))

    for e in man["expose"]:
        m = by[e["vtable"]]
        comps = out_components(m, e, header_text)
        A("\n".join(_java_method(m, e, comps, 0)))
        for k in range(1, optional_tail(m, e) + 1):
            A("\n".join(_java_method(m, e, comps, k)))
        A("\n".join([_native_decl(m, e, comps), ""]))

    A("}\n")
    return "\n".join(out)


# ═════════════════════════════════════════════════════════════════════════════
#  Artefact 2 — the JNI shim
# ═════════════════════════════════════════════════════════════════════════════

_JNI_BANNER = f'''\
/* jce_script_jni.gen.c -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * The JNI half of {JAVA_PKG}.{JAVA_CLASS}. One JNIEXPORT per native method,
 * forwarding to the matching jce_script_api_* export. It links the C ABI
 * shared library and NOTHING else -- no engine layer, no Lua.
 *
 * LOCAL REFERENCES. Every native below creates AT MOST ONE object (a jstring)
 * and returns it immediately; out values cross in arrays the CALLER allocated,
 * so nothing is allocated per element and nothing is allocated in a loop.
 *
 * That is checked, not asserted -- but NOT by -Xcheck:jni, which this banner
 * used to credit. Measured: a mutated shim leaking 200,000 local references
 * inside one native frame runs the whole differential GREEN under
 * `java -Xcheck:jni`; that checker reports reference MISUSE, not accumulation.
 * The discipline is enforced statically over this emitted file by
 * test_script_java_gate.py --
 * test_no_native_creates_more_than_one_object,
 * test_no_native_creates_a_reference_that_outlives_the_call and
 * test_the_shim_contains_no_loop (the count is only static while nothing
 * iterates). Each was applied as a mutation and observed red.
 *
 * STRING ARGUMENTS. GetStringUTFChars is paired with ReleaseStringUTFChars on
 * a SINGLE return path -- every function here has exactly one `return`, so a
 * release cannot be skipped by an early exit. When the JVM cannot allocate the
 * chars it returns NULL with an exception pending; the call is then skipped
 * (`ok`) rather than made with a NULL argument the host would have to
 * interpret.
 *
 * @COUNTS@
 */

#include <jni.h>

#include <jce/script_api/jce_script_api.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The entity id crosses as a jlong and an entity buffer is handed straight to
 * SetLongArrayRegion. Both are only true while these are the same width. */
typedef char jce_java_entity_fits_jlong[
    (sizeof(JceScriptEntity) == sizeof(jlong)) ? 1 : -1];

static JceScriptApi *jce_java_api(jlong h)
{{
    return (JceScriptApi *)(intptr_t)h;
}}
'''


def _jni_sig(ret_jni: str, fn: str, params: list[str]) -> str:
    head = f"JNIEXPORT {ret_jni} JNICALL {fn}("
    one = head + ", ".join(params) + ")"
    if len(one) <= 88:
        return one
    pad = " " * 4
    lines = [head]
    cur = pad + params[0]
    for p in params[1:]:
        if len(cur) + 2 + len(p) > 88:
            lines.append(cur + ",")
            cur = pad + p
        else:
            cur += ", " + p
    lines.append(cur + ")")
    return "\n".join(lines)


def _emit_jni_one(m: HostMember, ent: dict, comps: list[Component]) -> str:
    shape = ent["shape"]
    fn = JNI_PREFIX + native_name(ent["name"])
    ins = java_in_params(m, ent)
    params = ["JNIEnv *env", "jclass cls", "jlong api"]
    decls: list[str] = []
    pre: list[str] = []
    post: list[str] = []
    call_args = ["jce_java_api(api)"]
    strings: list[tuple[str, str]] = []       # (jstring param, char* local)
    uses_env = False

    for p in ins:
        n = p.name
        if p.arity > 0:                       # `const T name[N]` input
            uses_env = True
            elem = _elem_c_type(p.c_type)
            jt = _java_scalar(elem, ent["name"])
            params.append(f"{JNI_ARRAY[jt]} j_{n}")
            decls.append(f"    {elem} {n}[{p.arity}];")
            pre.append(f"    memset({n}, 0, sizeof {n});")
            pre.append(f"    if (j_{n})")
            pre.append(f"        (*env)->Get{JNI_REGION[jt]}ArrayRegion("
                       f"env, j_{n}, 0, {p.arity}, ({JNI_TYPE[jt]} *){n});")
            call_args.append(n)
        elif p.c_type == "const char *":
            uses_env = True
            params.append(f"jstring j_{n}")
            decls.append(f"    const char *{n} = NULL;")
            strings.append((f"j_{n}", n))
            call_args.append(n)
        else:
            jt = _java_scalar(p.c_type, ent["name"])
            params.append(f"{JNI_TYPE[jt]} {n}")
            call_args.append(f"({p.c_type}){n}")

    # From the SECOND string argument onward the read is guarded on `ok` too.
    # A failed GetStringUTFChars leaves an OutOfMemoryError PENDING, and the
    # JNI spec allows only a short list of calls with an exception pending --
    # ReleaseStringUTFChars is on it, a second GetStringUTFChars is not, and
    # -Xcheck:jni reports that as "JNI call made without checking exceptions".
    # The first read cannot be guarded on a flag that is still 1, and emitting
    # `if (ok && ...)` there would be a condition that guards nothing, in 71
    # functions.
    #
    # NO TEST REACHES THIS. The differential would have to make the JVM fail to
    # allocate a UTF-8 copy of a short string, which nothing here can force;
    # the reason is written down instead of being left as an immune mutation
    # with a silent one. What IS tested is the pairing and the single exit:
    # test_every_GetStringUTFChars_is_released_on_the_one_return_path.
    for k, (jparam, local) in enumerate(strings):
        cond = jparam if k == 0 else f"ok && {jparam}"
        pre.append(f"    if ({cond}) {{")
        pre.append(f"        {local} = (*env)->GetStringUTFChars("
                   f"env, {jparam}, NULL);")
        pre.append(f"        if (!{local})")
        pre.append("            ok = 0;")
        pre.append("    }")
    for jparam, local in reversed(strings):
        post.append(f"    if ({local})")
        post.append(f"        (*env)->ReleaseStringUTFChars("
                    f"env, {jparam}, {local});")

    ret_jni = "void"
    ret_decl: list[str] = []
    ret_stmt = ""

    # `ok` exists only when a string argument can fail to marshal.  Emitting it
    # unconditionally would wrap every call in `if (ok)` with ok never
    # assigned -- a constant condition that reads like a guard and guards
    # nothing, in 71 functions.
    has_ok = bool(strings)

    def guarded(extra, lines):
        conds = (["ok"] if has_ok else []) + ([extra] if extra else [])
        if not conds:
            return ["    " + ln for ln in lines]
        return (["    if (" + " && ".join(conds) + ") {"]
                + ["        " + ln for ln in lines] + ["    }"])

    if shape in ("fallible_out", "void_out_array"):
        uses_env = True
        for c in comps:
            if c["c_decl"]:
                decls.append(f"    {c['c_decl']};")
                pre.append(f"    {c['c_init']}")
        for t, slots in buckets_of(comps):
            v = "out" + t[:1].upper() + t[1:]
            params.append(f"{JNI_ARRAY[t]} j_{v}")
            decls.append(f"    {JNI_TYPE[t]} {v}[{len(slots)}];")
        for c in comps:
            if c["c_pass"]:
                call_args.append(c["c_pass"])

    def write_back():
        out = []
        for t, slots in buckets_of(comps):
            v = "out" + t[:1].upper() + t[1:]
            for i, expr in enumerate(slots):
                out.append(f"{v}[{i}] = ({JNI_TYPE[t]}){expr};")
            out.append(f"if (j_{v})")
            out.append(f"    (*env)->Set{JNI_REGION[t]}ArrayRegion("
                       f"env, j_{v}, 0, {len(slots)}, {v});")
        return out

    call = f"jce_script_api_{ent['name']}({', '.join(call_args)})"

    if shape == "void_call":
        body = guarded(None, [call + ";"])
    elif shape == "value_return":
        if m.ret == "const char *":
            uses_env = True
            ret_jni = "jstring"
            ret_decl.append("    jstring ret = NULL;")
            ret_decl.append("    const char *v = NULL;")
            # NewStringUTF BEFORE the releases below: `tr` answers with the
            # caller's own key pointer when the host member is absent, and
            # that pointer is the GetStringUTFChars buffer, released after.
            body = guarded(None, [f"v = {call};",
                                  "if (v)",
                                  "    ret = (*env)->NewStringUTF(env, v);"])
            ret_stmt = "    return ret;"
        else:
            jt = _java_scalar(m.ret, ent["name"])
            ret_jni = JNI_TYPE[jt]
            ret_decl.append(f"    {JNI_TYPE[jt]} ret = ({JNI_TYPE[jt]})0;")
            body = guarded(None, [f"ret = ({JNI_TYPE[jt]}){call};"])
            ret_stmt = "    return ret;"
    elif shape == "void_out_array":
        body = guarded(None, [call + ";"] + write_back())
    elif shape == "fallible_out":
        ret_jni = "jboolean"
        ret_decl.append("    jboolean ret = JNI_FALSE;")
        body = guarded(call, ["ret = JNI_TRUE;"] + write_back())
        ret_stmt = "    return ret;"
    elif shape in ("first_and_count", "entity_table"):
        uses_env = True
        cap = ent["out_capacity"]
        params.append("jlongArray j_out")
        decls.append(f"    jlong found[{cap}];")
        decls.append("    int n;")
        ret_jni = "jint"
        ret_decl.append("    jint ret = 0;")
        pre.append("    memset(found, 0, sizeof found);")
        call_args += ["(JceScriptEntity *)found", str(cap)]
        call = f"jce_script_api_{ent['name']}({', '.join(call_args)})"
        body = guarded(None, [
            f"n = {call};",
            f"if (n > {cap})",
            f"    n = {cap};",
            "if (n > 0 && j_out)",
            "    (*env)->SetLongArrayRegion(env, j_out, 0, n, found);",
            "ret = (jint)(n > 0 ? n : 0);"])
        ret_stmt = "    return ret;"
    elif shape == "owned_string_release":
        uses_env = True
        ret_jni = "jstring"
        ret_decl.append("    jstring ret = NULL;")
        ret_decl.append("    char stackbuf[1024];")
        ret_decl.append("    char *heap = NULL;")
        ret_decl.append("    char *buf = stackbuf;")
        ret_decl.append("    int cap = (int)sizeof stackbuf;")
        ret_decl.append("    int n;")
        one = f"jce_script_api_{ent['name']}({', '.join(call_args + ['buf', 'cap'])});"
        # The C ABI releases the host's string INSIDE the call, so a truncated
        # answer cannot be re-read from that one; asking again is a SECOND host
        # call.  That is the only place the Java host-call trace can differ
        # from Lua's, and the differential says so rather than hiding it.
        body = guarded(None, [
            "stackbuf[0] = " + chr(39) + chr(92) + "0" + chr(39) + ";",
            f"n = {one}",
            "if (n >= cap) {",
            "    heap = (char *)malloc((size_t)n + 1u);",
            "    if (heap) {",
            "        buf = heap;",
            "        cap = n + 1;",
            f"        n = {one}",
            "    }",
            "}",
            "if (n >= 0)",
            "    ret = (*env)->NewStringUTF(env, buf);",
            "free(heap);"])
        ret_stmt = "    return ret;"

    L = [f"/* jce.{ent['name']} -> jce_script_api_{ent['name']}"
         f" ({ent['shape']}) */",
         _jni_sig(ret_jni, fn, params), "{"]
    L += ret_decl
    L += decls
    if has_ok:
        L.append("    int ok = 1;")
    L.append("")
    if not uses_env:
        L.append("    (void)env;")
    L.append("    (void)cls;")
    L += pre
    L += body
    L += post
    if ret_stmt:
        L.append(ret_stmt)
    L.append("}")
    return "\n".join(L) + "\n"


def emit_jni_c(members: list[HostMember], man: dict) -> str:
    by = {m.name: m for m in members}
    header_text = HEADER.read_text(encoding="utf-8")
    counts = (f"{len(man['expose'])} entry points plus 3 meta "
              f"(nativeApiVersion / nativeOpen / nativeClose).")
    out = [_JNI_BANNER.replace("@COUNTS@", counts)]
    out.append(f"""
JNIEXPORT jint JNICALL {JNI_PREFIX}nativeApiVersion(JNIEnv *env, jclass cls)
{{
    (void)env;
    (void)cls;
    return (jint)jce_script_api_version();
}}

JNIEXPORT jlong JNICALL {JNI_PREFIX}nativeOpen(JNIEnv *env, jclass cls,
    jlong hostPointer, jlong hostSize, jint scriptApiMin)
{{
    const JceScriptHost *host = (const JceScriptHost *)(intptr_t)hostPointer;
    JceScriptApi *api;

    (void)env;
    (void)cls;
    api = jce_script_api_open(host, (size_t)hostSize, (uint32_t)scriptApiMin);
    return (jlong)(intptr_t)api;
}}

JNIEXPORT void JNICALL {JNI_PREFIX}nativeClose(JNIEnv *env, jclass cls,
    jlong api)
{{
    (void)env;
    (void)cls;
    jce_script_api_close(jce_java_api(api));
}}
""")
    for e in man["expose"]:
        m = by[e["vtable"]]
        out.append("")
        out.append(_emit_jni_one(m, e, out_components(m, e, header_text)))
    return "\n".join(out)


# ═════════════════════════════════════════════════════════════════════════════
#  The differential — cases, derived from the manifest and from nothing else
# ═════════════════════════════════════════════════════════════════════════════

def _seed(name: str) -> int:
    h = 0
    for ch in name:
        h = (h * 131 + ord(ch)) & 0xFFFFFFFF
    return h


def _mock_value(c_type: str, k: int, seed: int) -> str:
    """One canned out-value as a C literal.

    Floats end in .5 so a value that loses precision between the host and
    either surface shows up as a difference rather than passing; the
    fractional part is a negative power of two, so float and double agree bit
    for bit and the bit-pattern canonicalisation is exact."""
    base = seed % 41 + k
    if c_type == "float":
        return f"{base}.5f"
    if c_type == "double":
        return f"{base}.5"
    if c_type == "bool":
        return "true" if (seed >> (k % 24)) & 1 else "false"
    if c_type in ("int", "uint32_t", "uint64_t", "JceScriptEntity"):
        return str(base)
    raise SystemExit(
        f"error: emit_java: the differential mock has no canned value for C "
        f"type {c_type!r} -- give the member a hand-written mock rather than "
        f"inventing a value here.")


def synth_args(m: HostMember, ent: dict, mode: str) -> list[tuple[Param, object]]:
    """(parameter, python value) for one case, seeded by the entry name.

    Both drivers render THIS list -- one into Lua literals, one into Java
    literals -- so the two processes call the host with the same numbers by
    construction rather than by two independent choices that happen to agree.

    mode:
      base        real values everywhere
      defaults    every optional parameter takes its manifest default; the
                  Lua driver spells that `nil` and the Java driver reaches it
                  through the shortest overload
      below_base  the index_base parameter is one below its base, which is the
                  early return that never calls the host
    """
    opt = ent.get("optional") or {}
    s = _seed(ent["name"])
    out: list[tuple[Param, object]] = []
    ins = java_in_params(m, ent)
    for i, p in enumerate(ins):
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        if mode == "defaults" and p.name in opt:
            out.append((p, opt[p.name]))
            continue
        if mode == "below_base" and ent.get("index_base") and i == 0:
            out.append((p, ent["index_base"] - 1))
            continue
        if p.arity > 0:
            out.append((p, [((s >> (8 + 3 * j)) % 20000) / 8.0
                            for j in range(p.arity)]))
        elif p.c_type == "const char *":
            out.append((p, f"{ent['name']}_{p.name}"))
        elif p.c_type == "bool":
            out.append((p, bool((s >> 16) & 1)))
        elif p.c_type in ("float", "double"):
            out.append((p, ((s >> 8) % 20000) / 8.0))
        elif ent.get("index_base") and i == 0:
            out.append((p, ent["index_base"]))
        else:
            out.append((p, 1 + (s >> 12) % 97))
    return out


# host_mode: "full" every member mocked; "partial" only `log` and `user`, so
# every one of the 71 takes its absent path -- which is where absent_value,
# clamp_min and the host guard live and where the base cases never go.
Case = dict


def harness_cases(members: list[HostMember], man: dict) -> list[Case]:
    by = {m.name: m for m in members}
    cases: list[Case] = []

    def add(ent, label, mode, host_mode, skip=None):
        cases.append({"entry": ent["name"], "label": label, "mode": mode,
                      "host": host_mode, "skip": skip})

    for e in man["expose"]:
        add(e, e["name"], "base", "full")
        if e.get("optional"):
            add(e, e["name"] + " [defaults]", "defaults", "full")
        if e.get("index_base"):
            add(e, e["name"] + " [index below base]", "below_base", "full")
        for sp in (e.get("strict") or []):
            # The Lua binding calls luaL_checktype on this parameter and
            # raises on a non-boolean. Java's parameter IS boolean, so the
            # equivalent call does not compile -- strictly stronger, and
            # nothing to run. Recorded as a case rather than dropped, so the
            # modifier is visible in the compared stream instead of being
            # silently absent from it.
            add(e, f"{e['name']} [strict {sp}]", "base", "full",
                skip=f"strict-{sp}-is-a-compile-error-in-java")
    for e in man["expose"]:
        add(e, e["name"] + " [no host]", "base", "partial")

    for c in cases:
        m = by[next(x["vtable"] for x in man["expose"]
                    if x["name"] == c["entry"])]
        c["_m"] = m
    return cases


# ── Mock host ────────────────────────────────────────────────────────────────

_HAND_MOCKS = {
    "log": """\
/* NOT traced: this is the wire the Lua probe string comes back on, and it is
 * called on both host modes. Tracing it would put a line in every Lua case
 * that the Java side, which never calls log, could not produce. */
static void mk_log(void *user, const char *msg)
{
    size_t n;
    (void)user;
    n = strlen(g_jce_java_diff_out);
    snprintf(g_jce_java_diff_out + n, sizeof g_jce_java_diff_out - n,
             "%s\\n", msg ? msg : "(null)");
}
""",
    "read_file": """\
/* Reached only by the hand-written asset_read_* bindings, which are not on
 * the Java surface at all. It exists so the `full` host really is full: a
 * NULL here would make the Lua VM take a different path from the one the
 * generated bindings see. */
static void *mk_read_file(void *user, const char *path, uint64_t *out_size)
{
    static const char kBody[] = "[1,2,3]";
    size_t n = sizeof kBody - 1u;
    char *buf;

    (void)user;
    (void)path;
    buf = (char *)jce_malloc(n);
    if (!buf) {
        if (out_size) *out_size = 0u;
        return NULL;
    }
    memcpy(buf, kBody, n);
    if (out_size) *out_size = (uint64_t)n;
    return buf;
}
""",
    "json_free": """\
/* Traced BY CONTENT. The pointer differs between the two processes and means
 * nothing; "was the host's string released at all" is exactly what the
 * owned_string_release shape has to get right, and it is the only evidence
 * either side releases. free(), matching the malloc in mk_strdup. */
static void mk_json_free(void *user, char *s)
{
    (void)user;
    jce_java_diff_tracef("json_free|%s\\n", s ? s : "(null)");
    free(s);
}
""",
    "touch_count": """\
/* NEGATIVE on purpose. clamp_min exists because a host may answer with a
 * negative count, and a mock that never returns one leaves the modifier
 * unexercised: both surfaces would agree on 12 whether or not either clamped.
 * With -3 they agree on 0 only if BOTH clamp. */
static int mk_touch_count(void *user)
{
    (void)user;
    jce_java_diff_tracef("touch_count\\n");
    return -3;
}
""",
}


def _mock_param(p: Param) -> str:
    if p.arity > 0:
        return f"{p.c_type} {p.name}[{p.arity}]"
    sep = "" if p.c_type.endswith("*") else " "
    if p.arity < 0:
        return f"{p.c_type}{sep}*{p.name}"
    return f"{p.c_type}{sep}{p.name}"


def _mock_member(m: HostMember, header_text: str) -> str:
    from scriptgen_core import in_params           # local: neutral helper
    seed = _seed(m.name)
    ps = ", ".join(_mock_param(p) for p in m.params)
    body = ["    (void)user;",
            f'    jce_java_diff_tracef("{m.name}");']
    for p in in_params(m):
        if p.arity > 0:
            body += [f'    jce_java_diff_tracef("|%g", '
                     f'{p.name} ? (double){p.name}[{i}] : 0.0);'
                     for i in range(p.arity)]
        elif p.c_type == "const char *":
            body.append(f'    jce_java_diff_tracef("|%s", '
                        f'{p.name} ? {p.name} : "(null)");')
        elif p.c_type == "bool":
            body.append(f'    jce_java_diff_tracef("|%d", {p.name} ? 1 : 0);')
        elif p.c_type in ("float", "double"):
            body.append(f'    jce_java_diff_tracef("|%g", (double){p.name});')
        else:
            body.append(f'    jce_java_diff_tracef("|%lld", '
                        f'(long long){p.name});')
    body.append('    jce_java_diff_tracef("\\n");')
    if not m.name.startswith("find_by"):
        k = 0
        for p in out_params(m):
            if p.arity > 0:
                for i in range(p.arity):
                    k += 1
                    body.append(f"    {p.name}[{i}] = "
                                f"{_mock_value(p.c_type, k, seed)};")
            elif p.c_type in JAVA_SCALAR:
                k += 1
                body.append(f"    *{p.name} = "
                            f"{_mock_value(p.c_type, k, seed)};")
            else:
                body.append(f"    memset({p.name}, 0, sizeof *{p.name});")
                for j, (expr, ct) in enumerate(flatten_pod(header_text,
                                                           p.c_type)):
                    body.append(f"    {p.name}->{expr} = "
                                f"{_mock_value(ct, j + 1, seed)};")
    if m.ret != "void":
        if m.ret == "const char *":
            body.append(f'    return "{m.name}_ret";')
        elif m.ret == "char *":
            body.append(f'    return mk_strdup("{m.name}_json");')
        elif m.ret == "bool":
            body.append("    return true;")
        elif m.ret == "int" and m.name.startswith("find_by"):
            body += ["    for (int i = 0; i < 3 && i < max; i++)",
                     "        out[i] = (JceScriptEntity)(100 + i);",
                     "    return max < 3 ? max : 3;"]
        else:
            body.append(f"    return {_mock_value(m.ret, 1, seed)};")
    return f"static {m.ret} mk_{m.name}({ps})\n{{\n" + "\n".join(body) + "\n}\n"


_DIFF_HOST_H = """\
/* jce_java_diff_host.gen.h -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * The recording mock host, shared by BOTH sides of the cross-language
 * differential: the Lua reference driver links it and so does the JNI helper
 * the Java driver loads. ONE implementation, compiled twice -- so a trace
 * difference between the two processes is a difference in what the BINDINGS
 * did, never a difference in what the mock did.
 */
#ifndef JCE_JAVA_DIFF_HOST_GEN_H
#define JCE_JAVA_DIFF_HOST_GEN_H

#include <jce/middleware/script/jce_script.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Every member mocked; every call traced. */
const JceScriptHost *jce_java_diff_host_full(void);

/* Only `log` and `user`. Every one of the generated entries takes its absent
 * path, which is where absent_value, clamp_min and the host guard live. */
const JceScriptHost *jce_java_diff_host_partial(void);

size_t jce_java_diff_host_size(void);

/* The host-call trace since the last reset, and the Lua probe output. */
const char *jce_java_diff_trace(void);
const char *jce_java_diff_out(void);
void        jce_java_diff_reset(void);
void        jce_java_diff_tracef(const char *fmt, ...);

/* The case table, in ONE order both drivers walk. */
int         jce_java_diff_case_count(void);
const char *jce_java_diff_case_label(int i);

#ifdef __cplusplus
}
#endif

#endif /* JCE_JAVA_DIFF_HOST_GEN_H */
"""


def emit_diff_host_h(members: list[HostMember], man: dict) -> str:
    return _DIFF_HOST_H


def emit_diff_host_c(members: list[HostMember], man: dict) -> str:
    header_text = HEADER.read_text(encoding="utf-8")
    cases = harness_cases(members, man)
    L = ['''\
/* jce_java_diff_host.gen.c -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * See jce_java_diff_host.gen.h. Return values are canned from each member's
 * own NAME, so both processes see identical answers and any difference in the
 * compared stream is the binding's fault and not the mock's.
 *
 * INPUTS are traced; out parameters are not. An out array is uninitialised on
 * entry on the Lua side, and tracing it would hash uninitialised stack -- a
 * harness that fails for a reason that is not a defect gets muted, which is
 * worse than one that passes.
 */

#include "jce_java_diff_host.gen.h"

#include <jce/os/core/jce_alloc.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_jce_java_diff_trace[65536];
static int  g_jce_java_diff_len;
static char g_jce_java_diff_out[16384];

void jce_java_diff_tracef(const char *fmt, ...)
{
    va_list ap;
    int n;

    if (g_jce_java_diff_len < 0 ||
        g_jce_java_diff_len >= (int)sizeof g_jce_java_diff_trace - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(g_jce_java_diff_trace + g_jce_java_diff_len,
                  sizeof g_jce_java_diff_trace -
                      (size_t)g_jce_java_diff_len, fmt, ap);
    va_end(ap);
    if (n > 0 && g_jce_java_diff_len + n < (int)sizeof g_jce_java_diff_trace)
        g_jce_java_diff_len += n;
}

const char *jce_java_diff_trace(void) { return g_jce_java_diff_trace; }
const char *jce_java_diff_out(void)   { return g_jce_java_diff_out; }

void jce_java_diff_reset(void)
{
    g_jce_java_diff_trace[0] = \'\\0\';
    g_jce_java_diff_len = 0;
    g_jce_java_diff_out[0] = \'\\0\';
}

static char *mk_strdup(const char *s)
{
    size_t n = strlen(s) + 1u;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
''']
    by = {m.name: m for m in members}
    for m in members:
        L.append(_HAND_MOCKS.get(m.name) or _mock_member(m, header_text))
    L.append("static JceScriptHost g_full;\nstatic JceScriptHost g_partial;\n"
             "static int g_ready;\n")
    L.append("static void build(void)\n{\n"
             "    if (g_ready) return;\n"
             "    g_ready = 1;\n"
             "    memset(&g_full, 0, sizeof g_full);\n"
             "    memset(&g_partial, 0, sizeof g_partial);\n"
             "    g_full.user = (void *)&g_ready;\n"
             "    g_partial.user = (void *)&g_ready;\n"
             "    g_partial.log = mk_log;\n"
             + "".join(f"    g_full.{m.name} = mk_{m.name};\n" for m in members)
             + "}\n")
    L.append("""\
const JceScriptHost *jce_java_diff_host_full(void)
{
    build();
    return &g_full;
}

const JceScriptHost *jce_java_diff_host_partial(void)
{
    build();
    return &g_partial;
}

size_t jce_java_diff_host_size(void)
{
    return sizeof(JceScriptHost);
}
""")
    L.append("/* The case table. Both drivers walk THIS order. */\n"
             "static const char *const g_case_labels[] = {\n"
             + "".join(f'    "{c["label"]}",\n' for c in cases)
             + "};\n\n"
             "int jce_java_diff_case_count(void)\n{\n"
             "    return (int)(sizeof g_case_labels / "
             "sizeof g_case_labels[0]);\n}\n\n"
             "const char *jce_java_diff_case_label(int i)\n{\n"
             "    if (i < 0 || i >= jce_java_diff_case_count())\n"
             '        return "";\n'
             "    return g_case_labels[i];\n}\n")
    _ = by
    return "\n".join(L)


# ── The Lua reference driver ─────────────────────────────────────────────────

_LUA_PRELUDE = r"""local function num(v)
  if math.type(v) == 'integer' then return 'i:' .. string.format('%d', v) end
  return 'f:' .. string.format('%d', (string.unpack('<i8', string.pack('<d', v))))
end
local function canon(v)
  local t = type(v)
  if t == 'nil' then return 'nil' end
  if t == 'boolean' then return 'b:' .. tostring(v) end
  if t == 'number' then return num(v) end
  if t == 'string' then return 's:' .. v end
  if t == 'table' then
    local p = {}
    for i = 1, #v do p[i] = num(v[i]) end
    return 't:[' .. table.concat(p, ',') .. ']'
  end
  return 'other:' .. t
end
local function probe(...)
  local r = table.pack(...)
  local p = {}
  for i = 1, r.n do p[i] = canon(r[i]) end
  return r.n .. '|' .. table.concat(p, '|')
end
"""


def lua_call(m: HostMember, ent: dict, mode: str) -> str:
    args = []
    for p, v in synth_args(m, ent, mode):
        jt = java_param_type(p, ent["name"])
        if p.arity > 0:
            args += [lua_literal("float", x) for x in v]
        elif mode == "defaults" and p.name in (ent.get("optional") or {}):
            args.append("nil")            # what luaL_opt* / isnoneornil read
        else:
            args.append(lua_literal(jt.replace("[]", ""), v))
    return f"jce.{ent['name']}({', '.join(args)})"


def emit_diff_lua_reference(members: list[HostMember], man: dict) -> str:
    by = {m.name: m for m in members}
    ent_by = {e["name"]: e for e in man["expose"]}
    cases = harness_cases(members, man)
    lines = []
    for c in cases:
        e = ent_by[c["entry"]]
        m = by[e["vtable"]]
        if c["skip"]:
            lines.append((c["label"], c["host"], None, c["skip"]))
        else:
            lines.append((c["label"], c["host"],
                          lua_call(m, e, c["mode"]), None))
    for label, host, call, skip in lines:
        if call and ('"' in call or "\\" in call):
            raise SystemExit(
                f"error: emit_java: differential case {label!r} contains a "
                f"character that cannot cross into a C string literal: {call!r}")

    body = ['''\
/* jce_java_diff_lua_reference.gen.c -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * THE REFERENCE SIDE of the cross-language differential. Batch 1 proved the
 * generated Lua bindings equivalent to the hand-written originals over 87
 * cases and then deleted the originals, so Lua -- not the manifest, and not
 * this backend's own emitter -- is what a new language is measured against.
 *
 * It prints a canonical stream to stdout: per case, the full result (arity,
 * then TYPE and VALUE per slot, so nil / 0 / false cannot pass for each
 * other) and the host-call trace. The Java driver prints the same stream from
 * the same mock host; run_differential.py compares them byte for byte.
 *
 * Numbers are canonicalised as IEEE-754 BIT PATTERNS, not as formatted
 * decimals. C\'s %g strips trailing zeros and Java\'s does not, so -1.0 is "-1"
 * on one side and "-1.0" on the other -- a difference that is about printf
 * and about nothing else. The bits are exact in both languages and the
 * comparator decodes them when it reports a diff.
 */

#include "jce_java_diff_host.gen.h"

#include <jce/middleware/script/jce_script.h>

#include <stdio.h>
#include <string.h>

static const char kPrelude[] =
''']
    for ln in _LUA_PRELUDE.splitlines():
        body.append('    "' + ln.replace("\\", "\\\\").replace('"', '\\"')
                    + '\\n"')
    body.append("    ;\n")
    body.append("static const char *const g_calls[] = {")
    for label, host, call, skip in lines:
        body.append(f'    {("NULL" if call is None else chr(34) + call + chr(34))},')
    body.append("};\n")
    body.append("static const char *const g_skips[] = {")
    for label, host, call, skip in lines:
        body.append(f'    {("NULL" if skip is None else chr(34) + skip + chr(34))},')
    body.append("};\n")
    body.append("static const int g_partial[] = {")
    body.append("    " + ", ".join("1" if h == "partial" else "0"
                                   for _, h, _, _ in lines) + ",")
    body.append("};\n")
    body.append(r'''
static void print_trace(FILE *f)
{
    const char *t = jce_java_diff_trace();
    const char *p = t;

    while (*p) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        fprintf(f, "T %.*s\n", n, p);
        if (!nl)
            break;
        p = nl + 1;
    }
}

/* argv[1] is the stream file.  A FILE and not stdout, in BINARY mode: on
 * Windows a text-mode stdout turns every \n into \r\n, and the JVM's own
 * -Xcheck:jni diagnostics land on the Java side's stdout.  Neither belongs in
 * a stream that is compared byte for byte. */
int main(int argc, char **argv)
{
    int i;
    int total = jce_java_diff_case_count();
    FILE *f;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <stream-file>\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", argv[1]);
        return 2;
    }

    for (i = 0; i < total; i++) {
        JceScript *s;
        char src[8192];
        JceScriptInstance inst;
        const char *out;

        fprintf(f, "CASE %d %s\n", i, jce_java_diff_case_label(i));
        if (g_skips[i]) {
            fprintf(f, "R SKIPPED %s\n", g_skips[i]);
            continue;
        }
        jce_java_diff_reset();
        s = jce_script_create_sized(g_partial[i]
                                        ? jce_java_diff_host_partial()
                                        : jce_java_diff_host_full(),
                                    jce_java_diff_host_size());
        if (!s) {
            fprintf(f, "R VMFAIL\n");
            continue;
        }
        /* kPrelude carries string.format directives; it is an ARGUMENT here
         * and never part of the format, or snprintf would eat the call
         * expression as one of them.
         *
         * `return {}` IS LOAD-BEARING, not decoration. run_chunk() calls
         * build_instance() on whatever the chunk returns and refuses anything
         * that is not a table, so a chunk ending at the jce.log() line reports
         * R CHUNKFAIL for EVERY case while the call itself has already run --
         * the probe output is produced and then thrown away. Measured: without
         * it, 150 of the 151 cases print R CHUNKFAIL, i.e. the reference side
         * of this differential never ran once. run_differential.py's
         * "the chunk must RUN" check is what fails if this line is lost. */
        snprintf(src, sizeof src, "%s\njce.log(probe(%s))\nreturn {}\n",
                 kPrelude, g_calls[i]);
        jce_java_diff_reset();
        inst = jce_script_instantiate_source(s, "diff", src, 0);
        if (inst == 0) {
            fprintf(f, "R CHUNKFAIL\n");
            jce_script_destroy(s);
            continue;
        }
        out = jce_java_diff_out();
        fprintf(f, "R %s", out);
        if (out[0] == '\0' || out[strlen(out) - 1] != '\n')
            fprintf(f, "\n");
        print_trace(f);
        jce_script_release(s, inst);
        jce_script_destroy(s);
    }
    fprintf(f, "DONE %d\n", total);
    fclose(f);
    return 0;
}
''')
    return "\n".join(body)


# ── The JNI helper the Java driver loads ─────────────────────────────────────

def emit_diff_jni_c(members: list[HostMember], man: dict) -> str:
    return f'''\
/* jce_java_diff_jni.gen.c -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * The test-side JNI helper. It hands the Java driver the ADDRESS of the same
 * recording mock host the Lua reference driver uses, and reads back the trace.
 *
 * It is deliberately separate from scripting/java/native: the production shim
 * must not carry a line of test code, and the Java driver reaches the surface
 * through the production shim exactly as a game would.
 */

#include <jni.h>

#include "jce_java_diff_host.gen.h"

#include <stdint.h>

JNIEXPORT jlong JNICALL {DIFF_JNI_PREFIX}fullHostPointer(JNIEnv *env,
                                                         jclass cls)
{{
    (void)env;
    (void)cls;
    return (jlong)(intptr_t)jce_java_diff_host_full();
}}

JNIEXPORT jlong JNICALL {DIFF_JNI_PREFIX}partialHostPointer(JNIEnv *env,
                                                            jclass cls)
{{
    (void)env;
    (void)cls;
    return (jlong)(intptr_t)jce_java_diff_host_partial();
}}

JNIEXPORT jlong JNICALL {DIFF_JNI_PREFIX}hostSize(JNIEnv *env, jclass cls)
{{
    (void)env;
    (void)cls;
    return (jlong)jce_java_diff_host_size();
}}

JNIEXPORT void JNICALL {DIFF_JNI_PREFIX}reset(JNIEnv *env, jclass cls)
{{
    (void)env;
    (void)cls;
    jce_java_diff_reset();
}}

JNIEXPORT jstring JNICALL {DIFF_JNI_PREFIX}trace(JNIEnv *env, jclass cls)
{{
    (void)cls;
    return (*env)->NewStringUTF(env, jce_java_diff_trace());
}}

JNIEXPORT jint JNICALL {DIFF_JNI_PREFIX}caseCount(JNIEnv *env, jclass cls)
{{
    (void)env;
    (void)cls;
    return (jint)jce_java_diff_case_count();
}}

JNIEXPORT jstring JNICALL {DIFF_JNI_PREFIX}caseLabel(JNIEnv *env, jclass cls,
                                                     jint i)
{{
    (void)cls;
    return (*env)->NewStringUTF(env, jce_java_diff_case_label((int)i));
}}
'''


# ── The Java differential driver ─────────────────────────────────────────────

_DIFF_HOST_JAVA = f'''\
/* JceDiffHost.java -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * The test-side natives: the address of the shared recording mock host, and
 * the trace it wrote.
 */
package {DIFF_PKG};

/** Test-side access to the shared recording mock host. */
public final class JceDiffHost {{

    static {{
        System.load(System.getProperty("jce.diff.library"));
    }}

    private JceDiffHost() {{ }}

    public static native long fullHostPointer();
    public static native long partialHostPointer();
    public static native long hostSize();
    public static native void reset();
    public static native String trace();
    public static native int caseCount();
    public static native String caseLabel(int i);
}}
'''


def emit_diff_host_java(members: list[HostMember], man: dict) -> str:
    return _DIFF_HOST_JAVA


def emit_diff_main_java(members: list[HostMember], man: dict) -> str:
    by = {m.name: m for m in members}
    ent_by = {e["name"]: e for e in man["expose"]}
    header_text = HEADER.read_text(encoding="utf-8")
    cases = harness_cases(members, man)

    L = [f'''\
/* JceDifferential.java -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * THE JAVA SIDE of the cross-language differential. It prints the same
 * canonical stream the Lua reference driver prints, over the same recording
 * mock host, for the same manifest-derived cases in the same order.
 *
 * The answers are rendered INTO THE LUA VALUE MODEL, because Lua is the
 * reference implementation: `i:` an integer, `f:` a double\'s bit pattern,
 * `b:` a boolean, `s:` a string, `nil`, `t:[..]` a table. Two renderings are
 * read from the manifest rather than chosen here -- an entry with
 * `miss_value` renders its absent answer as that number (Lua pushes it; Java
 * returns null), and a value_return of a C string renders a null as "",
 * exactly as the Lua binding pushes "".
 *
 * WHAT THIS FORBIDS: the two surfaces disagreeing about the number of values,
 * the type of any value, the value itself, which host member was called, in
 * what order, or with which arguments. WHAT IT CANNOT SEE: a manifest
 * decision that is wrong in the same way on both sides -- both emitters read
 * the one manifest, so a mis-declared shape or capacity is invisible here and
 * is the differential\'s stated limit, not an oversight.
 */
package {DIFF_PKG};

import com.jce.script.JceScript;

/** Prints the canonical stream for every differential case. */
public final class JceDifferential {{

    private JceDifferential() {{ }}

    private static final StringBuilder SB = new StringBuilder();

    private static String i(long v) {{
        return "i:" + Long.toString(v);
    }}

    private static String f(double v) {{
        return "f:" + Long.toString(Double.doubleToRawLongBits(v));
    }}

    private static String b(boolean v) {{
        return "b:" + (v ? "true" : "false");
    }}

    private static String s(String v) {{
        return "s:" + v;
    }}

    /* Each element goes through i(), NOT Long.toString: the Lua reference's
     * canon() builds a table as t:[ .. num(v[i]) .. ], and num() tags every
     * element with its Lua type ('i:' an integer, 'f:' a double's bits). An
     * untagged element renders t:[100] against Lua's t:[i:100] and the
     * differential reports a difference in the HARNESS as though it were a
     * difference in the surface -- which is exactly how it was found. */
    private static String t(long[] v) {{
        StringBuilder sb = new StringBuilder("t:[");
        for (int k = 0; k < v.length; k++) {{
            if (k > 0) {{
                sb.append(\',\');
            }}
            sb.append(i(v[k]));
        }}
        return sb.append(\']\').toString();
    }}

    /* The stream goes to a FILE named by argv[0], never to stdout: -Xcheck:jni
     * writes its diagnostics to the JVM's own stdout, and a stream that is
     * compared byte for byte cannot share a channel with them. "\\n" is
     * written explicitly because println uses the platform separator and this
     * repository is LF. */
    private static java.io.PrintStream OUT;

    private static void line(String text) {{
        OUT.print(text);
        OUT.print("\\n");
    }}

    private static void emit(int index, String label, String result) {{
        line("CASE " + index + " " + label);
        line("R " + result);
        String tr = JceDiffHost.trace();
        int from = 0;
        while (from < tr.length()) {{
            int nl = tr.indexOf(\'\\n\', from);
            int end = nl < 0 ? tr.length() : nl;
            line("T " + tr.substring(from, end));
            if (nl < 0) {{
                break;
            }}
            from = nl + 1;
        }}
    }}

    public static void main(String[] argv) throws java.io.IOException {{
        if (argv.length < 1) {{
            System.err.println("usage: JceDifferential <stream-file>");
            System.exit(2);
        }}
        OUT = new java.io.PrintStream(new java.io.FileOutputStream(argv[0]),
                                      false, java.nio.charset.StandardCharsets.UTF_8);
        int total = JceDiffHost.caseCount();
        for (int i = 0; i < total; i++) {{
            runCase(i);
        }}
        line("DONE " + total);
        OUT.flush();
        OUT.close();
    }}

    /* ONE METHOD PER CASE, dispatched by a switch.  Inlining 150 cases into
     * runCase would put them in one method body, and a JVM method is capped
     * at 65535 bytes of bytecode -- a cap this file would cross silently as
     * the surface grows, failing at javac time with an error about a size
     * limit rather than about anything a reader of the manifest would
     * recognise. */
    private static void runCase(int index) {{
        String label = JceDiffHost.caseLabel(index);
        switch (index) {{''']

    for idx, _c in enumerate(cases):
        L.append(f"        case {idx}: c{idx}(label); return;")
    L.append("        default:")
    L.append('            throw new IllegalStateException("no case " + index);')
    L.append("        }")
    L.append("    }")
    L.append("")

    for idx, c in enumerate(cases):
        e = ent_by[c["entry"]]
        m = by[e["vtable"]]
        comps = out_components(m, e, header_text)
        L.append(f"    /* {c['label']} */")
        L.append(f"    private static void c{idx}(String label) {{")
        if c["skip"]:
            L.append(f'        line("CASE {idx} " + label);')
            L.append(f'        line("R SKIPPED {c["skip"]}");')
            L += ["    }", ""]
            continue
        ptr = ("JceDiffHost.partialHostPointer()" if c["host"] == "partial"
               else "JceDiffHost.fullHostPointer()")
        L.append("        JceDiffHost.reset();")
        L.append(f"        try (JceScript s = JceScript.open({ptr},")
        L.append("                JceDiffHost.hostSize())) {")
        L.append("            if (s == null) {")
        L.append(f'                line("CASE {idx} " + label);')
        L.append('                line("R VMFAIL");')
        L.append("                return;")
        L.append("            }")
        for ln in _java_case_body(m, e, comps, c["mode"]):
            L.append("            " + ln)
        L.append(f"            emit({idx}, label, r);")
        L.append("        }")
        L += ["    }", ""]

    L.append("}")
    return "\n".join(L) + "\n"


def _java_case_args(m: HostMember, ent: dict, mode: str) -> tuple[list[str], int]:
    """(rendered Java arguments, how many trailing ones to drop).

    In `defaults` mode the Java driver reaches the manifest defaults the way a
    Java caller would -- through the shortest overload -- and passes any
    NON-trailing default explicitly, which is what Lua's `nil` produces there.
    """
    drop = optional_tail(m, ent) if mode == "defaults" else 0
    rendered: list[str] = []
    for p, v in synth_args(m, ent, mode):
        jt = java_param_type(p, ent["name"])
        if p.arity > 0:
            elems = ", ".join(java_literal(jt[:-2], x) for x in v)
            rendered.append(f"new {jt[:-2]}[] {{{elems}}}")
        else:
            rendered.append(java_literal(jt, v))
    if drop:
        rendered = rendered[:len(rendered) - drop]
    return rendered, drop


def _java_case_body(m: HostMember, ent: dict, comps: list[Component],
                    mode: str) -> list[str]:
    """The statements that call one entry and build `r`, the canonical line."""
    shape = ent["shape"]
    name = camel(ent["name"])
    argv, _ = _java_case_args(m, ent, mode)
    call = f"s.{name}({', '.join(argv)})"
    L: list[str] = []
    miss = ent.get("miss_value")
    miss_render = ('"1|nil"' if miss is None else f'"1|" + i({int(miss)})')

    if shape == "void_call":
        L.append(f"{call};")
        L.append('String r = "0|";')
    elif shape == "value_return":
        jt = _java_scalar(m.ret, ent["name"])
        if jt == "String":
            L.append(f"String v = {call};")
            L.append('String r = "1|" + s(v);')
        elif jt == "boolean":
            L.append(f"boolean v = {call};")
            L.append('String r = "1|" + b(v);')
        elif jt in ("float", "double"):
            L.append(f"{jt} v = {call};")
            L.append('String r = "1|" + f((double) v);')
        else:
            L.append(f"{jt} v = {call};")
            L.append('String r = "1|" + i((long) v);')
    elif shape == "void_out_array":
        c = _value_components(comps)[0]
        n = c["count"]
        L.append(f"{c['elem']}[] v = {call};")
        parts = " + \"|\" + ".join(
            [("f((double) v[%d])" % k) if c["elem"] in ("float", "double")
             else ("i((long) v[%d])" % k) for k in range(n)])
        L.append(f'String r = "{n}|" + {parts};')
    elif shape == "entity_table":
        L.append(f"long[] v = {call};")
        L.append('String r = "1|" + t(v);')
    elif shape == "owned_string_release":
        L.append(f"String v = {call};")
        L.append('String r = v == null ? "1|nil" : "1|" + s(v);')
    elif shape == "first_and_count":
        rt = result_type(ent["name"])
        L.append(f"JceScript.{rt} v = {call};")
        L.append('String first = v.first == null ? "nil"'
                 ' : i(v.first.longValue());')
        L.append('String r = "2|" + first + "|" + i(v.count);')
    elif shape == "fallible_out":
        vc = _value_components(comps)
        n = slot_count(comps)
        rt = java_return(m, ent, comps)
        # Result records are NESTED in JceScript, so a driver in another
        # package names them qualified.  Unqualified compiles inside the
        # surface class and nowhere else.
        if rt == result_type(ent["name"]):
            rt = "JceScript." + rt
        L.append(f"{rt} v = {call};")
        if len(vc) == 1 and vc[0]["is_array"]:
            parts = " + \"|\" + ".join(
                [(f"f((double) v[{k}])" if vc[0]["elem"] in ("float", "double")
                  else f"i((long) v[{k}])") for k in range(vc[0]["count"])])
            L.append(f'String r = v == null ? {miss_render} '
                     f': "{n}|" + {parts};')
        elif len(vc) == 1:
            e0 = vc[0]["elem"]
            expr = ("f(v.doubleValue())" if e0 in ("float", "double")
                    else "b(v.booleanValue())" if e0 == "boolean"
                    else "i(v.longValue())")
            L.append(f'String r = v == null ? {miss_render} '
                     f': "1|" + {expr};')
        else:
            parts = []
            for c in vc:
                acc = f"v.{c['java_name']}"
                if c["is_array"]:
                    for k in range(c["count"]):
                        parts.append(
                            f"f((double) {acc}[{k}])"
                            if c["elem"] in ("float", "double")
                            else f"i((long) {acc}[{k}])")
                else:
                    parts.append(
                        f"f((double) {acc})"
                        if c["elem"] in ("float", "double")
                        else f"b({acc})" if c["elem"] == "boolean"
                        else f"i((long) {acc})")
            joined = " + \"|\" + ".join(parts)
            L.append(f'String r = v == null ? {miss_render} '
                     f': "{n}|" + {joined};')
    return L


# ═════════════════════════════════════════════════════════════════════════════
#  The backend
# ═════════════════════════════════════════════════════════════════════════════

class JavaBackend(ScriptBackend):
    name = "java"

    def artefacts(self, members: list[HostMember], man: dict) -> list[Artefact]:
        return [
            Artefact(JAVA_SRC, emit_java_surface),
            Artefact(JNI_C, emit_jni_c),
            Artefact(DIFF_HOST_H, emit_diff_host_h),
            Artefact(DIFF_HOST_C, emit_diff_host_c),
            Artefact(DIFF_LUA_C, emit_diff_lua_reference),
            Artefact(DIFF_JNI_C, emit_diff_jni_c),
            Artefact(DIFF_HOST_JAVA, emit_diff_host_java),
            Artefact(DIFF_MAIN_JAVA, emit_diff_main_java),
        ]

    def validate(self, members: list[HostMember], man: dict,
                 c_text: str) -> list[str]:
        """This backend's OWN conditions. Each one names what it forbids.

        None of them read jce_script.c: Java's TU has no register_binding call
        and no lua_CFunction, so Lua's condition 5 means nothing here."""
        problems: list[str] = []
        by = {m.name: m for m in members}

        # J1 -- the tenth modifier. Every emitter reads modifiers by exact
        # name with .get(), so a modifier this backend does not render is
        # SILENTLY dropped from the Java surface while the Lua surface keeps
        # it. The core's vocabulary check cannot see that: the key IS known,
        # just not here.
        for name in sorted(MODIFIERS - _HANDLED_MODIFIERS):
            if any(name in e for e in man["expose"]):
                problems.append(
                    f"emit_java: modifier '{name}' is used by the manifest but "
                    f"is not in _HANDLED_MODIFIERS -- the Java surface would "
                    f"drop it silently. Render it or mark those bindings "
                    f"hand_written.")

        # J2 -- the name rule, all three ways it can fail.
        seen: dict[str, str] = {}
        for e in man["expose"]:
            n = e["name"]
            if not _SNAKE_RE.match(n):
                problems.append(
                    f"emit_java: expose[{n}] is not lower_snake_case, so the "
                    f"camelCase rule has no defined result for it")
                continue
            j = camel(n)
            if j in _JAVA_RESERVED:
                problems.append(
                    f"emit_java: expose[{n}] camels to '{j}', which is a Java "
                    f"reserved word and cannot be a method name")
            if j in seen and seen[j] != n:
                problems.append(
                    f"emit_java: expose[{n}] and expose[{seen[j]}] both camel "
                    f"to '{j}' -- one would silently overwrite the other")
            seen[j] = n
            m = by.get(e["vtable"])
            if m is None:
                continue                       # the core already reported it
            for p in script_in_params(m, e):
                if not _SNAKE_RE.match(p.name):
                    problems.append(
                        f"emit_java: expose[{n}] parameter '{p.name}' is not "
                        f"lower_snake_case")

        # J3 -- every C type on the surface has a Java mapping. Reported as a
        # condition rather than raised from the emitter, so a new type names
        # itself in the gate's output instead of aborting mid-write.
        for e in man["expose"]:
            m = by.get(e["vtable"])
            if m is None:
                continue
            for p in script_in_params(m, e):
                t = _elem_c_type(p.c_type) if p.arity > 0 else p.c_type
                if t not in JAVA_SCALAR:
                    problems.append(
                        f"emit_java: expose[{e['name']}] parameter "
                        f"'{p.name}' has C type '{p.c_type}', which has no "
                        f"Java mapping")
            if e["shape"] == "value_return" and m.ret not in JAVA_SCALAR:
                problems.append(
                    f"emit_java: expose[{e['name']}] returns '{m.ret}', which "
                    f"has no Java mapping")

        # J4 -- a constant kind this backend has not decided about. json_null
        # is deliberately absent from the Java surface (a lightuserdata
        # sentinel is a Lua VM object and its only producer is hand-written);
        # a constant of some OTHER kind would be a decision nobody has made,
        # and silently omitting it would make the Java surface smaller than
        # the manifest without saying so.
        for c in man["constants"]:
            if c["kind"] != "lightuserdata_sentinel":
                problems.append(
                    f"emit_java: constant '{c['name']}' has kind "
                    f"'{c['kind']}', which this backend has no rendering for. "
                    f"Decide whether Java exposes it, in emit_java.py.")
        return problems


BACKEND = JavaBackend()
