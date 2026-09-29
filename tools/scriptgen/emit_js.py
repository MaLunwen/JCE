#!/usr/bin/env python3
"""
emit_js.py — the JavaScript backend.  One of N; see scriptgen_core.py's
docstring for how to add another without touching this file or that one.

What lives here is everything that is true only because the target is
QuickJS:

  * the `JS_NewInt32` / `JS_ToFloat64` marshalling tables and the argv
    indexing that walks them, including the JSValue OWNERSHIP rules -- a
    JS_ToCString must be freed, and a JSValue built for an array element is
    consumed by JS_SetPropertyUint32 and must NOT be freed again;
  * how a member with more than one output is returned.  QuickJS has one
    return value where Lua has a stack, so N outputs become an Array of N,
    exactly the flattening emit_python.py applies to build its tuple.  ONE
    output is returned bare, so the common case reads like JavaScript;
  * where `s` comes from.  Lua carries it in a closure upvalue; here it is
    the CONTEXT OPAQUE, set once by js_create.  Both are backend mechanisms
    for the same ABI fact -- the guard `s->have_host && s->host.<member>`
    below is the fact, and it is not optional in any backend;
  * `scripting/js/src/jce_script_bindings_js.gen.{c,h}` -- this backend's two
    committed artefacts;
  * registration parity for the JS installer, which registers through a
    JSCFunctionListEntry table and would be invisible to emit_lua.py's
    `register_binding(L, s, ...)` regex.

A .jcejs script sees these as methods on a global `jce` object, under the
same names the other four languages use.
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
    ScriptBackend,
    c_literal,
    flatten_pod,
    out_params,
    script_in_params,
)

GEN_C = REPO_ROOT / "scripting/js/src/jce_script_bindings_js.gen.c"
GEN_H = REPO_ROOT / "scripting/js/src/jce_script_bindings_js.gen.h"


# -- Marshalling -------------------------------------------------------------

def _new_value(c_type: str, expr: str) -> str:
    """A C expression building the JSValue for `expr` of type `c_type`.

    Signed vs unsigned matters and is not cosmetic: a JceScriptEntity is a
    uint32_t and JS_NewInt32 would hand a script -1 for entity 4294967295.
    JS numbers are doubles, so every 32-bit integer is exact either way --
    the choice only decides which ones come out negative."""
    if c_type == "bool":
        return "JS_NewBool(ctx, " + expr + ")"
    if c_type in ("float", "double"):
        return "JS_NewFloat64(ctx, (double)" + expr + ")"
    if c_type in ("uint32_t", "JceScriptEntity"):
        return "JS_NewUint32(ctx, (uint32_t)" + expr + ")"
    if c_type == "uint64_t":
        # Through a double, deliberately: BigInt would make these the only
        # values in this API a script cannot compare with === against a
        # literal, and the engine's ids are well inside 2^53.
        return "JS_NewFloat64(ctx, (double)" + expr + ")"
    if c_type in ("const char *", "char *"):
        return "JS_NewString(ctx, " + expr + ")"
    return "JS_NewInt32(ctx, (int32_t)" + expr + ")"


def _read_arg(p: Param, idx: int, ent: dict) -> list[str]:
    """Read argv[idx] into a local named p.name.  Returns C lines.

    Anything that can allocate (a string) is recorded by the caller so the
    single cleanup path frees it.  QuickJS does not free a JS_ToCString for
    you, and a binding that leaks one leaks it on EVERY call from a script."""
    opt = ent.get("optional") or {}
    t = p.c_type

    if p.arity > 0:            # `const T name[N]` -- ONE JS argument, an array
        base = t[len("const "):] if t.startswith("const ") else t
        cast = "(float)" if base == "float" else "(" + base + ")"
        return [
            base + " " + p.name + "[" + str(p.arity) + "] = { 0 };",
            "{",
            "    int ai;",
            "    for (ai = 0; ai < " + str(p.arity) + "; ++ai) {",
            "        double ad = 0.0;",
            "        JSValue ae = JS_GetPropertyUint32(ctx, argv[" + str(idx)
            + "], (uint32_t)ai);",
            "        int abad = JS_ToFloat64(ctx, &ad, ae) < 0;",
            "        JS_FreeValue(ctx, ae);",
            "        if (abad) goto fail;",
            "        " + p.name + "[ai] = " + cast + "ad;",
            "    }",
            "}",
        ]

    if t == "const char *":
        # ASSIGNMENT ONLY, into two hoisted locals -- _emit_one declares them
        # at the top of the function.  Two, not one, and that is the whole
        # point: <name>_cs is what JS_ToCString OWNS and the cleanup path
        # frees, while <name> is what the host member is called with.  They
        # differ exactly when an `optional` default supplies a STRING LITERAL,
        # and freeing that literal is a heap corruption a one-variable version
        # cannot even express.
        #
        # Hoisted because a `goto fail` from an earlier argument jumps over any
        # declaration that follows it, leaving the cleanup path reading an
        # indeterminate pointer and freeing it (MSVC C4700 -- a real crash).
        read = ["if (argc > " + str(idx) + " && !JS_IsUndefined(argv[" + str(idx)
                + "]) && !JS_IsNull(argv[" + str(idx) + "]))",
                "    " + p.name + "_cs = JS_ToCString(ctx, argv[" + str(idx)
                + "]);"]
        if p.name in opt and opt[p.name] is None:
            return read + [p.name + " = " + p.name + "_cs;"]
        if p.name in opt:
            return read + [p.name + " = " + p.name + "_cs ? " + p.name + "_cs : "
                           + c_literal(opt[p.name]) + ";"]
        return [
            p.name + "_cs = JS_ToCString(ctx, argv[" + str(idx) + "]);",
            "if (!" + p.name + "_cs) goto fail;",
            p.name + " = " + p.name + "_cs;",
        ]

    if t == "bool":
        if p.name in opt:
            return [
                "bool " + p.name + " = (argc > " + str(idx)
                + " && !JS_IsUndefined(argv[" + str(idx) + "]))",
                "    ? (JS_ToBool(ctx, argv[" + str(idx) + "]) != 0) : "
                + c_literal(opt[p.name]) + ";",
            ]
        # `strict` is a Lua rendering of "reject a non-boolean" through
        # luaL_checktype.  JS has no such call and its own coercion is what a
        # script author expects, so the modifier is READ and deliberately
        # renders as the ordinary truthiness read; the contract it describes
        # (this argument is a boolean) is unchanged.
        return ["bool " + p.name + " = JS_ToBool(ctx, argv[" + str(idx)
                + "]) != 0;"]

    # Every remaining scalar is a number.  Read as a double and cast: a script
    # that passes 3.7 for an int gets truncation, which is also what JS_ToInt32
    # does, but JS_ToFloat64 additionally accepts the numeric strings JS itself
    # accepts everywhere else.
    cast = ""
    if t == "float":
        cast = "(float)"
    elif t != "double":
        cast = "(" + t + ")"
    d = c_literal(opt[p.name]) if p.name in opt else "0.0"
    lines = ["double " + p.name + "_d = " + str(d) + ";"]
    if p.name in opt:
        lines += [
            "if (argc > " + str(idx) + " && !JS_IsUndefined(argv[" + str(idx)
            + "]) &&",
            "    JS_ToFloat64(ctx, &" + p.name + "_d, argv[" + str(idx)
            + "]) < 0) goto fail;",
        ]
    else:
        lines += [
            "if (JS_ToFloat64(ctx, &" + p.name + "_d, argv[" + str(idx)
            + "]) < 0) goto fail;",
        ]
    lines.append(t + " " + p.name + " = " + cast + p.name + "_d;")
    return lines


def _returns(pushes: list[tuple[str, str]], indent: str) -> list[str]:
    """Return the flattened slots: bare when there is one, an Array when more.

    An Array and not an object: the published contract names the slots in
    order and gives no key for them, so an object would be this backend
    inventing names the other four do not have."""
    if not pushes:
        return [indent + "ret = JS_UNDEFINED;"]
    if len(pushes) == 1:
        t, e = pushes[0]
        return [indent + "ret = " + _new_value(t, e) + ";"]
    lines = [indent + "ret = JS_NewArray(ctx);"]
    for i, (t, e) in enumerate(pushes):
        lines.append(indent + "JS_SetPropertyUint32(ctx, ret, " + str(i) + ", "
                     + _new_value(t, e) + ");")
    return lines


def _emit_one(m: HostMember, ent: dict, header_text: str) -> str:
    """Emit ONE binding.  Every branch keeps `s->have_host && s->host.<member>`;
    that guard is required by the short-struct ABI and no manifest field turns
    it off (spec 5.5)."""
    name, shape = ent["name"], ent["shape"]
    bind = ent.get("bind_args") or {}
    body: list[str] = []
    strings: list[str] = []       # JS_ToCString results this body must free

    ins = script_in_params(m, ent)
    outs = out_params(m)
    idx = 0
    args = ["s->host.user"]
    for p in ins:
        if p.name in bind:
            args.append("(" + p.c_type + ")" + c_literal(bind[p.name]))
            continue
        if ent.get("index_base") and p is ins[0]:
            body += [
                "double js_" + p.name + "_d = 0.0;",
                "if (JS_ToFloat64(ctx, &js_" + p.name + "_d, argv[" + str(idx)
                + "]) < 0) goto fail;",
                "long js_" + p.name + " = (long)js_" + p.name + "_d;",
            ]
            args.append("(" + p.c_type + ")(js_" + p.name + " - "
                        + str(ent["index_base"]) + ")")
            idx += 1
            continue
        body += _read_arg(p, idx, ent)
        if p.c_type == "const char *" and p.arity == 0:
            strings.append(p.name)
        args.append(p.name)
        idx += 1                  # a const T[N] input is ONE argument here

    guard = "s->have_host && s->host." + m.name
    tail: list[str] = []

    if shape == "void_call":
        if outs:
            raise SystemExit("error: " + name + ": void_call with an out parameter")
        tail += ["if (" + guard + ")",
                 "    s->host." + m.name + "(" + ", ".join(args) + ");",
                 "ret = JS_UNDEFINED;"]

    elif shape == "value_return":
        absent = ent.get("absent_value")
        if isinstance(absent, dict):
            default = absent["param"]
        elif absent is not None:
            default = c_literal(absent) + ("f" if m.ret == "float"
                                           and isinstance(absent, float) else "")
        else:
            default = C_ZERO[m.ret]
        tail += [m.ret + " v = (" + guard + ")",
                 "            ? s->host." + m.name + "(" + ", ".join(args)
                 + ") : " + str(default) + ";"]
        if "clamp_min" in ent:
            tail += ["if (v < " + c_literal(ent["clamp_min"]) + ")",
                     "    v = " + c_literal(ent["clamp_min"]) + ";"]
        if m.ret == "const char *":
            fallback = default if isinstance(absent, dict) else '""'
            tail.append("ret = JS_NewString(ctx, v ? v : " + str(fallback) + ");")
        else:
            tail.append("ret = " + _new_value(m.ret, "v") + ";")

    elif shape == "void_out_array":
        o = outs[0]
        zeros = ", ".join([C_ZERO[o.c_type]] * o.arity)
        tail += [o.c_type + " " + o.name + "[" + str(o.arity) + "] = { " + zeros
                 + " };",
                 "if (" + guard + ")",
                 "    s->host." + m.name + "(" + ", ".join(args + [o.name]) + ");"]
        tail += _returns([(o.c_type, o.name + "[" + str(i) + "]")
                          for i in range(o.arity)], "")

    elif shape == "fallible_out":
        pushes: list[tuple[str, str]] = []
        decls: list[str] = []
        call_args = list(args)
        for o in outs:
            if o.arity > 0:
                decls.append(o.c_type + " " + o.name + "[" + str(o.arity) + "];")
                call_args.append(o.name)
                pushes += [(o.c_type, o.name + "[" + str(i) + "]")
                           for i in range(o.arity)]
            elif o.c_type not in C_ZERO:
                decls += [o.c_type + " " + o.name + ";",
                          "memset(&" + o.name + ", 0, sizeof " + o.name + ");"]
                call_args.append("&" + o.name)
                pushes += [(t, o.name + "." + e)
                           for e, t in flatten_pod(header_text, o.c_type)]
            else:
                decls.append(o.c_type + " " + o.name + " = "
                             + C_ZERO[o.c_type] + ";")
                call_args.append("&" + o.name)
                pushes.append((o.c_type, o.name))
        miss = ent.get("miss_value")
        miss_expr = ("JS_NULL" if miss is None
                     else _new_value("int", c_literal(miss)))
        tail += decls
        if ent.get("index_base"):
            first = ins[0].name
            tail += ["if (js_" + first + " < " + str(ent["index_base"])
                     + " || !s->have_host || !s->host." + m.name + " ||",
                     "    !s->host." + m.name + "(" + ", ".join(call_args)
                     + ")) {",
                     "    ret = " + miss_expr + ";   /* miss */",
                     "} else {"]
            tail += _returns(pushes, "    ")
            tail.append("}")
        else:
            tail += ["if (" + guard + " &&",
                     "    s->host." + m.name + "(" + ", ".join(call_args)
                     + ")) {"]
            tail += _returns(pushes, "    ")
            tail += ["} else {", "    ret = " + miss_expr + ";   /* miss */", "}"]

    elif shape in ("first_and_count", "entity_table"):
        o = outs[0]
        cap = ent["out_capacity"]
        tail += [o.c_type + " found[" + str(cap) + "];", "int n = 0;",
                 "if (" + guard + ")",
                 "    n = s->host." + m.name + "("
                 + ", ".join(args + ["found", str(cap)]) + ");"]
        if shape == "first_and_count":
            tail += ["ret = JS_NewArray(ctx);",
                     "JS_SetPropertyUint32(ctx, ret, 0,",
                     "                     n > 0 ? "
                     + _new_value(o.c_type, "found[0]") + " : JS_NULL);",
                     "JS_SetPropertyUint32(ctx, ret, 1, "
                     + _new_value("int", "n") + ");"]
        else:
            tail += ["{", "    int fi;", "    ret = JS_NewArray(ctx);",
                     "    for (fi = 0; fi < n; ++fi)",
                     "        JS_SetPropertyUint32(ctx, ret, (uint32_t)fi,",
                     "                             "
                     + _new_value(o.c_type, "found[fi]") + ");",
                     "}"]

    elif shape == "owned_string_release":
        rel = ent["release"]
        tail += ["char *owned = NULL;", "if (" + guard + ")",
                 "    owned = s->host." + m.name + "(" + ", ".join(args) + ");",
                 "if (owned) {", "    ret = JS_NewString(ctx, owned);",
                 # Guards on the release member ALONE, exactly as the lua
                 # backend does: a non-NULL result already implies have_host.
                 # Normalising it away crashes a partial host.
                 "    if (s->host." + rel + ") s->host." + rel
                 + "(s->host.user, owned);",
                 "} else {", "    ret = JS_NULL;", "}"]
    else:
        raise SystemExit("error: " + name + ": unknown shape " + repr(shape))

    # -- assemble -----------------------------------------------------------
    head = ["static JSValue js_jce_" + name
            + "(JSContext *ctx, JSValueConst this_val,",
            " " * (len("static JSValue js_jce_" + name) + 1)
            + "int argc, JSValueConst *argv)",
            "{",
            "    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);",
            "    JSValue ret = JS_UNDEFINED;"]
    for sname in strings:
        head.append("    const char *" + sname + " = NULL, *" + sname
                    + "_cs = NULL;")
    head += ["    (void)this_val; (void)argc; (void)argv;",
             "    if (!s) return JS_UNDEFINED;"]
    out = head + [("    " + ln if ln else "") for ln in body + tail]
    for sname in strings:
        out.append("    if (" + sname + "_cs) JS_FreeCString(ctx, " + sname
                   + "_cs);")
    out.append("    return ret;")
    if any("goto fail" in ln for ln in body):
        out.append("fail:")
        for sname in strings:
            out.append("    if (" + sname + "_cs) JS_FreeCString(ctx, " + sname
                       + "_cs);")
        out.append("    return JS_EXCEPTION;")
    out.append("}")
    return "\n".join(out) + "\n"


_C_BANNER = """\
/* jce_script_bindings_js.gen.c - GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * Source of truth: struct JceScriptHost in
 * engine/include/jce/middleware/script/jce_script.h (C types, arity,
 * parameter names) joined with the exposure decisions in
 * engine/src/middleware/script/script_exposure.json (name, shape, modifiers).
 *
 * This file is owned WHOLE by the generator.  There are no sentinel-delimited
 * regions: a tool that rewrites part of a file containing hand-written code
 * eventually eats the hand-written code.
 *
 * EVERY function guards the host member before calling it, and that is not
 * optional.  jce_script_vm_create copies min(host_size, sizeof s->host) over
 * a zeroed table, so a member an older caller's header did not have stays
 * NULL and calling it unguarded jumps through whatever followed the caller's
 * shorter object.
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
 * `s` is the CONTEXT OPAQUE, set once by js_create.  A binding reached with a
 * NULL opaque returns undefined rather than dereferencing it: that state is
 * unreachable through jce_script_vm_create, and a crash would be a worse
 * answer than a no-op if some future path ever made it reachable.
 */

#include "jce_script_bindings_js.gen.h"

#include <string.h>

"""


def _c_banner(man: dict) -> str:
    """The banner, with its two guard-spelling counts DERIVED.

    Written out by hand they are a claim nothing enforces: adding a second
    index_base binding would silently make the numbers false, and a reader who
    greps the first spelling concludes the file is broken."""
    folded = [e["name"] for e in man["expose"] if e.get("index_base")]
    plain = len(man["expose"]) - len(folded)
    return (_C_BANNER
            .replace("@PLAIN@", str(plain))
            .replace("@FOLDED_NAMES@", ", ".join("jce." + n for n in folded))
            .replace("@FOLDED@", str(len(folded)) + " function"
                                 + ("" if len(folded) == 1 else "s")))


def emit_bindings_c(members: list[HostMember], man: dict) -> str:
    by = {m.name: m for m in members}
    header_text = HEADER.read_text(encoding="utf-8")
    out = [_c_banner(man)]
    for e in man["expose"]:
        m = by[e["vtable"]]
        doc = e.get("doc")
        out.append("/* jce." + e["name"] + " - shape: " + e["shape"]
                   + (("\n * " + doc) if doc else "") + " */\n")
        out.append(_emit_one(m, e, header_text))
        out.append("\n")

    out.append("const char *const JCE_SCRIPT_JS_BINDING_NAMES[] = {\n")
    out += ['    "' + e["name"] + '",\n' for e in man["expose"]]
    out.append("};\n\n")

    out.append("static const JSCFunctionListEntry k_jce_fns[] = {\n")
    for e in man["expose"]:
        n = len(script_in_params(by[e["vtable"]], e))
        out.append('    JS_CFUNC_DEF("' + e["name"] + '", ' + str(n)
                   + ", js_jce_" + e["name"] + "),\n")
    out.append("};\n\n")

    out.append(
        "bool jce_script_js_install_bindings(JceJsScript *s)\n"
        "{\n"
        "    JSValue global, jce;\n"
        "    if (!s || !s->ctx) return false;\n"
        "    global = JS_GetGlobalObject(s->ctx);\n"
        "    jce = JS_NewObject(s->ctx);\n"
        "    if (JS_IsException(jce)) {\n"
        "        JS_FreeValue(s->ctx, jce);\n"
        "        JS_FreeValue(s->ctx, global);\n"
        "        return false;\n"
        "    }\n"
        "    JS_SetPropertyFunctionList(s->ctx, jce, k_jce_fns,\n"
        "                               (int)(sizeof k_jce_fns"
        " / sizeof k_jce_fns[0]));\n")
    for c in man["constants"]:
        out.append("    /* " + c["name"] + ": " + c["kind"]
                   + " - not a function, so no\n"
                   "     * JS_CFUNC_DEF and no registration regex can see it."
                   " */\n")
        out.append('    JS_SetPropertyStr(s->ctx, jce, "' + c["name"]
                   + '", JS_NULL);\n')
    out.append(
        '    JS_SetPropertyStr(s->ctx, global, "jce", jce);\n'
        "    JS_FreeValue(s->ctx, global);\n"
        "    return true;\n"
        "}\n")
    return "".join(out)


def emit_bindings_h(man: dict) -> str:
    n = len(man["expose"])
    return (
        "/* jce_script_bindings_js.gen.h - GENERATED. DO NOT EDIT.\n"
        " *   python tools/scriptgen/gen_script_bindings.py --write\n"
        " */\n"
        "#ifndef JCE_SCRIPT_BINDINGS_JS_GEN_H\n"
        "#define JCE_SCRIPT_BINDINGS_JS_GEN_H\n\n"
        '#include "jce_script_js_internal.h"\n\n'
        "#define JCE_SCRIPT_JS_BINDING_COUNT " + str(n) + "\n\n"
        "/* Registration order matches script_exposure.json's expose[] order. */\n"
        "extern const char *const JCE_SCRIPT_JS_BINDING_NAMES"
        "[JCE_SCRIPT_JS_BINDING_COUNT];\n\n"
        "/* Installs the global `jce` object into s->ctx, with every generated\n"
        " * binding and then the manifest's constants.  False only when the\n"
        " * context could not allocate the object. */\n"
        "bool jce_script_js_install_bindings(JceJsScript *s);\n\n"
        "#endif /* JCE_SCRIPT_BINDINGS_JS_GEN_H */\n")


def _render_h(members: list[HostMember], man: dict) -> str:
    """Artefact.render is uniformly render(members, man); the .h needs only the
    manifest, and adapting here beats special-casing the driver."""
    return emit_bindings_h(man)


# -- Registration parity, both directions ------------------------------------

_JS_CFUNC_DEF_RE = re.compile(r'JS_CFUNC_DEF\s*\(\s*"(\w+)"')
_JS_STATIC_FN_RE = re.compile(r"^static JSValue (js_jce_\w+)\(JSContext", re.M)


class JsBackend(ScriptBackend):
    name = "js"

    def artefacts(self, members: list[HostMember], man: dict) -> list[Artefact]:
        return [Artefact(GEN_C, emit_bindings_c), Artefact(GEN_H, _render_h)]

    def validate(self, members: list[HostMember], man: dict,
                 c_text: str) -> list[str]:
        """Registration parity for the JS installer, both directions.

        Against the FRESHLY RENDERED text, not the file on disk.  Reading the
        committed artefact was tried and is wrong twice over: `--write` runs
        validate BEFORE writing, so a drift this reported could never be fixed
        by the command it told you to run; and the committed copy is already
        guaranteed equal to the rendered one by diff_artefact, so checking it
        again proves nothing new.  Rendered, this catches what diff_artefact
        structurally cannot -- an EMITTER that drops an entry, where both
        sides agree and both are wrong.

        `c_text` is jce_script.c, the LUA translation unit, and says nothing
        about this backend."""
        problems: list[str] = []
        text = emit_bindings_c(members, man)
        registered = set(_JS_CFUNC_DEF_RE.findall(text))
        named = {e["name"] for e in man["expose"]}
        for n in sorted(registered - named):
            problems.append('JS_CFUNC_DEF("' + n + '") has no manifest entry')
        for n in sorted(named - registered):
            problems.append(
                "manifest entry '" + n + "' is never registered in "
                "jce_script_js_install_bindings()")
        # And the other direction: a function body with no table entry is dead
        # code a script can never reach, which is the "150/151 dead" shape.
        for fn in _JS_STATIC_FN_RE.findall(text):
            script_name = fn[len("js_jce_"):]
            if script_name not in named:
                problems.append(fn + " is a JSCFunction that no manifest entry "
                                     "claims")
        return problems


BACKEND = JsBackend()
