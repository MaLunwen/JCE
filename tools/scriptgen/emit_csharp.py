#!/usr/bin/env python3
"""
emit_csharp.py — the C# backend.  One of N; see scriptgen_core.py's docstring
for how to add another without touching this file or that one.

What lives here is everything that is true only because the target is C#:

  * the P/Invoke declarations.  C# needs NO SHIM: unlike Java, whose only FFI
    is JNI and which therefore carries a hand-written C file, C# calls the
    c_abi shared library directly.  So this backend's whole engine-facing
    surface is generated and there is no second, hand-written copy of it;
  * how a member with more than one output is returned.  A `fallible_out`
    becomes `bool TryX(..., out (T, T, T) value)` -- the Try pattern, which
    is what a C# caller expects and what the underlying C ABI actually is
    (a bool return with out parameters).  The manifest's `miss_value` is
    therefore NOT a sentinel here; it is a doc note, because `false` already
    distinguishes a miss and a sentinel would be a second way to say so;
  * `scripting/csharp/managed/JceScript/Jce.g.cs` -- this backend's single
    committed artefact, carrying both the raw Interop declarations and the
    friendly Jce surface;
  * registration parity for the P/Invoke surface, which is a set of
    [DllImport] declarations and would be invisible to every other backend's
    regex.

WHAT IS DELIBERATELY THE SAME AS THE OTHER BACKENDS, and not made idiomatic:
an `index_base` index stays 1-based.  It is a CONTRACT value -- the manifest
says the index is 1-based and emit_python keeps it 1-based -- and a language
that quietly re-based it would make the same script mean different things in
different languages.  It is documented on every entry that has one.
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
    Param,
    REPO_ROOT,
    ScriptBackend,
    capacity_param,
    flatten_pod,
    out_params,
    script_in_params,
)

GEN_CS = REPO_ROOT / "scripting/csharp/managed/JceScript/Jce.g.cs"

# The c_abi shared library, by the name a consumer's loader resolves.  Not a
# path: the .NET loader searches beside the assembly and then the OS rules,
# which is what puts jce_script_api.dll beside a shipped game's exe.
LIB = "jce_script_api"

# C type -> the blittable C# type used in a [DllImport] signature.
INTEROP = {
    "bool": "byte",
    "int": "int",
    "float": "float",
    "double": "double",
    "uint32_t": "uint",
    "uint64_t": "ulong",
    "JceScriptEntity": "uint",
}

# C type -> the type the friendly surface uses.
FRIENDLY = dict(INTEROP)
FRIENDLY["bool"] = "bool"


def _cs_name(snake: str) -> str:
    """get_position -> GetPosition.  PascalCase is not decoration: a C# API
    that exposed snake_case members would be the only one in the language a
    consumer ever sees, and every analyser in the ecosystem would flag it."""
    return "".join(p[:1].upper() + p[1:] for p in snake.split("_") if p)


def _arg_name(snake: str) -> str:
    """out_xyz -> outXyz.  camelCase for parameters, and a C# keyword gets an
    @ so a manifest that later names a parameter `event` still compiles."""
    parts = [p for p in snake.split("_") if p]
    if not parts:
        return "_"
    name = parts[0] + "".join(p[:1].upper() + p[1:] for p in parts[1:])
    if name in {"event", "base", "params", "ref", "out", "in", "object",
                "string", "int", "float", "double", "bool", "value", "class",
                "lock", "namespace", "operator", "checked", "fixed"}:
        return "@" + name
    return name


def _friendly(c_type: str) -> str:
    return FRIENDLY.get(c_type, "int")


def _method_name(ent: dict) -> str:
    """The public method name.  A fallible_out becomes Try* because that IS
    what it is in C# — a bool return with an out parameter — and the framework
    guideline every consumer knows says so.  Spelled ONCE here, so validate()
    and the emitter cannot disagree about what to look for."""
    n = _cs_name(ent["name"])
    return ("Try" + n) if ent["shape"] == "fallible_out" else n


# ── The POD out-structs, reconstructed from the flattened field list ─────────

def _pod_struct(name: str, fields: list[tuple[str, str]]) -> str:
    """Rebuild a blittable C# struct from flatten_pod's access expressions.

    `point[0]`, `point[1]`, `point[2]` came from `float point[3]`, so they are
    folded back into one `fixed float point[3]` -- which keeps the C# layout
    identical to the C one AND keeps the access expression the emitter uses
    (`hit.point[0]`) valid in both languages."""
    lines = ["    [StructLayout(LayoutKind.Sequential)]",
             "    internal unsafe struct " + name,
             "    {"]
    i = 0
    while i < len(fields):
        expr, ctype = fields[i]
        m = re.match(r"^(\w+)\[(\d+)\]$", expr)
        if m:
            base, _ = m.group(1), int(m.group(2))
            n = 0
            while (i + n < len(fields)
                   and fields[i + n][0].startswith(base + "[")):
                n += 1
            lines.append("        public fixed " + INTEROP.get(ctype, "int")
                         + " " + base + "[" + str(n) + "];")
            i += n
            continue
        lines.append("        public " + INTEROP.get(ctype, "int") + " "
                     + expr + ";")
        i += 1
    lines.append("    }")
    return "\n".join(lines)


# ── One entry ───────────────────────────────────────────────────────────────

def _interop_params(m: HostMember, ent: dict) -> list[str]:
    """The [DllImport] parameter list, INCLUDING bind_args.

    Derived from the host member's own declaration minus `void *user`, which
    the C ABI forwarder replaces with the opaque handle — the same
    transformation gen_script_c_abi.py performs, read from the same
    HostMember, so the two cannot disagree silently."""
    out = ["IntPtr api"]
    for p in m.params[1:]:
        if p.arity > 0:
            base = (p.c_type[len("const "):] if p.c_type.startswith("const ")
                    else p.c_type)
            out.append(INTEROP.get(base, "int") + "* " + _arg_name(p.name))
        elif p.arity < 0:
            base = p.c_type
            out.append((INTEROP.get(base, base)) + "* " + _arg_name(p.name))
        elif p.c_type == "const char *":
            out.append("[MarshalAs(UnmanagedType.LPUTF8Str)] string? "
                       + _arg_name(p.name))
        else:
            out.append(INTEROP.get(p.c_type, "int") + " " + _arg_name(p.name))
    return out


def _interop_return(m: HostMember) -> str:
    if m.ret == "void":
        return "void"
    if m.ret == "bool":
        return "byte"
    if m.ret in ("const char *", "char *"):
        return "IntPtr"
    return INTEROP.get(m.ret, "int")


def _emit_interop(m: HostMember, ent: dict) -> str:
    return ("        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]\n"
            "        internal static extern " + _interop_return(m) + " "
            + "jce_script_api_" + ent["name"] + "(\n            "
            + ",\n            ".join(_interop_params(m, ent)) + ");\n")


def _doc(ent: dict, extra: list[str]) -> str:
    lines = []
    doc = (ent.get("doc") or "").strip()
    if doc:
        for ln in doc.split("\n"):
            lines.append("        /// " + ln.strip())
    for e in extra:
        lines.append("        /// <para>" + e + "</para>")
    if not lines:
        return ""
    return ("        /// <summary>\n" + "\n".join(lines)
            + "\n        /// </summary>\n")


def _notes(m: HostMember, ent: dict) -> list[str]:
    notes: list[str] = []
    if ent.get("bind_args"):
        notes.append("The C ABI supplies "
                     + ", ".join(str(k) + "=" + str(v)
                                 for k, v in ent["bind_args"].items())
                     + " (manifest bind_args), so it is not an argument.")
    if ent.get("index_base"):
        notes.append("index is " + str(ent["index_base"]) + "-based, as in "
                     "Lua and Python; below " + str(ent["index_base"])
                     + " answers without calling the host. Kept 1-based ON "
                     "PURPOSE: it is a contract value, not a rendering.")
    if ent.get("absent_value") is not None:
        notes.append("With the host member absent the C ABI answers the "
                     "manifest's absent_value, not zero.")
    if "clamp_min" in ent:
        notes.append("The C ABI clamps the result to a minimum of "
                     + str(ent["clamp_min"]) + ".")
    if ent.get("miss_value") is not None:
        notes.append("Lua answers " + repr(ent["miss_value"]) + " on a miss "
                     "because it must push something; this returns false and "
                     "leaves the out value zeroed. Same information, one way "
                     "to test it.")
    return notes


def _emit_friendly(m: HostMember, ent: dict, header_text: str,
                   pods: dict) -> str:
    name, shape = ent["name"], ent["shape"]
    bind = ent.get("bind_args") or {}
    ins = script_in_params(m, ent)
    outs = out_params(m)
    cap = ent.get("out_capacity")

    sig: list[str] = []
    pre: list[str] = []
    call: list[str] = ["_api"]
    fixes: list[tuple[str, str]] = []      # (pointer name, source expression)

    # The C ABI takes every in-param, including the bound ones.
    bound = {}
    for p in m.params[1:]:
        if p.arity != 0:
            continue
        if p.name in bind:
            bound[p.name] = bind[p.name]

    for p in ins:
        if p.arity > 0:
            sig.append("ReadOnlySpan<float> " + _arg_name(p.name))
            pre.append("            if (" + _arg_name(p.name) + ".Length < "
                       + str(p.arity) + ")")
            pre.append('                throw new ArgumentException("needs '
                       + str(p.arity) + ' values", nameof('
                       + _arg_name(p.name) + "));")
            continue
        if p.c_type == "const char *":
            sig.append("string? " + _arg_name(p.name))
        else:
            sig.append(_friendly(p.c_type) + " " + _arg_name(p.name))

    # Build the call arguments in the C ABI's own parameter order.
    cap_param = capacity_param(m, shape)
    for p in m.params[1:]:
        if p.arity > 0 and p.c_type.startswith("const "):
            call.append("p_" + p.name)
            continue
        if p.arity != 0:
            continue                      # an out param, appended below
        if cap_param is not None and p.name == cap_param.name:
            # The `int max` is supplied by the GENERATOR from out_capacity and
            # appended after `found` below; script_in_params already excludes
            # it from the signature, so leaving it here would emit a reference
            # to a parameter that does not exist AND pass the capacity twice.
            continue
        if p.name in bound:
            v = bound[p.name]
            lit = ("1" if v is True else "0" if v is False
                   else ('"' + str(v) + '"' if isinstance(v, str) else str(v)))
            if p.c_type == "bool":
                call.append("(byte)" + ("1" if v is True else "0"))
            else:
                call.append(lit)
            continue
        a = _arg_name(p.name)
        if p.c_type == "bool":
            call.append("(byte)(" + a + " ? 1 : 0)")
        elif ent.get("index_base") and p is ins[0]:
            call.append("(" + INTEROP.get(p.c_type, "int") + ")(" + a + " - "
                        + str(ent["index_base"]) + ")")
        else:
            call.append(a)

    for p in ins:
        if p.arity > 0:
            fixes.append(("p_" + p.name, _arg_name(p.name)))

    notes = _notes(m, ent)
    body: list[str] = []
    ret_type = "void"
    fn = "jce_script_api_" + name

    if shape == "void_call":
        body = ["            " + fn + "(" + ", ".join(call) + ");"]

    elif shape == "value_return":
        if m.ret in ("const char *", "char *"):
            ret_type = "string"
            body = ["            IntPtr s = " + fn + "(" + ", ".join(call) + ");",
                    "            return s == IntPtr.Zero ? string.Empty",
                    "                : (Marshal.PtrToStringUTF8(s) ?? string.Empty);"]
        elif m.ret == "bool":
            ret_type = "bool"
            body = ["            return " + fn + "(" + ", ".join(call)
                    + ") != 0;"]
        else:
            ret_type = _friendly(m.ret)
            body = ["            return " + fn + "(" + ", ".join(call) + ");"]

    elif shape == "void_out_array":
        o = outs[0]
        t = _friendly(o.c_type)
        if o.arity == 1:
            ret_type = t
            body = ["            " + t + " o0 = default;",
                    "            " + fn + "(" + ", ".join(call + ["&o0"]) + ");",
                    "            return o0;"]
        else:
            ret_type = "(" + ", ".join([t] * o.arity) + ")"
            body = ["            " + t + "* o = stackalloc " + t + "["
                    + str(o.arity) + "];",
                    "            " + fn + "(" + ", ".join(call + ["o"]) + ");",
                    "            return (" + ", ".join("o[" + str(i) + "]"
                                                       for i in range(o.arity))
                    + ");"]

    elif shape == "fallible_out":
        slots: list[tuple[str, str]] = []
        decl: list[str] = []
        args = list(call)
        for o in outs:
            if o.arity > 0:
                t = _friendly(o.c_type)
                decl.append("            " + t + "* " + _arg_name(o.name)
                            + " = stackalloc " + t + "[" + str(o.arity) + "];")
                args.append(_arg_name(o.name))
                slots += [(t, _arg_name(o.name) + "[" + str(i) + "]")
                          for i in range(o.arity)]
            elif o.c_type not in INTEROP:
                pods.setdefault(o.c_type, flatten_pod(header_text, o.c_type))
                decl.append("            " + o.c_type + " " + _arg_name(o.name)
                            + " = default;")
                args.append("&" + _arg_name(o.name))
                slots += [(_friendly(t), _arg_name(o.name) + "." + e)
                          for e, t in pods[o.c_type]]
            else:
                # The DECLARED local is the INTEROP type, not the friendly
                # one: a `bool` out parameter is byte* across the boundary
                # (C# bool is not blittable), and declaring it `bool` gives
                # "cannot convert bool* to byte*".  The compiler catches it
                # here; in a language with a looser boundary the same mistake
                # is a silent one-byte mismatch.
                it = INTEROP.get(o.c_type, "int")
                t = _friendly(o.c_type)
                decl.append("            " + it + " " + _arg_name(o.name)
                            + " = default;")
                args.append("&" + _arg_name(o.name))
                slots.append((t, _arg_name(o.name)
                              + (" != 0" if o.c_type == "bool" else "")))
        vt = (slots[0][0] if len(slots) == 1
              else "(" + ", ".join(t for t, _ in slots) + ")")
        ret_type = "bool"
        sig.append("out " + vt + " value")
        guard = []
        if ent.get("index_base"):
            guard = ["            if (" + _arg_name(ins[0].name) + " < "
                     + str(ent["index_base"]) + ") { value = default; "
                     "return false; }"]
        body = guard + decl + [
            "            if (" + fn + "(" + ", ".join(args) + ") == 0)",
            "            { value = default; return false; }",
            "            value = " + (slots[0][1] if len(slots) == 1
                                       else "(" + ", ".join(e for _, e in slots)
                                            + ")") + ";",
            "            return true;"]

    elif shape in ("first_and_count", "entity_table"):
        o = outs[0]
        t = _friendly(o.c_type)
        decl = ["            " + t + "* found = stackalloc " + t + "["
                + str(cap) + "];",
                "            int n = " + fn + "("
                + ", ".join(call + ["found", str(cap)]) + ");"]
        if shape == "first_and_count":
            ret_type = "(" + t + "? First, int Count)"
            body = decl + ["            return (n > 0 ? found[0] : (" + t
                           + "?)null, n);"]
        else:
            ret_type = t + "[]"
            body = decl + ["            if (n <= 0) return Array.Empty<" + t
                           + ">();",
                           "            var r = new " + t + "[n];",
                           "            for (int i = 0; i < n; ++i) r[i] = found[i];",
                           "            return r;"]

    elif shape == "owned_string_release":
        rel = ent["release"]
        ret_type = "string?"
        body = ["            IntPtr s = " + fn + "(" + ", ".join(call) + ");",
                "            if (s == IntPtr.Zero) return null;",
                "            string r = Marshal.PtrToStringUTF8(s) ?? string.Empty;",
                "            jce_script_api_" + rel + "(_api, s);",
                "            return r;"]
    else:
        raise SystemExit("error: " + name + ": unknown shape " + repr(shape))

    out = [_doc(ent, notes)]
    out.append("        public static " + ret_type + " " + _method_name(ent)
               + "(" + ", ".join(sig) + ")\n        {")
    out += pre
    if fixes:
        for pn, src in fixes:
            out.append("            fixed (float* " + pn + " = " + src + ")")
        out.append("            {")
        out += ["    " + b for b in body]
        out.append("            }")
    else:
        out += body
    out.append("        }\n")
    return "\n".join(out)


_BANNER = '''\
// Jce.g.cs - GENERATED. DO NOT EDIT.
//
//   python tools/scriptgen/gen_script_bindings.py --write
//
// Source of truth: struct JceScriptHost in
// engine/include/jce/middleware/script/jce_script.h (C types, arity, parameter
// names) joined with the exposure decisions in
// engine/src/middleware/script/script_exposure.json (name, shape, modifiers).
//
// NO SHIM.  Java carries a hand-written JNI C file because JNI is Java's only
// FFI; C# calls the c_abi shared library directly, so this generated file IS
// the whole engine-facing surface of the C# backend and there is no second,
// hand-written copy of it to drift.
//
// A `fallible_out` is rendered as the Try pattern, which is what the C ABI
// actually is - a bool return with out parameters - rather than as a nullable
// value.  The manifest's `miss_value` is therefore a doc note here and not a
// sentinel: `false` already distinguishes a miss, and a sentinel beside it
// would be a second way to ask the same question.
'''


def emit_cs(members: list[HostMember], man: dict) -> str:
    by = {m.name: m for m in members}
    header_text = HEADER.read_text(encoding="utf-8")
    pods: dict = {}

    friendly: list[str] = []
    interop: list[str] = []
    for e in man["expose"]:
        m = by[e["vtable"]]
        friendly.append(_emit_friendly(m, e, header_text, pods))
        interop.append(_emit_interop(m, e))
    # The release members owned_string_release calls are C ABI entries too and
    # are NOT in expose[]; without them the generated file would not compile.
    seen_release: set = set()
    for e in man["expose"]:
        rel = e.get("release")
        # DEDUPED: two owned_string_release entries share one release member
        # (both json-returning calls free through json_free), and declaring it
        # twice is CS0111 rather than a harmless repeat.
        if not rel or rel in seen_release:
            continue
        seen_release.add(rel)
        m = by.get(rel)
        if m is None:
            continue
        interop.append(
            "        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]\n"
            "        internal static extern void jce_script_api_" + rel
            + "(IntPtr api, IntPtr owned);\n")

    # #nullable enable, EXPLICITLY.  Roslyn treats a file named *.g.cs as
    # auto-generated and switches the project's <Nullable>enable</Nullable> OFF
    # for it, so a `string?` here is CS8669 rather than an annotation.  The
    # project setting is not inherited; this line is what turns it back on.
    out = [_BANNER, "\n#nullable enable\n",
           "using System.Runtime.InteropServices;\n\n",
           "namespace JceScript;\n\n"]

    for pod, fields in sorted(pods.items()):
        out.append(_pod_struct(pod, fields) + "\n\n")

    out.append("internal static unsafe partial class Interop\n{\n")
    out.append('    private const string Lib = "' + LIB + '";\n\n')
    out.append("    /// <summary>The ABI version this binding was generated\n"
               "    /// against.  open() REFUSES a library older than it rather\n"
               "    /// than degrading, because a newer binding would call entries\n"
               "    /// that do not exist.  From the manifest, so it cannot drift.\n"
               "    /// </summary>\n"
               "    internal const uint ScriptApiMin = "
               + str(man["script_api_version"]) + "u;\n\n")
    out.append("    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]\n"
               "    internal static extern IntPtr jce_script_api_open(\n"
               "        IntPtr host, nuint hostSize, uint scriptApiMin);\n\n"
               "    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]\n"
               "    internal static extern void jce_script_api_close(IntPtr api);\n\n")
    out.append("    internal static class Fn\n    {\n")
    out += interop
    out.append("    }\n}\n\n")

    out.append(
        "/// <summary>The engine, as a C# script sees it. "
        + str(len(man["expose"])) + " calls, generated from the same manifest "
        "the other five languages read.</summary>\n"
        "public static unsafe class Jce\n{\n"
        "    private static IntPtr _api;\n\n"
        "    /// <summary>Point the surface at a VM's C ABI handle.  Called by\n"
        "    /// JceEntityScript.Bind, never by a script.  A process hosts one\n"
        "    /// engine, so one handle is enough — and a per-instance handle\n"
        "    /// would be one more thing to get wrong at reload.</summary>\n"
        "    internal static void Use(IntPtr api) { if (api != IntPtr.Zero) _api = api; }\n\n"
        "    /// <summary>True once a VM has opened the C ABI.  Every call\n"
        "    /// below is a no-op answering its zero value until then, which is\n"
        "    /// the same thing a NULL host member does.</summary>\n"
        "    public static bool IsOpen => _api != IntPtr.Zero;\n\n")
    out.append("\n".join(friendly))
    out.append("}\n")

    text = "".join(out)
    # The friendly bodies call the interop declarations unqualified.
    return text.replace("            jce_script_api_",
                        "            Interop.Fn.jce_script_api_") \
               .replace("            IntPtr s = jce_script_api_",
                        "            IntPtr s = Interop.Fn.jce_script_api_") \
               .replace("            if (jce_script_api_",
                        "            if (Interop.Fn.jce_script_api_") \
               .replace("            int n = jce_script_api_",
                        "            int n = Interop.Fn.jce_script_api_") \
               .replace("            return jce_script_api_",
                        "            return Interop.Fn.jce_script_api_")


_DLLIMPORT_RE = re.compile(r"internal static extern [\w\*\?]+ "
                           r"jce_script_api_(\w+)\(")
# Leading whitespace, not a fixed four: the emitted bodies carry their own
# indent and the assembler dedents them once, so pinning a column here would
# make this regex a second, silent opinion about formatting.
_PUBLIC_RE = re.compile(r"^\s+public static [^\n]*?\b(\w+)\(", re.M)


class CSharpBackend(ScriptBackend):
    name = "csharp"

    def artefacts(self, members: list[HostMember], man: dict) -> list[Artefact]:
        return [Artefact(GEN_CS, emit_cs)]

    def validate(self, members: list[HostMember], man: dict,
                 c_text: str) -> list[str]:
        """Both directions of registration parity for the P/Invoke surface.

        Against the FRESHLY RENDERED text, not the file on disk.  Reading the
        committed artefact was tried and is wrong twice over: `--write` runs
        validate BEFORE writing, so a drift this reported could never be
        fixed by the command it told you to run; and the committed copy is
        already guaranteed equal to the rendered one by diff_artefact, so
        checking it a second time proves nothing new.  Rendered, this catches
        what diff_artefact structurally cannot -- an EMITTER that drops an
        entry, where both sides agree and both are wrong.

        `c_text` is jce_script.c, the LUA translation unit, and says nothing
        about this backend."""
        problems: list[str] = []
        text = emit_cs(members, man)
        declared = set(_DLLIMPORT_RE.findall(text))
        named = {e["name"] for e in man["expose"]}
        releases = {e["release"] for e in man["expose"] if e.get("release")}
        for n in sorted(declared - named - releases - {"open", "close"}):
            problems.append("[DllImport] jce_script_api_" + n
                            + " has no manifest entry")
        for n in sorted(named - declared):
            problems.append("manifest entry '" + n + "' has no [DllImport] in "
                            "Jce.g.cs")
        # And that every manifest entry reached the PUBLIC surface: a
        # declaration nothing calls is a binding no script can use.
        public = set(_PUBLIC_RE.findall(text))
        for e in man["expose"]:
            if _method_name(e) not in public:
                problems.append("manifest entry '" + e["name"] + "' has no "
                                "public Jce." + _method_name(e) + "()")
        return problems


BACKEND = CSharpBackend()
