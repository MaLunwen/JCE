#!/usr/bin/env python3
"""
emit_lua.py — the Lua backend.  One of N; see scriptgen_core.py's docstring
for how to add another without touching this file or that one.

What lives here is everything that is true only because the target is Lua:

  * the `lua_pushX` / `luaL_checkX` marshalling tables and the stack-slot
    arithmetic that walks them;
  * the emitted body of each of the seven shapes, including the SPELLING of
    the host-member guard (`s->have_host && s->host.<member>`, and its De
    Morgan dual where an index_base binding folds the guard into an early
    return).  The guard's REQUIREMENT is an ABI fact and lives in the core;
    only its spelling is here, because `jce_script_self_from_upvalue` is a
    lua_State upvalue mechanism and the `s` it produces does not exist in a
    backend that does not run inside a lua_State;
  * `jce_script_bindings.gen.{c,h}` — this backend's two committed artefacts;
  * CONDITION 5, registration parity.  It sat inside the core's validate()
    while Lua was the only backend and read as neutral validation; it is not.
    It greps for `register_binding(L, s, "name", fn)` and for
    `static int l_x(lua_State *L)`, which are the Lua installer and the Lua
    C-function signature.  A Python TU registers into a PyMethodDef table and
    would be invisible to every line of it.

Its tests: test_script_bindings_gate.py (class TestEmitters,
plus the registration-parity tests in TestManifestGate).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from scriptgen_core import (                               # noqa: E402
    Artefact,
    C_ZERO,
    HEADER,
    HostMember,
    Param,
    REPO_ROOT,
    SCRIPT_C,
    ScriptBackend,
    c_literal,
    flatten_pod,
    out_params,
    script_in_params,
)

GEN_C = REPO_ROOT / "engine/src/middleware/script/jce_script_bindings.gen.c"
GEN_H = REPO_ROOT / "engine/src/middleware/script/jce_script_bindings.gen.h"


# ── Condition 5: registration parity, both directions ────────────────────────

_REGISTER_RE = re.compile(r'register_binding\s*\(\s*L\s*,\s*s\s*,\s*"(\w+)"')

# No `$` anchor.  While the hand-written 71 existed, three of them were
# ONE-LINERS -- `static int l_jce_sprint(lua_State *L) { return ...; }` -- and
# an end-of-line anchor skipped exactly those three while still reporting a
# confident count, which is the shape of "the gate passed because it checked
# nothing".  Those three are gone (the generator emits full bodies), so the
# anchor would cost nothing TODAY; it stays off because the next hand-written
# binding may be a one-liner again and nothing would announce that.
_LUA_CFUNC_RE = re.compile(r"^static int (l_\w+)\(lua_State \*L\)", re.M)

# The one file-static lua_CFunction that is deliberately not in the jce table:
# l_sandbox_load, which jce_script.c installs as the GLOBAL `load` inside
# open_sandboxed_libs.  Named by SYMBOL, not by line: the `jce_script.c:1481`
# this comment used to carry was already pointing at the wrong line by the time
# the hand-written bindings were deleted.
_NOT_IN_TABLE = frozenset({"l_sandbox_load"})


def collect_registered_names(c_text: str) -> list[str]:
    return _REGISTER_RE.findall(c_text)


def registered_names_all() -> list[str]:
    """Every register_binding call site, across BOTH translation units.

    jce_script.c registers the seven hand-written names; the generated TU
    registers the other 71.  The regex matches `jce_script_register_binding(`
    too, since `register_binding(` is a substring of it."""
    text = SCRIPT_C.read_text(encoding="utf-8")
    if GEN_C.is_file():
        text += GEN_C.read_text(encoding="utf-8")
    return collect_registered_names(text)


def collect_static_lua_cfunctions(c_text: str) -> list[str]:
    return [n for n in _LUA_CFUNC_RE.findall(c_text) if n not in _NOT_IN_TABLE]


# ── Marshalling ──────────────────────────────────────────────────────────────

_PUSH = {
    "float":           "lua_pushnumber(L, (lua_Number){x});",
    "double":          "lua_pushnumber(L, (lua_Number){x});",
    "bool":            "lua_pushboolean(L, {x} ? 1 : 0);",
    "int":             "lua_pushinteger(L, (lua_Integer){x});",
    "uint32_t":        "lua_pushinteger(L, (lua_Integer){x});",
    "uint64_t":        "lua_pushinteger(L, (lua_Integer){x});",
    "JceScriptEntity": "lua_pushinteger(L, (lua_Integer){x});",
    "const char *":    "lua_pushstring(L, {x});",
}


def _push(c_type: str, expr: str) -> str:
    if c_type not in _PUSH:
        raise SystemExit(f"error: no push rule for C type {c_type!r} — mark the "
                         f"binding hand_written rather than adding a rule here")
    return _PUSH[c_type].replace("{x}", expr)


def _read_arg(p: Param, slot: int, ent: dict) -> str:
    """One Lua-stack read, named after the HEADER's own parameter name."""
    opt = ent.get("optional") or {}
    strict = set(ent.get("strict") or [])
    t = p.c_type
    if p.arity > 0:            # `const T name[N]` — N stack slots into a local
        base = t[len("const "):]
        cast = "(float)" if base == "float" else ""
        rd = [f"{base} {p.name}[{p.arity}];"]
        rd += [f"    {p.name}[{i}] = {cast}luaL_checknumber(L, {slot + i});"
               for i in range(p.arity)]
        return "\n".join(rd)
    if t == "const char *":
        if p.name in opt and opt[p.name] is None:
            return (f"const char *{p.name} = lua_isstring(L, {slot}) "
                    f"? lua_tostring(L, {slot}) : NULL;")
        if p.name in opt:
            return f'const char *{p.name} = luaL_optstring(L, {slot}, "{opt[p.name]}");'
        return f"const char *{p.name} = luaL_checkstring(L, {slot});"
    if t == "bool":
        if p.name in opt:
            return (f"bool {p.name} = lua_isnoneornil(L, {slot}) "
                    f"? {c_literal(opt[p.name])} : lua_toboolean(L, {slot});")
        if p.name in strict:
            return (f"luaL_checktype(L, {slot}, LUA_TBOOLEAN);\n"
                    f"    bool {p.name} = lua_toboolean(L, {slot}) != 0;")
        return f"bool {p.name} = lua_toboolean(L, {slot}) != 0;"
    if t in ("float", "double"):
        cast = "(float)" if t == "float" else ""
        if p.name in opt:
            return f"{t} {p.name} = {cast}luaL_optnumber(L, {slot}, {c_literal(opt[p.name])});"
        return f"{t} {p.name} = {cast}luaL_checknumber(L, {slot});"
    if p.name in opt:
        return f"{t} {p.name} = ({t})luaL_optinteger(L, {slot}, {c_literal(opt[p.name])});"
    return f"{t} {p.name} = ({t})luaL_checkinteger(L, {slot});"


def _emit_one(m: HostMember, ent: dict, header_text: str) -> str:
    """Emit ONE binding.  Every branch keeps `s->have_host && s->host.<member>`;
    that guard is required by the short-struct ABI and no manifest field turns
    it off (spec 5.5)."""
    name, shape = ent["name"], ent["shape"]
    bind = ent.get("bind_args") or {}
    lines = [f"static int l_jce_{name}(lua_State *L)", "{",
             "    JceScript *s = jce_script_self_from_upvalue(L);"]

    ins = script_in_params(m, ent)
    outs = out_params(m)
    slot = 1
    args = ["s->host.user"]
    for p in ins:
        if p.name in bind:
            args.append(f"({p.c_type}){c_literal(bind[p.name])}")
            continue
        if ent.get("index_base") and p is ins[0]:
            lines.append(f"    lua_Integer lua_{p.name} = luaL_checkinteger(L, {slot});")
            args.append(f"({p.c_type})(lua_{p.name} - {ent['index_base']})")
            slot += 1
            continue
        lines.append("    " + _read_arg(p, slot, ent))
        args.append(p.name)
        slot += p.arity if p.arity > 0 else 1   # a const T[N] input eats N slots

    guard = f"s->have_host && s->host.{m.name}"

    if shape == "void_call":
        if outs:
            raise SystemExit(f"error: {name}: void_call with an out parameter")
        lines += [f"    if ({guard})",
                  f"        s->host.{m.name}({', '.join(args)});",
                  "    return 0;", "}"]
        return "\n".join(lines) + "\n"

    if shape == "value_return":
        absent = ent.get("absent_value")
        if isinstance(absent, dict):
            default = absent["param"]
        elif absent is not None:
            default = c_literal(absent) + ("f" if m.ret == "float"
                                           and isinstance(absent, float) else "")
        else:
            default = C_ZERO[m.ret]
        lines += [f"    {m.ret} v = ({guard})",
                  f"                ? s->host.{m.name}({', '.join(args)}) : {default};"]
        if "clamp_min" in ent:
            lines += [f"    if (v < {c_literal(ent['clamp_min'])})",
                      f"        v = {c_literal(ent['clamp_min'])};"]
        if m.ret == "const char *":
            fallback = default if isinstance(absent, dict) else '""'
            lines.append(f"    lua_pushstring(L, v ? v : {fallback});")
        else:
            lines.append("    " + _push(m.ret, "v"))
        lines += ["    return 1;", "}"]
        return "\n".join(lines) + "\n"

    if shape == "void_out_array":
        o = outs[0]
        zeros = ", ".join([C_ZERO[o.c_type]] * o.arity)
        lines += [f"    {o.c_type} {o.name}[{o.arity}] = {{ {zeros} }};",
                  f"    if ({guard})",
                  f"        s->host.{m.name}({', '.join(args + [o.name])});"]
        lines += ["    " + _push(o.c_type, f"{o.name}[{i}]") for i in range(o.arity)]
        lines += [f"    return {o.arity};", "}"]
        return "\n".join(lines) + "\n"

    if shape == "fallible_out":
        pushes: list[tuple[str, str]] = []      # (c_type, expression)
        decls: list[str] = []
        call_args = list(args)
        for o in outs:
            if o.arity > 0:
                decls.append(f"    {o.c_type} {o.name}[{o.arity}];")
                call_args.append(o.name)
                pushes += [(o.c_type, f"{o.name}[{i}]") for i in range(o.arity)]
            elif o.c_type not in C_ZERO:
                decls += [f"    {o.c_type} {o.name};",
                          f"    memset(&{o.name}, 0, sizeof {o.name});"]
                call_args.append(f"&{o.name}")
                pushes += [(t, f"{o.name}.{e}")
                           for e, t in flatten_pod(header_text, o.c_type)]
            else:
                decls.append(f"    {o.c_type} {o.name} = {C_ZERO[o.c_type]};")
                call_args.append(f"&{o.name}")
                pushes.append((o.c_type, o.name))
        miss = ent.get("miss_value")
        miss_line = ("    lua_pushnil(L);" if miss is None
                     else f"    lua_pushinteger(L, {c_literal(miss)});   /* miss */")
        lines += decls
        if ent.get("index_base"):
            first = ins[0].name
            lines += [f"    if (lua_{first} < {ent['index_base']} || !s->have_host || "
                      f"!s->host.{m.name} ||",
                      f"        !s->host.{m.name}({', '.join(call_args)})) {{",
                      "    " + miss_line, "        return 1;", "    }"]
            lines += ["    " + _push(t, e) for t, e in pushes]
            lines += [f"    return {len(pushes)};", "}"]
        else:
            lines += [f"    if ({guard} &&",
                      f"        s->host.{m.name}({', '.join(call_args)})) {{"]
            lines += ["        " + _push(t, e) for t, e in pushes]
            lines += [f"        return {len(pushes)};", "    }", miss_line,
                      "    return 1;", "}"]
        return "\n".join(lines) + "\n"

    if shape in ("first_and_count", "entity_table"):
        o = outs[0]
        cap = ent["out_capacity"]
        lines += [f"    {o.c_type} found[{cap}];", "    int n = 0;",
                  f"    if ({guard})",
                  f"        n = s->host.{m.name}("
                  f"{', '.join(args + ['found', str(cap)])});"]
        if shape == "first_and_count":
            lines += ["    if (n > 0)", "        " + _push(o.c_type, "found[0]"),
                      "    else", "        lua_pushnil(L);",
                      "    " + _push("int", "n"), "    return 2;", "}"]
        else:
            lines += ["    lua_createtable(L, n, 0);",
                      "    for (int i = 0; i < n; i++) {",
                      "        " + _push(o.c_type, "found[i]"),
                      "        lua_rawseti(L, -2, i + 1);", "    }",
                      "    return 1;", "}"]
        return "\n".join(lines) + "\n"

    if shape == "owned_string_release":
        rel = ent["release"]
        lines += ["    char *json = NULL;", f"    if ({guard})",
                  f"        json = s->host.{m.name}({', '.join(args)});",
                  "    if (json) {", "        lua_pushstring(L, json);",
                  # Guards on the release member ALONE, exactly as
                  # jce_script.c:1036 does: a non-NULL json already implies
                  # have_host.  Normalising it away crashes a partial host.
                  f"        if (s->host.{rel}) s->host.{rel}(s->host.user, json);",
                  "    } else {", "        lua_pushnil(L);", "    }",
                  "    return 1;", "}"]
        return "\n".join(lines) + "\n"

    raise SystemExit(f"error: {name}: unknown shape {shape!r}")


_C_BANNER = """\
/* jce_script_bindings.gen.c — GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * Source of truth: struct JceScriptHost in
 * engine/include/jce/middleware/script/jce_script.h (C types, arity,
 * parameter names) joined with the exposure decisions in
 * engine/src/middleware/script/script_exposure.json (name, shape, modifiers).
 *
 * This file is owned WHOLE by the generator, which never opens jce_script.c.
 * There are no sentinel-delimited regions: a tool that rewrites part of a file
 * containing hand-written code eventually eats the hand-written code.
 *
 * EVERY function guards the host member before calling it, and that is not
 * optional.  jce_script_create_sized copies min(host_size, sizeof
 * s->host) over a zeroed table, so a member an older caller's header did not
 * have stays NULL and calling it unguarded jumps through whatever followed
 * the caller's shorter object.  That is the crash
 * tests/middleware/script/test_jce_script_host_abi.c exists to catch.
 *
 * The guard has TWO spellings and grepping for only the first will convince
 * you this file is broken when it is not:
 *
 *     s->have_host && s->host.<member>          @PLAIN@ functions
 *     !s->have_host || !s->host.<member>        @FOLDED@ (@FOLDED_NAMES@),
 *                                               where an index_base binding
 *                                               folds the guard into its
 *                                               early return
 *
 * Both are checked, per function and against the member the manifest names,
 * by test_script_bindings_gate.py:
 *     test_the_have_host_guard_is_in_every_emitted_function
 *
 * No line numbers are cited into jce_script.c from this banner ON PURPOSE.
 * Line citations here would rot on the next edit to that file -- which is
 * this repository's most-repeated failure and the reason the manifest's own
 * citations are gated at all.  Symbols do not rot.
 */

#include "jce_script_bindings.gen.h"
#include "jce_script_internal.h"

#include <string.h>

"""


def _c_banner(man: dict) -> str:
    """The banner, with its two guard-spelling counts DERIVED.

    Written out by hand they are a claim nothing enforces: adding a second
    index_base binding would silently make "70 / get_touch alone" false, and a
    reader who greps the first spelling and counts 69 concludes the file is
    broken.  Deriving them means the sentence cannot rot away from the code it
    describes.  test_the_banner_counts_match_the_emitted_bodies is what fails
    if this stops being true."""
    folded = [e["name"] for e in man["expose"] if e.get("index_base")]
    plain = len(man["expose"]) - len(folded)
    return (_C_BANNER
            .replace("@PLAIN@", str(plain))
            .replace("@FOLDED_NAMES@", ", ".join("jce." + n for n in folded))
            .replace("@FOLDED@", f"{len(folded)} function"
                                 f"{'' if len(folded) == 1 else 's'}"))


def emit_bindings_c(members: list[HostMember], man: dict) -> str:
    by = {m.name: m for m in members}
    header_text = HEADER.read_text(encoding="utf-8")
    out = [_c_banner(man)]
    for e in man["expose"]:
        m = by[e["vtable"]]
        doc = e.get("doc")
        out.append(f"/* jce.{e['name']} — shape: {e['shape']}"
                   + (f"\n * {doc}" if doc else "") + " */\n")
        out.append(_emit_one(m, e, header_text))
        out.append("\n")
    out.append("const char *const JCE_SCRIPT_GENERATED_BINDING_NAMES[] = {\n")
    out += [f'    "{e["name"]}",\n' for e in man["expose"]]
    out.append("};\n\n")
    out.append("void jce_script_install_generated_bindings(JceScript *s)\n{\n"
               "    lua_State *L = s->L;\n")
    out += [f'    jce_script_register_binding(L, s, "{e["name"]}", '
            f'l_jce_{e["name"]});\n' for e in man["expose"]]
    for c in man["constants"]:
        out.append(f"    /* {c['name']}: {c['kind']} — no register_binding call,\n"
                   "     * so no source regex can see it.\n"
                   "     * tests/middleware/script/test_jce_script_table_shape.c "
                   "can. */\n")
        out.append("    lua_pushlightuserdata(L, &jce_script_json_null_token);\n")
        out.append(f'    lua_setfield(L, -2, "{c["name"]}");\n')
    out.append("}\n")
    return "".join(out)


def emit_bindings_h(man: dict) -> str:
    n = len(man["expose"])
    return (
        "/* jce_script_bindings.gen.h — GENERATED. DO NOT EDIT.\n"
        " *   python tools/scriptgen/gen_script_bindings.py --write\n"
        " */\n"
        "#ifndef JCE_SCRIPT_BINDINGS_GEN_H\n"
        "#define JCE_SCRIPT_BINDINGS_GEN_H\n\n"
        '#include "jce_script_internal.h"\n\n'
        f"#define JCE_SCRIPT_GENERATED_BINDING_COUNT {n}\n\n"
        "/* Registration order matches script_exposure.json's expose[] order. */\n"
        "extern const char *const JCE_SCRIPT_GENERATED_BINDING_NAMES"
        "[JCE_SCRIPT_GENERATED_BINDING_COUNT];\n\n"
        "/* Installs every generated binding into the table on top of s->L's\n"
        " * stack, then the manifest's constants. */\n"
        "void jce_script_install_generated_bindings(JceScript *s);\n\n"
        "#endif /* JCE_SCRIPT_BINDINGS_GEN_H */\n")


def _render_h(members: list[HostMember], man: dict) -> str:
    """Artefact.render is uniformly render(members, man); the .h needs only the
    manifest, and adapting here beats special-casing the driver."""
    return emit_bindings_h(man)


class LuaBackend(ScriptBackend):
    name = "lua"

    def artefacts(self, members: list[HostMember], man: dict) -> list[Artefact]:
        return [Artefact(GEN_C, emit_bindings_c), Artefact(GEN_H, _render_h)]

    def validate(self, members: list[HostMember], man: dict,
                 c_text: str) -> list[str]:
        """Condition 5: registration parity, both directions.

        BOTH translation units: the seven hand-written names are registered in
        jce_script.c and the other 71 in jce_script_bindings.gen.c.  Reading
        only jce_script.c here would report 71 missing entries forever.  The
        `l_*` scan below still reads jce_script.c ALONE -- it asks "is there a
        lua_CFunction here that no manifest entry claims", and the generated
        file is by construction all manifest entries."""
        problems: list[str] = []
        registered = registered_names_all()
        named = ([e["name"] for e in man["expose"]] +
                 [e["name"] for e in man["hand_written"]])
        for n in sorted(set(registered) - set(named)):
            problems.append(f"register_binding(\"{n}\") has no manifest entry")
        for n in sorted(set(named) - set(registered)):
            problems.append(
                f"manifest entry '{n}' is never registered in install_bindings()")
        hand = {e["name"] for e in man["hand_written"]}
        for fn in collect_static_lua_cfunctions(c_text):
            script_name = fn[len("l_jce_"):] if fn.startswith("l_jce_") else fn
            if script_name in hand:
                continue
            if script_name in {e["name"] for e in man["expose"]}:
                continue
            problems.append(
                f"{fn} is a lua_CFunction in jce_script.c that no manifest entry "
                f"claims -- mark it hand_written or delete it")
        return problems


BACKEND = LuaBackend()
