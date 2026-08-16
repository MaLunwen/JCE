#!/usr/bin/env python3
"""
gen_script_c_abi.py — the C ABI backend of the scripting surface.

  (default) --check   regenerate in memory, diff against the committed output,
                      run every gate condition, exit non-zero on failure
  --write             emit the five artefacts

Same one-tool shape as gen_script_bindings.py: the generator IS the checker.
A separate checker would be a second implementation of the same contract, and
two implementations drift.

WHAT CROSSES THE BOUNDARY, and why it is not the 78 Lua entry points
-------------------------------------------------------------------
Owner decision 7 says the shared library exports "the 78 scripting entry
points".  Measured, that phrase cannot be a C ABI as written:

  * all 71 generated entry points are `static int l_jce_<name>(lua_State *L)`
    in jce_script_bindings.gen.c -- `static`, so not linkable at all, and
    taking a `lua_State *`, which no ctypes / JNA / JNI caller can construct;
  * the 7 hand-written ones are the same shape.

So the ABI exports the OTHER half of each entry point: the JceScriptHost
member it calls, flattened into a plain C function.  That is what the manifest
already describes in `c_signature`, and it is the half a foreign language can
actually call.  One entry point per manifest `expose` entry, same name, same
semantics, `void *user` replaced by an opaque `JceScriptApi *`.

The 7 `hand_written` entries are EXCLUDED as a class, by rule and not by hand:
three are Lua-VM machinery (`lua_newthread`, `lua_yield`) that has no meaning
without a Lua VM; `asset_read_text` / `asset_read_json` carry SANDBOX POLICY
(path validation, the 1 MiB cap) that lives in `static` functions inside
jce_script.c -- re-implementing it here would be a second copy of a security
decision, and exporting the raw `read_file` member instead would be the
sandbox escape the manifest calls P0-2; `log` and `play_sound` are policy and
arity dispatch respectively.  The emitted header names all seven with the
manifest's own `reason` text, so the gap is documented rather than silent.

`JceScriptVM` is NOT exported: it does not exist.  A repo-wide grep for it
over engine/, editor/, tests/, tools/ returns nothing, and the design's 6.1
places it in a later batch whose engine-side consumer this step is forbidden
to add ("the engine itself stays static and unchanged").  Exporting an empty
vtable nothing implements would be a contract declared before anything
enforces it.

VERSIONING
----------
`jce_script_api_version()` returns the manifest's `script_api_version`, and
`jce_script_api_open()` takes the caller's `script_api_min` plus the caller's
`sizeof(JceScriptHost)`.  What a mismatched loader sees is spelled out in the
emitted header, beside the two functions.

Its tests: test_script_c_abi_gate.py
           tests/scripting/c_abi/test_jce_script_api_abi.c (the symbol table)
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
# cdecl.py sits beside this file, not under tools/audit/ where it was born:
# tools/audit/ is no longer tracked, and the ONE C-declaration parser this
# repo has (5e232821: "one C-declaration parser, not two") is a generator
# dependency, not an audit tool.  Its consumers are all in this directory.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from cdecl import norm_ws, split_params_raw                # noqa: E402

API_JSON = REPO_ROOT / "contracts/script-api.json"

OUT_DIR = REPO_ROOT / "scripting/c_abi"
PUBLIC_H = OUT_DIR / "include/jce/script_api/jce_script_api.h"
GEN_C = OUT_DIR / "src/jce_script_api.gen.c"
DEF_FILE = OUT_DIR / "src/jce_script_api.def"
MAP_FILE = OUT_DIR / "src/jce_script_api.map"
EXPORTS_H = OUT_DIR / "src/jce_script_api_exports.gen.h"

PREFIX = "jce_script_api_"

# The three entry points that are not a manifest entry.  They are listed HERE,
# in the one place that also emits the .def, the version script, the header and
# the expected-export table, so "the export set" has exactly one definition.
# Adding a fourth by hand to any single artefact makes the check mode red.
META = ("version", "open", "close")

_SIG_RE = re.compile(r"^(?P<ret>.+?)\s*\(\s*\*\s*\)\s*\((?P<params>.*)\)$", re.S)
_PNAME_RE = re.compile(r"(?P<name>[A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*$")
_EXPORT_RE = re.compile(r"^jce_script_api_[a-z][a-z0-9_]*$")


# --------------------------------------------------------------------- #
#  Front end: script-api.json is the ONLY input.                         #
# --------------------------------------------------------------------- #

def load_api() -> dict:
    return json.loads(API_JSON.read_text(encoding="utf-8"))


def split_signature(sig: str) -> tuple[str, list[str]]:
    """`RET (*)(void *user, ...)` -> (RET, [parameter declarations after user]).

    Textual passthrough on purpose.  The forwarder re-declares each parameter
    exactly as the vtable declares it and passes it straight through, so this
    needs no notion of in/out/const: the semantics the Lua backend must
    understand in order to marshal are precisely the semantics a C ABI does not
    have to re-derive.  Out-parameter names come from the manifest's own
    `out_params`, not from a second reading of the type.
    """
    m = _SIG_RE.match(norm_ws(sig))
    if not m:
        raise ValueError(f"unparsable c_signature: {sig!r}")
    parts = [norm_ws(p) for p in split_params_raw(m.group("params"))]
    parts = [p for p in parts if p not in ("", "void")]
    if not parts or parts[0] != "void *user":
        raise ValueError(f"c_signature does not start with `void *user`: {sig!r}")
    return norm_ws(m.group("ret")), parts[1:]


def param_name(decl: str) -> str:
    m = _PNAME_RE.search(decl)
    if not m:
        raise ValueError(f"parameter has no name: {decl!r}")
    return m.group("name")


def entry_names(api: dict) -> list[str]:
    """The exported name of every manifest entry, in manifest order."""
    return [PREFIX + e["name"] for e in api["expose"]]


def export_names(api: dict) -> list[str]:
    """THE export set, sorted.  Every artefact is emitted from this one list."""
    return sorted(entry_names(api) + [PREFIX + m for m in META])


def tail_member(api: dict) -> tuple[str, str]:
    """(host member, exported entry) with the highest vtable_index.

    Used only to give the short-host test a truncation point that MOVES as the
    struct grows, so the test cannot quietly start withholding nothing.  This
    is not the slot-indexed dispatch script-api.json's `_note` forbids: the
    index picks a member NAME for a compile-time offsetof, and nothing keys a
    call off a slot number.
    """
    e = max(api["expose"], key=lambda x: x["vtable_index"])
    return e["vtable"], PREFIX + e["name"]


# --------------------------------------------------------------------- #
#  The gate.                                                             #
# --------------------------------------------------------------------- #

def validate(api: dict) -> list[str]:
    problems: list[str] = []
    totals = api.get("declared_totals", {})

    # G1 -- the gate must prove it read the manifest, not an empty list.
    if totals.get("expose") != len(api["expose"]):
        problems.append(
            f"declared_totals.expose = {totals.get('expose')} but the manifest "
            f"carries {len(api['expose'])} entries -- a run that checked "
            f"nothing must fail, not print a cheerful OK")
    if totals.get("hand_written") != len(api["hand_written"]):
        problems.append(
            f"declared_totals.hand_written = {totals.get('hand_written')} but "
            f"the manifest carries {len(api['hand_written'])}")

    # G2 -- every export name is legal, unique, and none collides with META.
    names = entry_names(api) + [PREFIX + m for m in META]
    for n in names:
        if not _EXPORT_RE.match(n):
            problems.append(f"export '{n}' is not a legal jce_script_api_* name")
    for n in sorted({n for n in names if names.count(n) > 1}):
        problems.append(
            f"export '{n}' is declared twice -- a manifest entry named "
            f"{', '.join(META)} would silently replace a meta entry point")

    # G3 -- every signature parses, and the one shape this backend transforms
    #       carries what the transform needs.
    for e in api["expose"]:
        try:
            _, params = split_signature(e["c_signature"])
            sig_names = {param_name(p) for p in params}
        except ValueError as exc:
            problems.append(f"expose[{e['name']}]: {exc}")
            continue
        if e["shape"] == "owned_string_release" and not e.get("release"):
            problems.append(
                f"expose[{e['name']}]: shape owned_string_release with no "
                f"`release` member -- the forwarder would leak the host's string")
        # G4 -- the manifest's two halves agree: every out parameter it names
        #       is a parameter of the signature it also carries.
        for op in e.get("out_params", []):
            if op["name"] not in sig_names:
                problems.append(
                    f"expose[{e['name']}]: out_params names '{op['name']}', "
                    f"which is not a parameter of its own c_signature")
        if e["since"] > api["script_api_version"]:
            problems.append(
                f"expose[{e['name']}]: since={e['since']} is newer than "
                f"script_api_version={api['script_api_version']}")

    # G5 -- the exclusion of the hand-written entries stays DOCUMENTED.  The
    #       emitted header prints these reasons; an entry that lost its reason
    #       would be missing from the ABI with nothing left to say why.
    for e in api["hand_written"]:
        if not e.get("reason"):
            problems.append(
                f"hand_written[{e['name']}] has no `reason` -- it is excluded "
                f"from the C ABI and the header has nothing to print")
    return problems


# --------------------------------------------------------------------- #
#  Emitters.                                                             #
# --------------------------------------------------------------------- #

def _wrap(text: str, prefix: str, width: int = 78) -> list[str]:
    """Word-wrap `text` into comment lines, each starting with `prefix`."""
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


def _lit(v, ret: str) -> str:
    """A manifest scalar as a C literal of the member's return type."""
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, float):
        return repr(v) + ("f" if ret == "float" else "")
    return str(v)


def _absent_return(e: dict, ret: str) -> str:
    """What the entry returns when the host member is absent.

    NOT always the zero of the type.  The manifest carries `absent_value` for
    the two entries where zero is a wrong answer -- music_request_transition
    returns a negative playhead on a miss, and `tr` degrades to KEY
    PASSTHROUGH, which is what jce_loc_t does for a missing key and the reason
    an unlocalized build shows keys instead of empty strings.  A C ABI that
    returned 0 / NULL there would be a different surface from the Lua one.
    """
    absent = e.get("absent_value")
    if isinstance(absent, dict):                 # {"param": "key"} -> echo it
        return f"return {absent['param']};"
    if absent is not None:
        # clamp_min applies to the absent value too: the Lua emitter clamps `v`
        # after choosing it, so a defaulted value below the floor would be
        # lifted there and must be lifted here.
        if "clamp_min" in e and isinstance(absent, (int, float)) \
                and not isinstance(absent, bool):
            absent = max(absent, e["clamp_min"])
        return f"return {_lit(absent, ret)};"
    if ret == "void":
        return "return;"
    if ret == "bool":
        return "return false;"
    if ret == "float":
        return "return 0.0f;"
    if ret == "double":
        return "return 0.0;"
    if ret.endswith("*"):
        return "return NULL;"
    return "return 0;"


def _zero_outs(e: dict, indent: str) -> list[str]:
    out = []
    for op in e.get("out_params", []):
        n = op["name"]
        if op["arity"] > 0:
            out.append(f"{indent}if ({n}) memset({n}, 0, "
                       f"{op['arity']} * sizeof {n}[0]);")
        else:
            out.append(f"{indent}if ({n}) memset({n}, 0, sizeof *{n});")
    return out


def _signature_text(ret: str, fn: str, decl_params: list[str]) -> str:
    sep = "" if ret.endswith("*") else " "
    head = f"JCE_SCRIPT_API {ret}{sep}{fn}("
    one = head + ", ".join(decl_params) + ")"
    if len(one) <= 78:
        return one
    pad = " " * len(head)
    lines: list[str] = []
    cur = head + decl_params[0]
    for p in decl_params[1:]:
        if len(cur) + 2 + len(p) > 78:
            lines.append(cur + ",")
            cur = pad + p
        else:
            cur += ", " + p
    lines.append(cur + ")")
    return "\n".join(lines)


def _entry_decl(e: dict) -> tuple[str, str, list[str], list[str]]:
    """(return type, exported name, declared parameters, call arguments).

    `bind_args` is applied here, and it is not cosmetic: jump_pressed, sprint
    and attack_pressed are three manifest entries over ONE host member
    (`input_button`), separated only by the button number the binding supplies.
    Passing that number through instead would make the three exports the same
    function and let a caller ask for button 99 -- a surface WIDER than the
    scripting surface, which is the one thing this library must not be.
    """
    ret, params = split_signature(e["c_signature"])
    fn = PREFIX + e["name"]
    bind = e.get("bind_args") or {}
    c_type = {p["name"]: p["c_type"] for p in e.get("params", [])}
    kept = [p for p in params if param_name(p) not in bind]
    decl = ["JceScriptApi *api"] + kept
    args = ["api->host.user"]
    for p in params:
        n = param_name(p)
        args.append(f"({c_type[n]}){_lit(bind[n], c_type[n])}" if n in bind else n)
    if e["shape"] == "owned_string_release":
        # The host returns a heap string it owns and releases through another
        # member.  Handing that pointer to Python or Java would put a free()
        # obligation on the far side of an ABI; the forwarder copies into the
        # caller's buffer and releases before returning, so no ownership ever
        # crosses.  Returns the length, or -1 when absent.
        ret = "int"
        decl = decl + ["char *out", "int out_cap"]
    return ret, fn, decl, args


def emit_header(api: dict) -> str:
    L: list[str] = []
    A = L.append
    A("/* jce_script_api.h -- GENERATED. DO NOT EDIT.")
    A(" *")
    A(" *   python tools/scriptgen/gen_script_c_abi.py --write")
    A(" *")
    A(" * The script-facing C ABI: every entry of the scripting surface in")
    A(" * contracts/script-api.json, flattened into plain C so that a")
    A(" * ctypes / JNA / JNI / C++ binding can call it.  The Lua entry points")
    A(" * themselves cannot be this ABI -- they are `static int (lua_State *)`.")
    A(" * What is exported is the JceScriptHost member each one calls, with")
    A(" * `void *user` replaced by an opaque handle.")
    A(" *")
    A(" * THE EXPORTED SET IS THE WHOLE SURFACE.  The shared library exports")
    A(" * these names and nothing else -- no engine symbol, no allocator, no")
    A(" * asset reader.  That is the sandbox property, and it is asserted at the")
    A(" * symbol table by tests/scripting/c_abi/test_jce_script_api_abi.c.")
    A(" *")
    A(" * ABSENT MEMBERS.  A host callback may be NULL, and a host built against")
    A(" * an older header is shorter than this one -- jce_script_api_open copies")
    A(" * min(caller, engine) over a zeroed table, so its missing members stay")
    A(" * NULL.  Either way the entry point does nothing, zero-fills every out")
    A(" * parameter and returns false / 0 / NULL / -1.  That is what the Lua")
    A(" * binding does when it pushes nil.  Out parameters are meaningful only")
    A(" * when the call returns true (always, for the void ones).")
    A(" */")
    A("#ifndef JCE_SCRIPT_API_H")
    A("#define JCE_SCRIPT_API_H")
    A("")
    A("/* JceScriptHost, JceScriptEntity, JceScriptRaycastHit: this ABI restates")
    A(" * no engine type.  Consumers that compile C need engine/include on the")
    A(" * include path; ctypes and JNA consumers do not read this header at all")
    A(" * -- they read contracts/script-api.json. */")
    A("#include <jce/middleware/script/jce_script.h>")
    A("")
    A("#include <stdbool.h>")
    A("#include <stddef.h>")
    A("#include <stdint.h>")
    A("")
    A("#ifdef __cplusplus")
    A('extern "C" {')
    A("#endif")
    A("")
    A("/* On Windows the export set is jce_script_api.def, generated beside this")
    A(" * header from the same manifest: ONE list decides what leaves the DLL,")
    A(" * and an accidental __declspec(dllexport) elsewhere shows up as an extra")
    A(" * export the symbol-table test names.  On ELF/Mach-O the library is built")
    A(" * -fvisibility=hidden and this attribute is what opts a symbol back in. */")
    A("#if defined(JCE_SCRIPT_API_BUILD) && (defined(__GNUC__) || defined(__clang__))")
    A('#  define JCE_SCRIPT_API __attribute__((visibility("default")))')
    A("#else")
    A("#  define JCE_SCRIPT_API")
    A("#endif")
    A("")
    A("/* The scripting surface's version.  Monotonic, and NOT the engine ABI")
    A(" * version -- that handshake is jce_api_version(), which this library")
    A(" * deliberately does not export, because a binding never calls the engine")
    A(" * directly.")
    A(" *")
    A(" * Entries are never renamed or removed, so:")
    A(" *   older binding, newer library -> loads and runs.  It is a strict")
    A(" *     subset: the binding declares no entry whose `since` exceeds the")
    A(" *     script_api_version it was generated from.")
    A(" *   newer binding, older library -> jce_script_api_open() returns NULL.")
    A(" *     It cannot degrade: the binding would call entries that do not")
    A(" *     exist.  The loader reports both numbers -- its own script_api_min")
    A(" *     and jce_script_api_version(). */")
    A(f"#define JCE_SCRIPT_API_VERSION {api['script_api_version']}u")
    A("")
    A("/* Entry points that are a manifest entry; the library exports these plus")
    A(f" * the {len(META)} meta entry points below. */")
    A(f"#define JCE_SCRIPT_API_ENTRY_COUNT {len(api['expose'])}")
    A("")
    A("/* Opaque: the library owns the host copy, the caller owns nothing. */")
    A("typedef struct JceScriptApi JceScriptApi;")
    A("")
    A("/* The script_api_version this library implements. */")
    A("JCE_SCRIPT_API uint32_t jce_script_api_version(void);")
    A("")
    A("/* Bind to a host.  `host_size` is the CALLER's sizeof(JceScriptHost) --")
    A(" * always from sizeof, never summed -- and `script_api_min` is the")
    A(" * script_api_version the caller was generated from.")
    A(" *")
    A(" * Returns NULL when `host` is NULL, `host_size` is 0, or `script_api_min`")
    A(" * is newer than JCE_SCRIPT_API_VERSION.  The host table is COPIED, so the")
    A(" * caller may keep it on the stack. */")
    A("JCE_SCRIPT_API JceScriptApi *jce_script_api_open(const JceScriptHost *host,")
    A("                                                 size_t host_size,")
    A("                                                 uint32_t script_api_min);")
    A("")
    A("/* Releases the handle.  NULL is a no-op. */")
    A("JCE_SCRIPT_API void jce_script_api_close(JceScriptApi *api);")
    A("")
    A("/* ------------------------------------------------------------------ *")
    A(" *  NOT in this ABI: the manifest's hand-written entries.")
    A(" *")
    A(" *  They are excluded as a class, by rule: each is either Lua-VM")
    A(" *  machinery or carries policy that lives in `static` functions inside")
    A(" *  jce_script.c.  Re-implementing that policy here would be a second")
    A(" *  copy of a security decision; exporting the raw host member instead")
    A(" *  would hand a binding the unvalidated primitive.  The manifest's own")
    A(" *  reasons, verbatim:")
    A(" *")
    for e in api["hand_written"]:
        A(f" *    {e['name']}")
        for ln in _wrap(e["reason"], " *        "):
            A(ln)
    A(" * ------------------------------------------------------------------ */")
    A("")
    for e in api["expose"]:
        ret, fn, decl, _ = _entry_decl(e)
        A(f"/* {PREFIX}{e['name']} -- shape: {e['shape']}, since {e['since']}")
        if e.get("doc"):
            L.extend(_wrap(e["doc"], " * "))
        if e.get("bind_args"):
            bound = ", ".join(f"{k}={v}" for k, v in e["bind_args"].items())
            L.extend(_wrap(f"The binding supplies {bound} (manifest bind_args), "
                           f"so it is not an argument here either.", " * "))
        if e.get("index_base"):
            L.extend(_wrap(
                f"INDEX BASE: the Lua binding takes a {e['index_base']}-based "
                f"index and subtracts {e['index_base']} before calling the host. "
                f"This ABI is the transport, so it passes the host's own "
                f"0-based index straight through.", " * "))
        if isinstance(e.get("absent_value"), dict):
            L.extend(_wrap(f"When the host member is absent this returns "
                           f"`{e['absent_value']['param']}` itself, not NULL.",
                           " * "))
        elif e.get("absent_value") is not None:
            L.extend(_wrap(f"When the host member is absent this returns "
                           f"{e['absent_value']}, not 0.", " * "))
        if "clamp_min" in e:
            L.extend(_wrap(f"The returned value is clamped to a minimum of "
                           f"{e['clamp_min']}.", " * "))
        if e["shape"] == "owned_string_release":
            A(f" * Copies at most out_cap-1 bytes plus a NUL into `out`, releases")
            A(f" * the host's string through `{e['release']}`, and returns the full")
            A(" * length (which may exceed out_cap-1) or -1 when absent.  Call")
            A(" * with out=NULL, out_cap=0 to ask for the length.")
        A(" */")
        A(_signature_text(ret, fn, decl) + ";")
        A("")
    A("#ifdef __cplusplus")
    A("}")
    A("#endif")
    A("")
    A("#endif /* JCE_SCRIPT_API_H */")
    return "\n".join(L) + "\n"


def emit_c(api: dict) -> str:
    L: list[str] = []
    A = L.append
    A("/* jce_script_api.gen.c -- GENERATED. DO NOT EDIT.")
    A(" *")
    A(" *   python tools/scriptgen/gen_script_c_abi.py --write")
    A(" *")
    A(" * One forwarder per manifest `expose` entry.  Every one of them guards")
    A(" * the host member before calling it, for the reason")
    A(" * tests/middleware/script/test_jce_script_host_abi.c exists:")
    A(" * jce_script_api_open copies min(host_size, sizeof) over a zeroed table,")
    A(" * so a member the caller's shorter header did not have stays NULL, and")
    A(" * calling it unguarded jumps through whatever followed the caller's")
    A(" * object.")
    A(" *")
    A(" * The guard has exactly ONE spelling here -- `!api || !api->host.<member>`")
    A(" * -- and the gate's unit tests assert it appears in every emitted")
    A(" * function, naming the member the manifest names.")
    A(" */")
    A("")
    A('#include "jce_script_api_internal.h"')
    A("")
    A("#include <string.h>")
    A("")
    for e in api["expose"]:
        ret, fn, decl, args = _entry_decl(e)
        member = e["vtable"]
        A(f"/* {PREFIX}{e['name']} -> host.{member} ({e['shape']}) */")
        A(_signature_text(ret, fn, decl))
        A("{")
        if e["shape"] == "owned_string_release":
            rel = e["release"]
            A("    char *s;")
            A("    size_t n;")
            A("")
            A("    if (out && out_cap > 0)")
            A("        out[0] = '\\0';")
            A(f"    if (!api || !api->host.{member} || !api->host.{rel})")
            A("        return -1;")
            A(f"    s = api->host.{member}({', '.join(args)});")
            A("    if (!s)")
            A("        return -1;")
            A("    n = strlen(s);")
            A("    if (out && out_cap > 0) {")
            A("        size_t room = (size_t)out_cap - 1u;")
            A("        size_t take = (n < room) ? n : room;")
            A("        memcpy(out, s, take);")
            A("        out[take] = '\\0';")
            A("    }")
            A(f"    api->host.{rel}(api->host.user, s);")
            A("    return (int)n;")
            A("}")
            A("")
            continue
        clamp = "clamp_min" in e
        if clamp:
            A(f"    {ret} v;")
            A("")
        A(f"    if (!api || !api->host.{member}) {{")
        for z in _zero_outs(e, "        "):
            A(z)
        A(f"        {_absent_return(e, ret)}")
        A("    }")
        call = f"api->host.{member}({', '.join(args)})"
        if clamp:
            floor = _lit(e["clamp_min"], ret)
            A(f"    v = {call};")
            A(f"    if (v < {floor})")
            A(f"        v = {floor};")
            A("    return v;")
        elif ret == "void":
            A(f"    {call};")
        else:
            A(f"    return {call};")
        A("}")
        A("")
    return "\n".join(L) + "\n"


def emit_def(api: dict) -> str:
    L = ["; jce_script_api.def -- GENERATED. DO NOT EDIT.",
         ";",
         ";   python tools/scriptgen/gen_script_c_abi.py --write",
         ";",
         "; MSVC exports nothing by default, so THIS FILE is the export set on",
         "; Windows: what is not listed here does not leave the DLL.  It is",
         "; generated from contracts/script-api.json, never edited, and",
         "; tests/scripting/c_abi/test_jce_script_api_abi.c asserts the DLL's",
         "; real export table equals it -- in both directions.",
         "EXPORTS"]
    L += [f"    {n}" for n in export_names(api)]
    return "\n".join(L) + "\n"


def emit_map(api: dict) -> str:
    L = ["/* jce_script_api.map -- GENERATED. DO NOT EDIT.",
         " *",
         " *   python tools/scriptgen/gen_script_c_abi.py --write",
         " *",
         " * ELF linker version script: the GNU/LLVM counterpart of the .def.",
         " * -fvisibility=hidden already hides everything without the",
         " * JCE_SCRIPT_API attribute; this makes the export set an explicit",
         " * list rather than a property of every source file remembering the",
         " * macro.",
         " *",
         " * Anonymous version node on purpose: it restricts visibility without",
         ' * attaching a symbol version, so dlsym("jce_script_api_...") keeps',
         " * working unqualified.",
         " */",
         "{",
         "    global:"]
    L += [f"        {n};" for n in export_names(api)]
    L += ["    local:",
          "        *;",
          "};"]
    return "\n".join(L) + "\n"


def emit_exports_h(api: dict) -> str:
    names = export_names(api)
    member, entry = tail_member(api)
    L = ["/* jce_script_api_exports.gen.h -- GENERATED. DO NOT EDIT.",
         " *",
         " *   python tools/scriptgen/gen_script_c_abi.py --write",
         " *",
         " * The export set, for the symbol-table test.  Not installed and not",
         " * part of the ABI: it exists so that the test compares the DLL's real",
         " * export table against the manifest instead of against a list someone",
         " * maintains by hand.",
         " */",
         "#ifndef JCE_SCRIPT_API_EXPORTS_GEN_H",
         "#define JCE_SCRIPT_API_EXPORTS_GEN_H",
         "",
         f"#define JCE_SCRIPT_API_EXPORT_COUNT {len(names)}",
         "",
         "/* The host member with the highest vtable_index, and the entry point",
         " * that reaches it.  The short-host test truncates AT this member's",
         " * offset, so it keeps withholding the real tail as the struct grows",
         " * instead of testing a member that stopped being last. */",
         f"#define JCE_SCRIPT_API_TAIL_MEMBER {member}",
         f'#define JCE_SCRIPT_API_TAIL_ENTRY  "{entry}"',
         "",
         "static const char *const",
         "kJceScriptApiExports[JCE_SCRIPT_API_EXPORT_COUNT] = {"]
    L += [f'    "{n}",' for n in names]
    L += ["};", "", "#endif /* JCE_SCRIPT_API_EXPORTS_GEN_H */"]
    return "\n".join(L) + "\n"


# --------------------------------------------------------------------- #
#  Modes.                                                                #
# --------------------------------------------------------------------- #

# newline="\n" on EVERY write: Path.write_text() translates line endings on
# Windows, and a CRLF emit against an LF-committed file makes check mode
# permanently red for a reason that has nothing to do with the ABI.
def write_lf(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def _diff(path: Path, want: str) -> list[str]:
    rel = path.relative_to(REPO_ROOT).as_posix()
    if not path.is_file():
        return [f"{rel} is MISSING -- regenerate with:",
                "        python tools/scriptgen/gen_script_c_abi.py --write"]
    have = path.read_text(encoding="utf-8")
    if have == want:
        return []
    d = list(difflib.unified_diff(have.splitlines(), want.splitlines(),
                                  f"{rel} (committed)", f"{rel} (fresh emit)",
                                  lineterm="", n=1))
    return ([f"{rel} differs from a fresh emit:"]
            + ["        " + x for x in d[:40]]
            + ["        python tools/scriptgen/gen_script_c_abi.py --write"])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()

    api = load_api()
    problems = validate(api)
    if problems:
        print("script-c-abi: FAILED")
        for p in problems:
            print("    " + p)
        return 1

    want = {PUBLIC_H: emit_header(api), GEN_C: emit_c(api),
            DEF_FILE: emit_def(api), MAP_FILE: emit_map(api),
            EXPORTS_H: emit_exports_h(api)}

    if args.write:
        for path, text in want.items():
            write_lf(path, text)
        print("wrote " + ", ".join(p.relative_to(REPO_ROOT).as_posix()
                                   for p in want))
        return 0

    emitted: list[str] = []
    for path, text in want.items():
        emitted += _diff(path, text)
    if emitted:
        print("script-c-abi: FAILED")
        for p in emitted:
            print("    " + p)
        return 1
    print(f"script-c-abi: OK - {len(api['expose'])} manifest entries + "
          f"{len(META)} meta = {len(export_names(api))} exports, "
          f"{len(api['hand_written'])} hand-written entries excluded with a "
          f"reason; 5 artefacts byte-identical to a fresh emit")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
