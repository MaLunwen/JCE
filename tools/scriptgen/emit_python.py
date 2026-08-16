#!/usr/bin/env python3
"""
emit_python.py — the Python backend.  One of N; see scriptgen_core.py's
docstring for how to add another without touching this file or that one.

WHAT BINDS, AND WHY IT IS ctypes AND NOT cffi
---------------------------------------------
`scripting/c_abi/` is a plain C shared library with 74 exports and no C++, no
callbacks going out, and a type vocabulary of eight scalars plus two POD
structs.  Both ctypes and cffi's ABI mode can call that with NO C compilation.
ctypes is chosen because it is in the standard library: `pip install jce-script`
resolves to a pure-Python wheel with zero dependencies and zero build step on
every interpreter that can run the engine's tooling, which is the
godot-python / UnrealEnginePython expectation ("install it and it works").
cffi would add a dependency that itself ships a C extension — a wheel per
(python, platform) tuple — to buy a parser this surface does not need, because
the signatures are already machine-readable in script-api.json.

Consequence stated rather than hidden: ctypes' per-call overhead is higher than
cffi's API mode.  That cost is paid on a surface of 71 entries called from
gameplay code, not in a hot loop, and the alternative is a compiler at install
time.  If it ever matters, the fix is a compiled accelerator BESIDE this
module, not a different transport for it.

THE ABSENT-VALUE POLICY IS THIS BACKEND'S, AND IT IS "None SPELLS nil"
---------------------------------------------------------------------
scriptgen_core.C_ZERO's docstring says the absent value is a per-language
CONTRACT decision and suggests a Python backend "would answer None where Lua
answers 0.0".  It does NOT, and the reason is measured rather than stylistic:

  Lua is the reference implementation (batch 1 proved the generated Lua
  bindings equivalent to the hand-written originals over 87 cases; that oracle
  is spent and never comes back).  The only acceptance available to Python is a
  cross-language differential against Lua.  A per-shape translation table
  saying "Lua's "" may equal Python's None here" sits BETWEEN the reference and
  the thing under test, and it is exactly the place an error hides: with such a
  table, an emitter that returned None for EVERY `const char *` — including a
  live host's real answer — still passes the absent case.

So the rule is one line, and it is total:

    None is Python's spelling of Lua's nil.  Nothing else becomes None.

Per shape, with the value an ABSENT host produces:

  void_call             -> None always (Lua returns zero values; the driver
                           requires `is None` and records zero slots)
  value_return          -> the C value.  bool/int/float as-is; `const char *`
                           NULL decodes to "" because that is what
                           `lua_pushstring(L, v ? v : "")` pushes.  `absent_value`
                           and `clamp_min` are applied by the C ABI forwarder
                           and are NOT re-applied here.
  fallible_out          -> tuple of the out slots on success; on failure the
                           manifest's `miss_value` if it has one (raycast: the
                           integer 0), otherwise None.
  void_out_array        -> tuple of N floats; zeros when absent, same as Lua's
                           zero-initialised local.
  first_and_count       -> (first_or_None, count).  n <= 0 gives (None, n) —
                           n is passed through even when negative, as Lua does.
  entity_table          -> list[int]; empty when n <= 0.
  owned_string_release  -> str, or None when the producer returned NULL or the
                           host cannot release (see the divergence note below).

WHAT IS DELIBERATELY NOT EXPOSED
--------------------------------
The manifest's seven `hand_written` entries are absent as a class, by the same
rule that keeps them out of the C ABI, and the generated module carries the
manifest's own `reason` text for each so the gap is named rather than silent.

`jce.json_null` — the 79th table key — is ALSO absent, and that is a decision
this backend makes and must defend.  It is a sentinel with NO PRODUCER on this
side: the only thing that ever yields it is `asset_read_json`, which is
hand-written and excluded from the C ABI, and `owned_string_release` hands back
the raw JSON TEXT (as Lua does) rather than a parsed tree.  A sentinel object
no reader can return is a contract declared before anything enforces it.
differential.py's check_key_sets is what fails if a name appears or disappears
without a reason beside it — and it reads the LIVE Lua table with pairs(), not
the manifest both surfaces were generated from.

Its tests: tests/scripting/python/differential.py  the cross-language
               differential against Lua, which is the acceptance for
               everything this file emits
               (ctest: test_jce_script_python_differential)
           tests/scripting/python/test_emit_python.py  the four properties
               that differential is structurally blind to: stub-versus-module
               drift, the optional-argument ordering rule, this backend's own
               validate(), and "no C compilation"
               (ctest: test_jce_script_python_emitter)
           test_script_bindings_gate.py  the shared gate
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
    flatten_pod,
    in_params,
    out_params,
    script_in_params,
)

PKG = REPO_ROOT / "scripting/python/jce_script"
GEN_PY = PKG / "_generated.py"
GEN_PYI = PKG / "_generated.pyi"
MOCK_C = REPO_ROOT / "tests/scripting/python/jce_script_py_mock.gen.c"

C_ABI_HEADER = REPO_ROOT / "scripting/c_abi/include/jce/script_api/jce_script_api.h"
PREFIX = "jce_script_api_"

# The three exports that are not a manifest entry.  gen_script_c_abi.py owns
# this list (its META tuple); it is restated here rather than imported because
# importing another backend's driver to read one constant makes two tools that
# must be loaded together.  It is not a free-floating claim: validate() below
# asserts each of these is declared in the committed C ABI header, so a rename
# there reddens this backend by name.
META = ("version", "open", "close")

# ── The C type vocabulary, twice: once for ctypes, once for annotations ──────
#
# Closed on purpose.  A member whose type is not here raises rather than
# guessing, for the same reason emit_lua._push does: a silently wrong ctypes
# declaration reads garbage off the stack and produces a plausible number.
CTYPE = {
    "float":           "ctypes.c_float",
    "double":          "ctypes.c_double",
    "bool":            "ctypes.c_bool",
    "int":             "ctypes.c_int",
    "uint32_t":        "ctypes.c_uint32",
    "uint64_t":        "ctypes.c_uint64",
    "JceScriptEntity": "ctypes.c_uint64",
    "const char *":    "ctypes.c_char_p",
    "char *":          "ctypes.c_char_p",
}

PYTYPE = {
    "float": "float", "double": "float", "bool": "bool", "int": "int",
    "uint32_t": "int", "uint64_t": "int", "JceScriptEntity": "int",
    "const char *": "str", "char *": "str", "void": "None",
}

# Python keywords that appear as C parameter names on this surface.  `dir` is
# not a keyword but shadows a builtin inside the method body, and the emitted
# bodies never call it; renaming anyway would make the published signature
# disagree with script-api.json's `params`, which is the two-sided contract
# this generator exists to prevent.  Only true keywords are renamed, and the
# rename is visible in the stub.
_PY_KEYWORDS = frozenset("""
False None True and as assert async await break class continue def del elif
else except finally for from global if import in is lambda nonlocal not or
pass raise return try while with yield
""".split())


def py_name(name: str) -> str:
    return name + "_" if name in _PY_KEYWORDS else name


def _ctype(c_type: str) -> str:
    if c_type not in CTYPE:
        raise SystemExit(
            f"error: emit_python: no ctypes rule for C type {c_type!r} — mark "
            f"the binding hand_written rather than guessing a declaration here")
    return CTYPE[c_type]


def _pytype(c_type: str) -> str:
    if c_type not in PYTYPE:
        raise SystemExit(f"error: emit_python: no annotation for {c_type!r}")
    return PYTYPE[c_type]


# ── POD out parameters ───────────────────────────────────────────────────────
#
# raycast's out parameter is a JceScriptRaycastHit, which Lua pushes as eight
# scalars via flatten_pod.  The ctypes mirror is emitted with the SAME flattened
# fields in the SAME order, so the tuple this backend returns is slot-for-slot
# what Lua pushes.  `float point[3]` and three consecutive `float` have
# identical layout in C (arrays carry no inter-element padding and float[3] has
# float's alignment); the differential asserts ctypes.sizeof against the C
# sizeof rather than trusting that sentence — see differential.py's
# check_struct_layout, which reads sizeof out of the compiled mock.
def _pod_fields(header_text: str, c_type: str) -> list[tuple[str, str]]:
    return [(expr.replace("[", "_").replace("]", ""), t)
            for expr, t in flatten_pod(header_text, c_type)]


def _pod_class(c_type: str) -> str:
    return "_" + c_type.replace("JceScript", "")


def pod_out_types(members: list[HostMember], man: dict) -> list[str]:
    """Every non-scalar out-parameter type on the surface, in first-use order."""
    by = {m.name: m for m in members}
    seen: list[str] = []
    for e in man["expose"]:
        for p in out_params(by[e["vtable"]]):
            if p.arity < 0 and p.c_type not in CTYPE and p.c_type not in seen:
                seen.append(p.c_type)
    return seen


# ── One entry's Python-visible signature ─────────────────────────────────────

def entry_params(m: HostMember, ent: dict) -> list[Param]:
    """The parameters the Python method takes: script inputs minus bind_args.

    bind_args is applied by the C ABI forwarder (jump_pressed / sprint /
    attack_pressed are three entries over one `input_button`), so the button
    number is not an argument here EITHER — passing it through would make the
    three methods one function and widen the surface."""
    bind = ent.get("bind_args") or {}
    return [p for p in script_in_params(m, ent) if p.name not in bind]


def default_for(p: Param, ent: dict, params: list[Param]) -> str | None:
    """The Python default for `p`, or None when it must be passed positionally.

    A parameter gets a real default ONLY if it is optional AND every parameter
    after it is optional too.  gas_apply is why: its optional `op` is followed
    by a REQUIRED `magnitude`, and Python forbids a defaulted parameter before
    a non-defaulted one.  Lua's answer to the same problem is nil IN PLACE
    (`luaL_optinteger` treats an explicit nil as absent), so the Python spelling
    of `jce.gas_apply(e, a, nil, m)` is `api.gas_apply(e, a, None, m)` — the
    method still accepts None there and substitutes the manifest default.
    tests/scripting/python/test_emit_python.py's
    test_optional_before_a_required_parameter_has_no_default is what fails; it
    derives the rule from script-api.json, never from this function."""
    opt = ent.get("optional") or {}
    if p.name not in opt:
        return None
    tail = params[params.index(p) + 1:]
    if any(q.name not in opt for q in tail):
        return None
    v = opt[p.name]
    if v is None:
        return "None"
    if v is True:
        return "True"
    if v is False:
        return "False"
    return repr(v)


def _sig_parts(m: HostMember, ent: dict) -> list[str]:
    out = ["self"]
    params = entry_params(m, ent)
    for p in params:
        ann = ("Sequence[float]" if p.arity > 0 else _pytype(p.c_type))
        opt = ent.get("optional") or {}
        d = default_for(p, ent, params)
        # Every optional accepts None, whether or not it also carries a
        # default: None is nil-in-place, which is the only spelling available
        # for an optional that a required parameter follows (gas_apply's `op`).
        if p.name in opt:
            ann += " | None"
        out.append(f"{py_name(p.name)}: {ann}" + (f" = {d}" if d else ""))
    return out


def return_slots(m: HostMember, ent: dict,
                 header_text: str) -> list[tuple[str, str]]:
    """(expression-type, python-type) per RESULT SLOT, in push order.

    Push order is Lua's, because Lua is the reference: fallible_out pushes each
    out parameter in declaration order, flattening a POD."""
    slots: list[tuple[str, str]] = []
    for o in out_params(m):
        if o.arity > 0:
            slots += [(o.c_type, "float")] * o.arity
        elif o.c_type in CTYPE:
            slots.append((o.c_type, _pytype(o.c_type)))
        else:
            slots += [(t, _pytype(t)) for _, t in _pod_fields(header_text,
                                                             o.c_type)]
    return slots


def return_annotation(m: HostMember, ent: dict, header_text: str) -> str:
    shape = ent["shape"]
    if shape == "void_call":
        return "None"
    if shape == "value_return":
        return _pytype(m.ret)
    if shape == "entity_table":
        return "list[int]"
    if shape == "first_and_count":
        return "tuple[int | None, int]"
    if shape == "owned_string_release":
        return "str | None"
    slots = return_slots(m, ent, header_text)
    tup = "tuple[" + ", ".join(t for _, t in slots) + "]"
    if shape == "void_out_array":
        return tup
    miss = "int" if ent.get("miss_value") is not None else "None"
    return f"{tup} | {miss}"


# ── The ctypes declaration of one C ABI entry ────────────────────────────────

def argtypes(m: HostMember, ent: dict) -> list[str]:
    """The C ABI forwarder's argtypes, INCLUDING the ones bind_args supplies.

    Derived from the host member's own declaration minus `void *user`, which
    the forwarder replaces with the opaque handle — the same transformation
    gen_script_c_abi.py performs, read from the same HostMember, so the two
    cannot disagree without this backend's validate() naming it."""
    bind = ent.get("bind_args") or {}
    out = ["ctypes.c_void_p"]
    for p in m.params[1:]:
        if p.name in bind:
            continue
        if p.arity > 0:
            base = p.c_type[len("const "):] if p.c_type.startswith("const ") \
                else p.c_type
            out.append(f"{_ctype(base)} * {p.arity}")
        elif p.arity < 0:
            out.append(f"ctypes.POINTER({_ctype(p.c_type)})"
                       if p.c_type in CTYPE
                       else f"ctypes.POINTER({_pod_class(p.c_type)})")
        else:
            out.append(_ctype(p.c_type))
    if ent["shape"] == "owned_string_release":
        # The forwarder copies into the caller's buffer and releases the host's
        # string before returning, so no ownership crosses the ABI.  Its extra
        # two parameters are not in the vtable declaration.
        out += ["ctypes.c_char_p", "ctypes.c_int"]
    return out


def restype(m: HostMember, ent: dict) -> str:
    if ent["shape"] == "owned_string_release":
        return "ctypes.c_int"        # length, or -1
    if m.ret == "void":
        return "None"
    return _ctype(m.ret)


# ── Emitting the module ──────────────────────────────────────────────────────

_PY_BANNER = '''\
# jce_script/_generated.py — GENERATED. DO NOT EDIT.
#
#   python tools/scriptgen/gen_script_bindings.py --write
#
# Source of truth: struct JceScriptHost in
# engine/include/jce/middleware/script/jce_script.h joined with the exposure
# decisions in engine/src/middleware/script/script_exposure.json, published as
# contracts/script-api.json.  The transport is the C ABI shared library
# in scripting/c_abi/, bound with ctypes — no C compilation, no dependency.
#
# EVERY entry point is resolved BY NAME (_decl below), never by ordinal and
# never by slot.  script-api.json's own _note explains why: the append-only
# rule plus the min(caller, engine) copy over a zeroed host is what makes a
# short host safe, and anything keyed off a position defeats it.  A missing
# export raises at bind time naming the symbol, rather than at first call
# through a NULL — the same reason tests/scripting/c_abi's must_sym() exists.
#
# ABSENT VALUES: `None` is this module's spelling of Lua's `nil` and is used
# for nothing else.  The rule and its derivation are in
# tools/scriptgen/emit_python.py's docstring; the cross-language differential
# in tests/scripting/python/ is what holds it.
#
@ABSENT_TABLE@
'''

_PY_PRELUDE = '''

from __future__ import annotations

import ctypes
from typing import Sequence

__all__ = [
    "Api",
    "ENTRY_NAMES",
    "ENTRY_COUNT",
    "SCRIPT_API_VERSION",
    "NOT_EXPOSED",
    "OWNED_STRING_INITIAL_CAPACITY",
    "OWNED_STRING_MAX_ATTEMPTS",
    "MissingExportError",
]


class MissingExportError(AttributeError):
    """A jce_script_api_* entry point is not in the loaded library.

    Raised at bind time, from _decl, with the symbol named.  A ctypes CDLL
    hands back a callable for any attribute it can resolve and raises
    AttributeError otherwise; catching it here turns "the library is an older
    build" into one legible message instead of an AttributeError from inside a
    generated method."""


def _decl(lib, name: str, restype, argtypes):
    try:
        fn = getattr(lib, name)
    except AttributeError:
        raise MissingExportError(
            f"{name} is not exported by the loaded jce_script_api library; "
            f"it was generated for script_api_version "
            f"{SCRIPT_API_VERSION}") from None
    fn.restype = restype
    fn.argtypes = argtypes
    return fn


def _s(v):
    """A Python str as the C ABI's `const char *`; None stays NULL.

    None is passed through rather than encoded because two manifest entries
    (send_message, broadcast) carry `optional: {"str_arg": null}`, whose Lua
    reading is `lua_isstring(...) ? lua_tostring(...) : NULL` — an explicit
    absent string, not an empty one."""
    if v is None:
        return None
    if isinstance(v, bytes):
        return v
    return str(v).encode("utf-8")


def _f(v, n: int, arr, what: str):
    """A length-checked `const float[n]` input.

    Lua reads these with n separate luaL_checknumber calls and raises when the
    caller passed fewer; a short sequence here would otherwise reach the host
    as zero-filled tail, which is a wrong answer rather than an error."""
    seq = tuple(v)
    if len(seq) != n:
        raise ValueError(f"{what} takes exactly {n} numbers, got {len(seq)}")
    return arr(*seq)


# How many bytes the first owned-string call offers the C ABI.
#
# THE C ABI HAS NO WAY TO ASK FOR A LENGTH WITHOUT PRODUCING THE STRING: its
# owned_string_release forwarder calls the host, copies into the caller's
# buffer, and RELEASES — so a call that finds the buffer too small and retries
# calls the host a SECOND time and frees a SECOND time.  Lua, which receives
# the host's pointer directly, always calls once.  That divergence is real,
# documented, and pinned by differential.py's check_long_string_retry, which
# asserts the two call counts are exactly 1 and 2 and that every copy-out
# released; this constant is set high enough that component and render JSON fit
# in one call.
OWNED_STRING_INITIAL_CAPACITY = 8192

# A host whose string keeps growing between calls must not spin forever.
OWNED_STRING_MAX_ATTEMPTS = 4


def _owned(fn, args, what: str):
    cap = OWNED_STRING_INITIAL_CAPACITY
    for _ in range(OWNED_STRING_MAX_ATTEMPTS):
        buf = ctypes.create_string_buffer(cap)
        n = fn(*args, buf, cap)
        if n < 0:
            return None
        if n < cap:
            return buf.value.decode("utf-8", "replace")
        cap = n + 1
    raise RuntimeError(
        f"{what}: the host's string grew on every one of "
        f"{OWNED_STRING_MAX_ATTEMPTS} attempts")

'''


def _absent_table(man: dict) -> str:
    """The exclusion list, with the manifest's own reason text.

    Emitted into the banner AND into NOT_EXPOSED, so the reason is reachable
    from a REPL and not only from a comment.  gen_script_c_abi.py's condition
    G5 already refuses a hand_written entry with no reason; this backend does
    not restate that check, it consumes the text."""
    lines = ["# NOT EXPOSED — the manifest's seven hand-written entries, as a",
             "# class, with its own reason text, plus json_null:"]
    for name, reason in ([(e["name"], e["reason"]) for e in man["hand_written"]]
                         + [("json_null", _JSON_NULL_REASON)]):
        lines.append(f"#   {name}")
        lines += _wrap(reason, "#       ")
    return "\n".join(lines)


_JSON_NULL_REASON = (
    "no producer on this side: the only thing that yields the sentinel is "
    "asset_read_json, which is hand-written and excluded from the C ABI, and "
    "owned_string_release returns raw JSON TEXT. A sentinel no reader can "
    "return is a contract nothing enforces.")


def _wrap(text: str, indent: str, width: int = 79) -> list[str]:
    """Word-wrap prose into docstring lines.

    The manifest's `doc` fields run to 200 characters; pasted unwrapped they
    make the generated module unreadable in exactly the place a reader goes to
    find out what an entry does."""
    out: list[str] = []
    cur = indent
    for word in text.split():
        cand = cur + word if cur == indent else cur + " " + word
        if len(cand) > width and cur != indent:
            out.append(cur)
            cur = indent + word
        else:
            cur = cand
    if cur != indent:
        out.append(cur)
    return out


def _method(m: HostMember, ent: dict, header_text: str) -> list[str]:
    name, shape = ent["name"], ent["shape"]
    L: list[str] = []
    A = L.append
    sig = ", ".join(_sig_parts(m, ent))
    A(f"    def {py_name(name)}({sig}) -> "
      f"{return_annotation(m, ent, header_text)}:")

    doc = [f'        """jce.{name} — shape: {shape}, since {ent["since"]}.']
    if ent.get("doc"):
        doc.append("")
        doc += _wrap(ent["doc"], "        ")
    notes = []
    if ent.get("bind_args"):
        notes.append("The C ABI supplies "
                     + ", ".join(f"{k}={v}" for k, v in ent["bind_args"].items())
                     + " (manifest bind_args), so it is not an argument.")
    if ent.get("index_base"):
        notes.append(f"index is {ent['index_base']}-based, as in Lua; below "
                     f"{ent['index_base']} returns without calling the host.")
    if ent.get("absent_value") is not None:
        notes.append("With the host member absent the C ABI answers the "
                     "manifest's absent_value, not zero.")
    if "clamp_min" in ent:
        notes.append(f"The C ABI clamps the result to a minimum of "
                     f"{ent['clamp_min']}.")
    if ent.get("miss_value") is not None:
        notes.append(f"A miss answers {ent['miss_value']!r}, not None — "
                     f"scripts branch on it.")
    if shape == "owned_string_release":
        notes.append(f"The C ABI releases the host's string through "
                     f"`{ent['release']}` before returning; nothing is owned "
                     f"here.")
    if notes:
        doc.append("")
        for n in notes:
            doc += _wrap(n, "        ")
    doc.append('        """')
    L += doc

    params = entry_params(m, ent)
    opt = ent.get("optional") or {}
    strict = set(ent.get("strict") or [])
    call: list[str] = ["self._h"]

    for p in params:
        v = py_name(p.name)
        if p.name in opt and opt[p.name] is not None:
            # None means "use the manifest default" for EVERY optional, not
            # only the ones that could not carry a Python default.  Lua's
            # luaL_opt* treats an explicit nil exactly like an absent argument,
            # so `jce.spawn(p, nil, nil, nil)` and `jce.spawn(p)` agree; if the
            # Python side accepted None only where the signature forced it,
            # api.spawn(p, None) would reach ctypes and raise instead.
            # differential.py's <entry>/nil_in_place case is what fails, with
            # <entry>/short as its companion: both must produce the same host
            # call the full-arity case produces.
            A(f"        if {py_name(p.name)} is None:")
            A(f"            {py_name(p.name)} = {opt[p.name]!r}")
        if p.arity > 0:
            base = p.c_type[len("const "):]
            A(f"        _{v} = _f({v}, {p.arity}, "
              f"{_ctype(base)} * {p.arity}, {name!r})")
            call.append(f"_{v}")
            continue
        if p.c_type == "const char *":
            call.append(f"_s({v})")
            continue
        if p.c_type == "bool":
            if p.name in strict:
                # The ONLY type-checked argument on this surface.  Lua's
                # luaL_checktype raises here; accepting a truthy 5 in Python
                # would make the two surfaces disagree on the one entry the
                # manifest singles out.  differential.py's
                # set_parent/strict_bad case is what fails, and every other
                # boolean entry's /loose_bool case is its control: without that
                # control, "raised" would be a category both sides could enter
                # for unrelated reasons.
                A(f"        if not isinstance({v}, bool):")
                A(f"            raise TypeError("
                  f"{name!r} + ' expects a bool for {p.name}, got ' "
                  f"+ type({v}).__name__)")
                call.append(v)
            else:
                # Every other boolean takes any truthy value, because
                # lua_toboolean does.
                #
                # MEASURED IMMUNE, and the reason is written here rather than
                # left for the next reader to rediscover: deleting this bool()
                # changes nothing observable, because ctypes' c_bool argtype
                # ALREADY takes any object's truthiness -- c_bool(5),
                # c_bool("x") and c_bool([1]) are all True (measured, CPython
                # 3.12).  Mutation M16 is green and cannot be made red without
                # a test that asserts on the emitted TEXT rather than on
                # behaviour.  It stays because it makes the lua_toboolean
                # contract explicit at the call site and because a future
                # transport that is stricter than ctypes would need it.
                call.append(f"bool({v})")
            continue
        if ent.get("index_base") and p is params[0]:
            base = ent["index_base"]
            A(f"        if {v} < {base}:")
            A("            return None")
            call.append(f"{v} - {base}")
            continue
        call.append(v)

    outs = out_params(m)
    if shape == "void_call":
        A(f"        self._f.{name}({', '.join(call)})")
        A("        return None")
    elif shape == "value_return":
        if m.ret == "const char *":
            absent = ent.get("absent_value")
            # The NULL fallback is emit_lua's, verbatim in meaning:
            #     lua_pushstring(L, v ? v : <key or "">)
            # An absent_value of the form {"param": X} means "echo X", and Lua
            # echoes it for a NULL RETURN as well as for a missing member.
            #
            # MEASURED DIVERGENCE, and the reason this branch is not simply
            # `or ""`: the C ABI forwarder applies absent_value ONLY when the
            # host member is NULL (`if (!api || !api->host.loc_translate)
            # return key;`) and passes a NULL RETURN straight through.  So a
            # host that answers NULL for an untranslated key gives Lua the key
            # and would give Python "" -- blank UI, which is exactly what the
            # manifest's own doc says passthrough exists to prevent.  The
            # differential's `tr/full` case in miss mode is what caught it and
            # is what keeps it caught; jce_script_api.gen.c is another
            # backend's file and is reported, not edited.
            fallback = (py_name(absent["param"]) if isinstance(absent, dict)
                        else '""')
            A(f"        v = self._f.{name}({', '.join(call)})")
            A('        return v.decode("utf-8", "replace") if v is not None '
              f'else {fallback}')
        else:
            A(f"        return self._f.{name}({', '.join(call)})")
    elif shape == "void_out_array":
        o = outs[0]
        A(f"        {o.name} = ({_ctype(o.c_type)} * {o.arity})()")
        A(f"        self._f.{name}({', '.join(call + [o.name])})")
        A("        return (" + ", ".join(f"{o.name}[{i}]"
                                         for i in range(o.arity)) + ",)")
    elif shape == "fallible_out":
        reads: list[str] = []
        for o in outs:
            if o.arity > 0:
                A(f"        {o.name} = ({_ctype(o.c_type)} * {o.arity})()")
                call.append(o.name)
                reads += [f"{o.name}[{i}]" for i in range(o.arity)]
            elif o.c_type in CTYPE:
                A(f"        {o.name} = {_ctype(o.c_type)}()")
                call.append(f"ctypes.byref({o.name})")
                reads.append(f"{o.name}.value")
            else:
                A(f"        {o.name} = {_pod_class(o.c_type)}()")
                call.append(f"ctypes.byref({o.name})")
                reads += [f"{o.name}.{f}"
                          for f, _ in _pod_fields(header_text, o.c_type)]
        miss = ent.get("miss_value")
        A(f"        if not self._f.{name}({', '.join(call)}):")
        A(f"            return {'None' if miss is None else repr(miss)}")
        A("        return (" + ", ".join(reads) + ",)")
    elif shape in ("first_and_count", "entity_table"):
        o = outs[0]
        cap = ent["out_capacity"]
        A(f"        found = ({_ctype(o.c_type)} * {cap})()")
        A(f"        n = self._f.{name}({', '.join(call + ['found', str(cap)])})")
        if shape == "first_and_count":
            A("        return (found[0], n) if n > 0 else (None, n)")
        else:
            # min(n, cap): a host that answers more than the capacity it was
            # handed would run this list comprehension off the end of the
            # array.  The Lua binding does NOT clamp — jce_script_bindings
            # .gen.c's find_by_prefix loops to the host's n over a fixed
            # JceScriptEntity[1024] — which is an over-read in emit_lua, not
            # here.  Reported, not fixed: that file is another backend's.
            #
            # MEASURED IMMUNE (mutation M17 is green), and it must stay that
            # way: reaching this clamp needs a LYING host, and a lying host
            # drives the Lua side into an out-of-bounds read of a fixed C
            # array.  The differential would then be comparing this binding
            # against undefined behaviour.  So the reason is written here
            # instead of being turned into a test.
            A(f"        n = min(n, {cap})")
            A("        return [found[i] for i in range(n)] if n > 0 else []")
    elif shape == "owned_string_release":
        A(f"        return _owned(self._f.{name}, ({', '.join(call)},), "
          f"{name!r})")
    else:
        raise SystemExit(f"error: emit_python: unknown shape {shape!r}")
    A("")
    return L


def emit_module(members: list[HostMember], man: dict) -> str:
    by = {m.name: m for m in members}
    header_text = HEADER.read_text(encoding="utf-8")
    L: list[str] = [_PY_BANNER.replace("@ABSENT_TABLE@", _absent_table(man)),
                    _PY_PRELUDE.rstrip("\n"), ""]
    A = L.append

    A(f"SCRIPT_API_VERSION = {man['script_api_version']}")
    A(f"ENTRY_COUNT = {len(man['expose'])}")
    A("")
    A("ENTRY_NAMES = (")
    for e in man["expose"]:
        A(f'    "{e["name"]}",')
    A(")")
    A("")
    A("# Every Lua table key this module deliberately does not carry, with the")
    A("# reason it does not.  A name may not leave the Lua surface and vanish")
    A("# from here silently; the differential's key-set case reads BOTH.")
    A("NOT_EXPOSED = {")
    for e in man["hand_written"]:
        A(f'    "{e["name"]}":')
        A(f'        {e["reason"]!r},')
    A('    "json_null":')
    A(f"        {_JSON_NULL_REASON!r},")
    A("}")
    A("")

    for c_type in pod_out_types(members, man):
        cls = _pod_class(c_type)
        A(f"class {cls}(ctypes.Structure):")
        A(f'    """{c_type}, flattened to the scalars Lua pushes.')
        A("")
        A("    The field order IS the push order: emit_lua flattens this POD")
        A("    with the same scriptgen_core.flatten_pod call, so slot i here")
        A("    is slot i there.  `float point[3]` is emitted as three floats")
        A("    because C gives them identical layout, and the differential")
        A("    asserts ctypes.sizeof against the C sizeof rather than trusting")
        A('    that.')
        A('    """')
        A("")
        A("    _fields_ = [")
        for f, t in _pod_fields(header_text, c_type):
            A(f'        ("{f}", {_ctype(t)}),')
        A("    ]")
        A("")
        A("")

    A("class _Entries:")
    A('    """Every C ABI entry point, resolved by name once per library."""')
    A("")
    A("    __slots__ = (")
    for mname in META:
        A(f'        "_meta_{mname}",')
    for e in man["expose"]:
        A(f'        "{e["name"]}",')
    A("    )")
    A("")
    A("    def __init__(self, lib: ctypes.CDLL) -> None:")
    A(f'        self._meta_version = _decl(lib, "{PREFIX}version", '
      "ctypes.c_uint32, [])")
    A(f'        self._meta_open = _decl(lib, "{PREFIX}open", ctypes.c_void_p,')
    A("            [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32])")
    A(f'        self._meta_close = _decl(lib, "{PREFIX}close", None, '
      "[ctypes.c_void_p])")
    for e in man["expose"]:
        m = by[e["vtable"]]
        A(f'        self.{e["name"]} = _decl(')
        A(f'            lib, "{PREFIX}{e["name"]}", {restype(m, e)},')
        A("            [" + ", ".join(argtypes(m, e)) + "])")
    A("")
    A("")

    A("class Api:")
    A('    """The scripting surface, over one open JceScriptApi handle.')
    A("")
    A("    Construct through jce_script.open_host() or jce_script.attach();")
    A("    this class never opens or closes the handle it is given, because")
    A("    the process that created it owns it — the engine, in-process.")
    A('    """')
    A("")
    A("    __slots__ = (\"_h\", \"_f\", \"_lib\")")
    A("")
    A("    def __init__(self, handle: int, entries: _Entries,")
    A("                 lib: ctypes.CDLL) -> None:")
    A("        self._h = ctypes.c_void_p(handle)")
    A("        self._f = entries")
    A("        self._lib = lib")
    A("")
    A("    @property")
    A("    def handle(self) -> int:")
    A('        """The raw JceScriptApi* this Api wraps."""')
    A("        return int(self._h.value or 0)")
    A("")
    for e in man["expose"]:
        L += _method(by[e["vtable"]], e, header_text)
    return "\n".join(L).rstrip("\n") + "\n"


# ── Emitting the stubs ───────────────────────────────────────────────────────

_PYI_BANNER = '''\
# jce_script/_generated.pyi — GENERATED. DO NOT EDIT.
#
#   python tools/scriptgen/gen_script_bindings.py --write
#
# PEP 561 stubs for _generated.py, emitted from the same manifest as the
# module itself.  This file is most of the value of the binding: an editor that
# completes `api.` with 71 entries, their argument names and their return
# shapes is what makes a generated surface usable, and the contract was already
# machine-readable — writing the stubs by hand would be a second copy of it.
#
# A stub SHADOWS its module for type checkers, so everything public in
# _generated.py is declared here, with the SAME signature.  Byte-identity
# against a fresh emit cannot see the two emitters drift apart — both files
# would still be fresh — so they are compared to EACH OTHER by
# tests/scripting/python/test_emit_python.py's
# test_stub_declares_every_public_name.
'''


def emit_stub(members: list[HostMember], man: dict) -> str:
    by = {m.name: m for m in members}
    header_text = HEADER.read_text(encoding="utf-8")
    L = [_PYI_BANNER, "from __future__ import annotations", "",
         "import ctypes", "from typing import Sequence", ""]
    A = L.append
    A("SCRIPT_API_VERSION: int")
    A("ENTRY_COUNT: int")
    A("ENTRY_NAMES: tuple[str, ...]")
    A("NOT_EXPOSED: dict[str, str]")
    A("OWNED_STRING_INITIAL_CAPACITY: int")
    A("OWNED_STRING_MAX_ATTEMPTS: int")
    A("")
    A("class MissingExportError(AttributeError): ...")
    A("")
    for c_type in pod_out_types(members, man):
        A(f"class {_pod_class(c_type)}(ctypes.Structure):")
        for f, t in _pod_fields(header_text, c_type):
            A(f"    {f}: {_pytype(t)}")
        A("")
    A("class _Entries:")
    A("    def __init__(self, lib: ctypes.CDLL) -> None: ...")
    A("")
    A("class Api:")
    A("    def __init__(self, handle: int, entries: _Entries,")
    A("                 lib: ctypes.CDLL) -> None: ...")
    A("    @property")
    A("    def handle(self) -> int: ...")
    for e in man["expose"]:
        m = by[e["vtable"]]
        sig = ", ".join(_sig_parts(m, e))
        ret = return_annotation(m, e, header_text)
        one = f"    def {py_name(e['name'])}({sig}) -> {ret}: ..."
        if len(one) <= 79:
            A(one)
        else:
            head = f"    def {py_name(e['name'])}("
            parts = _sig_parts(m, e)
            A(head + parts[0] + ",")
            pad = " " * len(head)
            for p in parts[1:-1]:
                A(pad + p + ",")
            A(pad + parts[-1] + f") -> {ret}: ...")
    return "\n".join(L).rstrip("\n") + "\n"


# ── Emitting the differential's mock host ────────────────────────────────────
#
# The mock is generated, and it is generated HERE rather than written by hand,
# for one reason: it must implement the SAME 70 host members the manifest
# reaches, with the same C signatures, or the differential would be comparing
# two bindings over two different hosts.  Both sides of the differential — the
# Lua VM in a C runner and the Python binding over the C ABI — load THIS ONE
# object, so the mock's behaviour is identical by construction rather than by
# two implementations agreeing.

_MOCK_BANNER = '''\
/* jce_script_py_mock.gen.c -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * ONE recording mock host, loaded by BOTH sides of the cross-language
 * differential:
 *
 *   the Lua side   tests/scripting/python/lua_case_runner.c installs
 *                  jce_mock_host() into jce_script_create_sized and runs a
 *                  generated Lua chunk;
 *   the Python side tests/scripting/python/differential.py passes the same
 *                  table to jce_script_api_open through ctypes.
 *
 * Sharing the object is the point.  Two mocks written to agree would be a
 * second two-sided contract, and this repository's standing failure is exactly
 * that: two sides that agree with each other and are both wrong.
 *
 * EVERY call appends one line to a single ordered buffer that jce.log also
 * writes to, so the recorded stream interleaves what the script asked for with
 * what the host was asked to do.  Comparing those two streams IS the
 * differential: a missing call, an extra call, a permuted argument list, a
 * wrong capacity, a free that did not happen and a free that happened in the
 * wrong order are all a text difference.
 *
 * Canned answers are a pure function of the MEMBER NAME (an FNV-1a hash) and
 * the mode, never of call order, so a test that runs one case in isolation
 * sees the same values as a test that runs all of them.
 */

#include <jce/middleware/script/jce_script.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define JCE_MOCK_EXPORT __declspec(dllexport)
#else
#  define JCE_MOCK_EXPORT __attribute__((visibility("default")))
#endif

/* Modes.  Kept in sync with differential.py's MODES by that file's
 * check_mock_modes, which reads this list and that dict. */
#define JCE_MOCK_MODE_OK        0   /* every member present and answering */
#define JCE_MOCK_MODE_MISS      1   /* present, but answering "no" */
#define JCE_MOCK_MODE_ABSENT    2   /* every member NULL but log */
#define JCE_MOCK_MODE_NORELEASE 3   /* OK, except json_free is NULL */

#define JCE_MOCK_TRACE_CAP (1u << 20)

static char   g_trace[JCE_MOCK_TRACE_CAP];
static size_t g_trace_len;
static int    g_mode;
static int    g_live_strings;
static int    g_alloc_seq;

/* Allocation identity, so the trace can say WHICH string was released rather
 * than that some pointer was.  A raw %p would differ between the two runs and
 * make every trace comparison fail for a reason that is not a defect. */
#define JCE_MOCK_MAX_ALLOCS 256
static char *g_allocs[JCE_MOCK_MAX_ALLOCS];
static int   g_alloc_ids[JCE_MOCK_MAX_ALLOCS];
static int   g_alloc_count;

static void mock_emit(const char *text)
{
    size_t n = strlen(text);
    if (g_trace_len + n + 2u >= JCE_MOCK_TRACE_CAP)
        return;                      /* the count assertion catches truncation */
    memcpy(g_trace + g_trace_len, text, n);
    g_trace_len += n;
    g_trace[g_trace_len++] = '\\n';
    g_trace[g_trace_len] = '\\0';
}

static unsigned mock_hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h % 97u + 1u;
}

static const char *mock_str(const char *s)
{
    return s ? s : "<null>";
}

/* jce.log is the wire the Lua side writes CASE / RESULT lines through, and
 * jce_mock_note is the same wire for the Python side.  Both append verbatim,
 * so the two streams are directly comparable. */
static void mock_log(void *user, const char *msg)
{
    (void)user;
    mock_emit(mock_str(msg));
}

JCE_MOCK_EXPORT void jce_mock_note(const char *text)
{
    mock_emit(mock_str(text));
}

JCE_MOCK_EXPORT const char *jce_mock_trace_text(void)
{
    return g_trace;
}

JCE_MOCK_EXPORT void jce_mock_reset(int mode)
{
    g_trace_len = 0u;
    g_trace[0] = '\\0';
    g_mode = mode;
    g_live_strings = 0;
    g_alloc_seq = 0;
    g_alloc_count = 0;
    memset(g_allocs, 0, sizeof g_allocs);
}

/* Owned strings still held by nobody.  The differential asserts this is 0
 * after every owned_string_release case on BOTH sides -- which is how the
 * release is SEEN rather than assumed. */
JCE_MOCK_EXPORT int jce_mock_live_strings(void)
{
    return g_live_strings;
}

JCE_MOCK_EXPORT size_t jce_mock_host_size(void)
{
    return sizeof(JceScriptHost);
}

JCE_MOCK_EXPORT size_t jce_mock_sizeof_raycast_hit(void)
{
    return sizeof(JceScriptRaycastHit);
}

static char *mock_alloc_string(const char *member, size_t extra)
{
    char  *s;
    size_t head;
    size_t want = strlen(member) + extra + 64u;

    s = (char *)malloc(want);
    if (!s)
        return NULL;
    snprintf(s, want, "{\\"member\\":\\"%s\\",\\"n\\":%u,\\"pad\\":\\"", member,
             mock_hash(member));
    head = strlen(s);
    if (head + extra + 3u < want) {
        memset(s + head, '.', extra);
        s[head + extra] = '"';
        s[head + extra + 1] = '}';
        s[head + extra + 2] = '\\0';
    }
    if (g_alloc_count < JCE_MOCK_MAX_ALLOCS) {
        g_allocs[g_alloc_count] = s;
        g_alloc_ids[g_alloc_count] = ++g_alloc_seq;
        g_alloc_count++;
    }
    g_live_strings++;
    return s;
}

/* Find the id of a live allocation and RETIRE the slot.
 *
 * Retiring matters: malloc is free to hand the next allocation the address it
 * just took back, and a table that kept dead entries would then report the
 * OLD id for the NEW string.  Whether that happens depends on each process's
 * allocator history, so the two sides of the differential would disagree at
 * random -- a flaky trace difference that looks exactly like a real defect. */
static int mock_alloc_id(const char *p)
{
    int i;
    if (!p)
        return -1;               /* or NULL would match the first retired slot */
    for (i = 0; i < g_alloc_count; i++) {
        if (g_allocs[i] == p) {
            g_allocs[i] = NULL;
            return g_alloc_ids[i];
        }
    }
    return -1;
}

'''


def _trace_call(m: HostMember) -> list[str]:
    """The snprintf that records one call, arguments and all.

    in_params, not script_in_params: the `int max` capacity a first_and_count
    member receives is supplied by the BINDING, not by the script, and a
    binding that passed the wrong out_capacity would otherwise be invisible.
    Recording it makes `max=1024` versus `max=2` a text difference."""
    fmt, args = [], []
    for p in in_params(m):
        if p.arity > 0:
            fmt.append(f"{p.name}=[" + ",".join(["%.9g"] * p.arity) + "]")
            args += [f"(double){p.name}[{i}]" for i in range(p.arity)]
        elif p.c_type == "const char *":
            fmt.append(f"{p.name}=%s")
            args.append(f"mock_str({p.name})")
        elif p.c_type in ("float", "double"):
            fmt.append(f"{p.name}=%.9g")
            args.append(f"(double){p.name}")
        elif p.c_type == "bool":
            fmt.append(f"{p.name}=%s")
            args.append(f"({p.name} ? \"true\" : \"false\")")
        elif p.c_type in ("uint64_t", "JceScriptEntity"):
            fmt.append(f"{p.name}=%llu")
            args.append(f"(unsigned long long){p.name}")
        elif p.c_type == "uint32_t":
            fmt.append(f"{p.name}=%u")
            args.append(f"(unsigned){p.name}")
        else:
            fmt.append(f"{p.name}=%d")
            args.append(f"(int){p.name}")
    text = f"CALL {m.name}(" + ", ".join(fmt) + ")"
    line = f'    snprintf(line, sizeof line, "{text}"'
    if args:
        line += ",\n             " + ", ".join(args)
    return [line + ");", "    mock_emit(line);"]


def _mock_body(m: HostMember, header_text: str) -> list[str]:
    """One mock member: record, then answer deterministically for the mode."""
    L: list[str] = []
    A = L.append
    outs = out_params(m)
    A("    char line[1024];")
    A("    unsigned h = mock_hash(\"" + m.name + "\");")
    A("")
    A("    (void)user;")
    A("    (void)h;")
    L += _trace_call(m)

    # EVERY out slot gets a DIFFERENT value, and that is the whole point of the
    # running counter.  A mock that filled x, y and pressure with the same
    # number would let a binding that permuted them pass the differential --
    # the values are compared slot by slot, so identical values make a
    # permutation invisible.  Measured on the first draft: touch_get's three
    # floats and raycast's point/normal were all `h + 0.5f`.
    slot = [0]

    def fill(o: Param) -> list[str]:
        body: list[str] = []

        def one(lhs: str, t: str) -> None:
            i = slot[0]
            slot[0] += 1
            if t == "bool":
                body.append(f"    {lhs} = {'true' if i % 2 == 0 else 'false'};")
            elif t in ("float", "double"):
                sfx = "f" if t == "float" else ""
                body.append(f"    {lhs} = ({t})(h + {i}u) + 0.25{sfx};")
            else:
                body.append(f"    {lhs} = ({t})(h + {i}u);")

        if o.arity > 0:
            for i in range(o.arity):
                one(f"{o.name}[{i}]", o.c_type)
        elif o.c_type in CTYPE:
            one(f"*{o.name}", o.c_type)
        else:
            for f, t in flatten_pod(header_text, o.c_type):
                one(f"{o.name}->{f}", t)
        return body

    if m.ret == "void":
        for o in outs:
            L += fill(o)
        return L

    if m.ret == "bool":
        if outs:
            A("    if (g_mode == JCE_MOCK_MODE_MISS)")
            A("        return false;")
            for o in outs:
                L += fill(o)
            A("    return true;")
        else:
            A("    return g_mode != JCE_MOCK_MODE_MISS;")
        return L

    if m.ret == "int":
        if outs:
            # find_by_name / find_by_prefix: an honest host never answers more
            # than the capacity it was handed, so the count is clamped here.
            # The negative answer in MISS mode is what exercises clamp_min on
            # touch_count AND a negative count on both list shapes.
            o = outs[0]
            cap = [p for p in m.params if p.c_type == "int" and p.arity == 0][-1]
            A("    if (g_mode == JCE_MOCK_MODE_MISS)")
            A("        return -3;")
            A(f"    n = ({cap.name} < 3) ? {cap.name} : 3;")
            A("    for (i = 0; i < n; i++)")
            A(f"        {o.name}[i] = ({o.c_type})(h * 100u + (unsigned)i);")
            A("    return n;")
            L.insert(1, "    int n, i;")
        else:
            A("    return (g_mode == JCE_MOCK_MODE_MISS) ? -3 : (int)h;")
        return L

    if m.ret in ("float", "double"):
        suffix = "f" if m.ret == "float" else ""
        A(f"    if (g_mode == JCE_MOCK_MODE_MISS)")
        A(f"        return -1.5{suffix};")
        A(f"    return ({m.ret})h + 0.5{suffix};")
        return L

    if m.ret in ("uint64_t", "JceScriptEntity", "uint32_t"):
        A("    if (g_mode == JCE_MOCK_MODE_MISS)")
        A("        return 0;")
        A(f"    return ({m.ret})h;")
        return L

    if m.ret == "const char *":
        # A non-ASCII answer on purpose: the Python binding decodes bytes to
        # str and Lua pushes the bytes verbatim, so an ASCII-only mock would
        # leave the decode untested on the only surface that performs one.
        A("    if (g_mode == JCE_MOCK_MODE_MISS)")
        A("        return NULL;")
        # The UTF-8 bytes for "é中", spelled as hex escapes rather than as
        # é中.  A universal character name is encoded in the
        # EXECUTION character set, which MSVC takes from the ANSI code page
        # unless /utf-8 is passed -- the Lua side would then push cp1252 bytes
        # while the Python side decodes UTF-8, and the differential would go
        # red for a compiler setting rather than for a defect.  Adjacent string
        # literals keep each \x escape from swallowing the next character.
        A(f'    return "{m.name}/" "\\xc3\\xa9" "\\xe4\\xb8\\xad";')
        return L

    if m.ret in ("char *", "char*"):
        A("    if (g_mode == JCE_MOCK_MODE_MISS)")
        A("        return NULL;")
        A(f'    return mock_alloc_string("{m.name}", g_long_strings ? '
          "OWNED_LONG_EXTRA : 0u);")
        return L

    raise SystemExit(f"error: emit_python: mock has no rule for return type "
                     f"{m.ret!r} on {m.name}")


def _release_body(m: HostMember) -> list[str]:
    """json_free: record WHICH allocation was released, and account for it."""
    L = ["    char line[1024];", "    int id;", "",
         "    (void)user;",
         f"    id = mock_alloc_id({m.params[1].name});",
         f'    snprintf(line, sizeof line, "CALL {m.name}(alloc#%d)", id);',
         "    mock_emit(line);",
         f"    free({m.params[1].name});",
         "    g_live_strings--;"]
    return L


def _c_decl(m: HostMember) -> str:
    ps = []
    for p in m.params:
        if p.arity > 0:
            ps.append(f"{p.c_type} {p.name}[{p.arity}]")
        elif p.arity < 0:
            ps.append(f"{p.c_type} *{p.name}")
        else:
            ps.append(f"{p.c_type} {p.name}")
    sep = "" if m.ret.endswith("*") else " "
    return f"static {m.ret}{sep}mock_{m.name}({', '.join(ps)})"


def emit_mock_c(members: list[HostMember], man: dict) -> str:
    by = {m.name: m for m in members}
    header_text = HEADER.read_text(encoding="utf-8")
    releases = {e["release"] for e in man["expose"] if e.get("release")}
    reached: list[str] = []
    for e in man["expose"]:
        if e["vtable"] not in reached:
            reached.append(e["vtable"])
    for r in sorted(releases):
        if r not in reached:
            reached.append(r)

    L = [_MOCK_BANNER]
    A = L.append
    A("/* Owned strings are made longer than "
      "OWNED_STRING_INITIAL_CAPACITY in one mode, to reach the C ABI's")
    A(" * copy-out retry path from the Python side. */")
    A("#define OWNED_LONG_EXTRA 9000u")
    A("static int g_long_strings;")
    A("")
    A("JCE_MOCK_EXPORT void jce_mock_set_long_strings(int on)")
    A("{")
    A("    g_long_strings = on;")
    A("}")
    A("")
    for name in reached:
        m = by[name]
        A(_c_decl(m))
        A("{")
        L += (_release_body(m) if name in releases and name not in
              {e["vtable"] for e in man["expose"]} else _mock_body(m, header_text))
        A("}")
        A("")

    A("/* Four host tables, one per mode.  ABSENT keeps only `log`, which is")
    A(" * the trace wire and not part of the generated surface. */")
    A("static JceScriptHost g_host;")
    A("")
    A("JCE_MOCK_EXPORT const JceScriptHost *jce_mock_host(int mode)")
    A("{")
    A("    memset(&g_host, 0, sizeof g_host);")
    A("    g_host.log = mock_log;")
    A("    if (mode == JCE_MOCK_MODE_ABSENT)")
    A("        return &g_host;")
    for name in reached:
        A(f"    g_host.{name} = mock_{name};")
    A("    if (mode == JCE_MOCK_MODE_NORELEASE) {")
    for r in sorted(releases):
        A(f"        g_host.{r} = NULL;")
    A("    }")
    A("    return &g_host;")
    A("}")
    return "\n".join(L).rstrip("\n") + "\n"


# ── The backend ──────────────────────────────────────────────────────────────

_DECL_RE = re.compile(r"^JCE_SCRIPT_API\s+.*?\b(jce_script_api_[a-z0-9_]+)\s*\(",
                      re.M | re.S)


class PythonBackend(ScriptBackend):
    name = "python"

    def artefacts(self, members: list[HostMember], man: dict) -> list[Artefact]:
        return [Artefact(GEN_PY, emit_module),
                Artefact(GEN_PYI, emit_stub),
                Artefact(MOCK_C, emit_mock_c)]

    def validate(self, members: list[HostMember], man: dict,
                 c_text: str) -> list[str]:
        """This backend's own condition: every symbol the ctypes module
        resolves is declared by the COMMITTED C ABI header.

        Not neutral, and not covered by anything else.  The Lua backend's
        registration parity reads jce_script.c; the C ABI's own gate checks its
        artefacts against script-api.json.  Nothing checks that the ctypes
        declarations name symbols that exist -- a rename in
        gen_script_c_abi.py's PREFIX or META would leave this module raising
        MissingExportError at load time, in a test that has to run a DLL to
        find out.  This turns that into a manifest-time failure naming the
        symbol.

        tests/scripting/python/test_emit_python.py's
        test_a_missing_c_abi_declaration_is_named is what fails.  It has to
        feed validate() a synthetic entry: this condition cannot fail against a
        clean tree, so without that test it could be emptied and the generator
        would go on printing OK."""
        problems: list[str] = []
        if not C_ABI_HEADER.is_file():
            return [f"emit_python: {C_ABI_HEADER.relative_to(REPO_ROOT).as_posix()}"
                    f" is missing -- the ctypes module binds to that library and "
                    f"cannot be checked against a header that is not there"]
        declared = set(_DECL_RE.findall(
            C_ABI_HEADER.read_text(encoding="utf-8")))
        if len(declared) < len(man["expose"]):
            problems.append(
                f"emit_python: the C ABI header declares {len(declared)} "
                f"jce_script_api_* entry points but the manifest carries "
                f"{len(man['expose'])} + {len(META)} meta -- the declaration "
                f"scan matched too little to be checking anything")
        for name in [PREFIX + m for m in META] + \
                    [PREFIX + e["name"] for e in man["expose"]]:
            if name not in declared:
                problems.append(
                    f"emit_python: {name} is not declared in "
                    f"{C_ABI_HEADER.relative_to(REPO_ROOT).as_posix()}, but "
                    f"_generated.py resolves it by name from the shared "
                    f"library")
        return problems


BACKEND = PythonBackend()
