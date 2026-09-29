#!/usr/bin/env python3
"""
emit_cpp.py — the C++ backend.  One of N; see scriptgen_core.py's docstring for
how a backend registers itself without touching any existing file.

THERE IS NO C BACKEND HERE, AND THAT IS THE FINDING, NOT AN OMISSION
────────────────────────────────────────────────────────────────────
The scripting surface's C binding already exists and is already generated:
`scripting/c_abi/include/jce/script_api/jce_script_api.h`, emitted by
tools/scriptgen/gen_script_c_abi.py from the same manifest.  It is plain C99,
it takes an opaque `JceScriptApi *`, and every entry is a real exported symbol
whose presence-and-nothing-else is asserted at the DLL's export table.  A
second "C binding" underneath it would be a wrapper around a wrapper with no
marshalling to do — the two languages are the same language.  So this backend
emits nothing for C, and `validate()` below CHECKS that the C header really
does declare every manifest entry, so "C is already done" is a measured claim
rather than a sentence in a report.

WHAT THIS BACKEND DOES EMIT
───────────────────────────
1. `scripting/cpp/include/jce/script_api/jce_script_api.hpp` — a header-only
   C++ wrapper over that C ABI.  It earns its existence in four ways and no
   others:

     * RAII.  `jce::script::Api` owns the opaque `JceScriptApi *`, closes it in
       its destructor, and is move-only.  A raw handle leaked past a `return`
       is the only resource this surface has.
     * `owned_string_release` becomes a type that CANNOT leak.  The C entry is
       `int f(..., char *out, int out_cap)` and the caller must size a buffer;
       the C++ entry is `std::optional<std::string>` and no `char *` ever
       reaches the user.  (The C ABI already releases the host's string before
       returning, so there was never a free() obligation to cross — what the
       C++ type removes is the BUFFER, not the ownership.)
     * The seven closed shapes become C++ types instead of out parameters:
       `std::optional<T>` for fallible_out, `std::array<float,N>` for the
       fixed-arity out arrays, `std::vector<Entity>` for entity_table.
     * `index_base`.  The Lua binding refuses an index below its base WITHOUT
       calling the host; the C ABI passes the index straight through.  The
       wrapper restores the refusal at the 0-based floor, which is the one
       place it adds behaviour rather than sugar — and the differential's
       `below the index base` case is what fails if it is dropped.

2. `tests/scripting/cpp/test_jce_script_cpp_differential.gen.cpp` — the
   CROSS-LANGUAGE DIFFERENTIAL.  Batch 1 spent the free oracle (the
   hand-written Lua bindings) and said so; what is left is that LUA IS THE
   REFERENCE IMPLEMENTATION.  So every case is driven through a real Lua VM
   and through this wrapper over ONE recording mock host, and both the full
   result (type AND value per slot) and the host-call trace are compared.

   Non-circular in the one way that matters: the C++ result is projected onto
   the comparison form by HAND-WRITTEN, TYPE-KEYED overloads in
   tests/scripting/cpp/jce_script_cpp_differential.hpp — never by anything this
   file emits.  A per-entry defect in the wrapper therefore cannot be mirrored
   by a per-entry defect in the projection, because there is no per-entry
   projection.

   The one self-cancelling axis, stated because it cannot be removed: both
   sides' ARGUMENT VALUES come from `_arg_values` below, so a transposition
   THERE permutes both sides identically and is invisible.  A transposition in
   the WRAPPER is visible, because the values are distinct per slot and the
   host-call trace records them in the order the host received them.  M1 of
   batch 1 is the same shape and is why this paragraph exists.

WHAT THIS BACKEND DELIBERATELY DOES NOT EXPOSE
──────────────────────────────────────────────
The seven `hand_written` manifest entries, as a class, by rule — identical to
the C ABI's exclusion.  `asset_read_text` / `asset_read_json` carry sandbox
policy that lives in `static` functions inside jce_script.c; a C++ convenience
wrapper reaching around them to the raw `read_file` member is the escape the
manifest calls P0-2.  The emitted header prints all seven with the manifest's
own `reason` text.

Its tests: tests/scripting/cpp/  (the differential, the contract tests, and the
C++17 floor).  This file's own conditions run inside
`python tools/scriptgen/gen_script_bindings.py`.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from scriptgen_core import (                               # noqa: E402
    Artefact,
    HostMember,
    Param,
    REPO_ROOT,
    ScriptBackend,
    capacity_param,
    in_params,
    out_params,
    script_in_params,
)

HPP = REPO_ROOT / "scripting/cpp/include/jce/script_api/jce_script_api.hpp"
DIFF_CPP = REPO_ROOT / "tests/scripting/cpp/test_jce_script_cpp_differential.gen.cpp"

# The C ABI's own committed header.  READ ONLY, and read for one purpose: to
# prove the entry this backend is about to wrap actually exists over there.
C_ABI_HEADER = REPO_ROOT / "scripting/c_abi/include/jce/script_api/jce_script_api.h"

C_PREFIX = "jce_script_api_"

# How many bytes of an owned string the wrapper takes on the stack before it
# has to ask a second time.  See `_emit_owned_string` for what the second ask
# costs.  The measurement that pins this number lives in the C++ suite, which
# this repository does not track; it is described here rather than cited by
# name, because a name nobody can resolve reads the same whether the test is
# missing or misspelt.
INLINE_OWNED_STRING_BYTES = 256


# ── C type -> C++ type ───────────────────────────────────────────────────────
#
# `const char *` is TWO different C++ types depending on direction, and that is
# the whole reason this is a pair of tables rather than one:
#
#   IN   -> jce::script::CStr, which converts implicitly from `const char *`
#           and from `const std::string &` (via c_str(), no copy) and NOT from
#           std::string_view -- a string_view is not NUL-terminated, so a
#           string_view parameter would force this wrapper to copy every string
#           argument in order to add a NUL.  That is a heap allocation per call
#           inside on_update, which is the cost model owner decision 8 rejected
#           JNA for.  The refusal is deliberate and is why CStr exists at all.
#   OUT  -> std::string, an owned copy.  That is exactly what the Lua binding
#           does (lua_pushstring copies), so the C++ and Lua lifetimes agree.
_CPP_IN = {
    "float": "float",
    "double": "double",
    "bool": "bool",
    "int": "int",
    "uint32_t": "std::uint32_t",
    "uint64_t": "std::uint64_t",
    "JceScriptEntity": "Entity",
    "const char *": "CStr",
}

_CPP_OUT = {
    "float": "float",
    "double": "double",
    "bool": "bool",
    "int": "int",
    "uint32_t": "std::uint32_t",
    "uint64_t": "std::uint64_t",
    "JceScriptEntity": "Entity",
    "const char *": "std::string",
    "char *": "std::string",
}

# Out-parameter C types that are a POD STRUCT rather than a scalar.  The value
# is the C++ spelling the wrapper hands back.
_POD_OUT = {"JceScriptRaycastHit": "RaycastHit"}

# Identifiers a generated method must not take.  The first group are the RAII
# handle's own members -- a manifest entry named `close` would emit a second
# `close()` and the overload would compile, silently shadowing the destructor's
# partner.  The second group is C++ itself.
_RESERVED_METHOD_NAMES = frozenset({
    "open", "close", "get", "Api", "operator",
    "and", "asm", "auto", "bool", "break", "case", "catch", "char", "class",
    "const", "continue", "default", "delete", "do", "double", "else", "enum",
    "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
    "if", "inline", "int", "long", "mutable", "namespace", "new", "not",
    "nullptr", "operator", "or", "private", "protected", "public", "register",
    "return", "short", "signed", "sizeof", "static", "struct", "switch",
    "template", "this", "throw", "true", "try", "typedef", "typename", "union",
    "unsigned", "using", "virtual", "void", "volatile", "while", "xor",
})

_IDENT_RE = re.compile(r"^[A-Za-z_]\w*$")


def _camel(name: str) -> str:
    return "".join(part[:1].upper() + part[1:] for part in name.split("_"))


def _result_struct(name: str) -> str:
    """The C++ type a multi-out fallible_out entry hands back.

    MECHANICAL, not a hand-maintained mapping: get_touch -> GetTouchResult.  A
    table would be a second place to edit when an entry is added."""
    return _camel(name) + "Result"


def _wrap(text: str, prefix: str, width: int = 78) -> list[str]:
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


def _fmt_g(v) -> str:
    """A number as `%.9g` would print it -- the one numeric spelling shared by
    the Lua canonicaliser and the C++ one."""
    if isinstance(v, bool):
        return "true" if v else "false"
    return f"{float(v):.9g}"


def _cpp_real(v, suffix: str) -> str:
    """A real number as a C++ floating literal.

    ALWAYS with a decimal point: `%.9g` renders 0.0 as `0`, and `0f` is not a
    C++ token -- it is an invalid integer literal with a stray suffix, and the
    error names the line rather than the emitter.  Caught by the first compile
    of the generated header; recorded here so the next person does not
    re-derive it from a C2059."""
    s = f"{float(v):.9g}"
    if "." not in s and "e" not in s and "E" not in s and "inf" not in s:
        s += ".0"
    return s + suffix


def _cpp_default(c_type: str, v) -> str:
    """A manifest `optional` default rendered as a C++ initialiser."""
    if v is None:
        return "CStr()" if c_type == "const char *" else "{}"
    if isinstance(v, bool):
        return "true" if v else "false"
    if c_type == "float":
        return _cpp_real(v, "f")
    if c_type == "double":
        return _cpp_real(v, "")
    if c_type == "const char *":
        return f'CStr("{v}")'
    return str(v)


# ── One entry, decomposed the way BOTH emitted files need it ─────────────────

class Entry:
    """The C++ view of one manifest `expose` entry.

    Built ONCE and shared by the header emitter and the differential emitter,
    because the two must agree about the C-ABI call they describe.  What they
    must NOT share is the projection of the RESULT -- that is hand-written and
    type-keyed, in tests/scripting/cpp/jce_script_cpp_differential.hpp."""

    def __init__(self, m: HostMember, ent: dict):
        self.m = m
        self.ent = ent
        self.name = ent["name"]
        self.shape = ent["shape"]
        self.bind = ent.get("bind_args") or {}
        self.opt = ent.get("optional") or {}
        self.cap = capacity_param(m, self.shape)
        self.ins = [p for p in script_in_params(m, ent) if p.name not in self.bind]
        self.outs = out_params(m)
        self.c_fn = C_PREFIX + self.name

        # C++ default arguments must be TRAILING.  gas_apply's `op` is optional
        # and is followed by a REQUIRED `magnitude`, so it cannot carry one --
        # the value still ships, as a named constant in `defaults`, and the
        # header says why.  Lua has no such restriction (it reads nil in place),
        # which is exactly the sort of per-language difference the manifest
        # keeps OUT of itself.
        self.defaulted: set[str] = set()
        for p in reversed(self.ins):
            if p.name in self.opt:
                self.defaulted.add(p.name)
            else:
                break

    # -- the C++ signature -----------------------------------------------
    def params_decl(self, allow_defaults: bool = True) -> list[str]:
        """The C++ parameter list.

        allow_defaults=False when the caller appends further parameters after
        these (a std::span<Entity> out, say).  C++ forbids a defaulted
        parameter before a non-defaulted one, so an `optional` modifier on a
        table-returning entry emitted a header that would not compile:

            int overlap_sphere(float x, float y, float z, float radius,
                               uint32_t layer_mask = defaults::...,
                               std::span<Entity> out) const

        Every other language keeps the optional argument; C++ asks for it
        explicitly, which is the only shape the language allows without
        reordering the parameters away from the C ABI's own."""
        out = []
        for p in self.ins:
            if p.arity > 0:                      # `const float origin[3]`
                base = p.c_type[len("const "):] if p.c_type.startswith("const ") \
                    else p.c_type
                t = f"const std::array<{_CPP_IN[base]}, {p.arity}> &"
                out.append(f"{t}{p.name}")
                continue
            t = _CPP_IN[p.c_type]
            d = (f" = defaults::{self.name}_{p.name}"
                 if (allow_defaults and p.name in self.defaulted) else "")
            sep = "" if t.endswith("&") else " "
            out.append(f"{t}{sep}{p.name}{d}")
        return out

    def c_call_args(self, out_expr: dict[str, str],
                    extra: list[str] | None = None) -> list[str]:
        """The argument list of the C ABI entry point, in DECLARATION order.

        Rebuilt from the same vtable member gen_script_c_abi.py rebuilt it
        from.  That is a second derivation of one contract -- and it is checked
        by the C++ COMPILER, because tests/scripting/cpp compiles this header
        against the real jce_script_api.h.  A drifted derivation is a build
        error there, not a silent divergence."""
        args = ["h_"]
        for p in self.m.params[1:]:
            if p.name in self.bind:
                continue                          # the C ABI supplies it
            if self.cap is not None and p.name == self.cap.name:
                args.append(out_expr["__capacity__"])
                continue
            if p.name in out_expr:
                args.append(out_expr[p.name])
                continue
            if p.arity > 0:
                args.append(f"{p.name}.data()")
            elif p.c_type == "const char *":
                args.append(f"{p.name}.c_str()")
            else:
                args.append(p.name)
        return args + (extra or [])

    # -- the C++ return type ---------------------------------------------
    def return_type(self) -> str:
        s = self.shape
        if s == "void_call":
            return "void"
        if s == "value_return":
            return _CPP_OUT[self.m.ret]
        if s == "void_out_array":
            o = self.outs[0]
            return f"std::array<{_CPP_OUT[o.c_type]}, {o.arity}>"
        if s == "fallible_out":
            return f"std::optional<{self.success_type()}>"
        if s == "first_and_count":
            return "FirstAndCount"
        if s == "entity_table":
            return "std::vector<Entity>"
        if s == "owned_string_release":
            return "std::optional<std::string>"
        raise SystemExit(f"error: {self.name}: emit_cpp has no rendering for "
                         f"shape {s!r} — mark the binding hand_written rather "
                         f"than growing an eighth shape here")

    def success_type(self) -> str:
        """What a fallible_out yields when it succeeds."""
        outs = self.outs
        if len(outs) == 1:
            o = outs[0]
            if o.arity > 0:
                return f"std::array<{_CPP_OUT[o.c_type]}, {o.arity}>"
            if o.c_type in _POD_OUT:
                return _POD_OUT[o.c_type]
            return _CPP_OUT[o.c_type]
        return _result_struct(self.name)

    def needs_result_struct(self) -> bool:
        return self.shape == "fallible_out" and len(self.outs) > 1

    def is_noexcept(self) -> bool:
        """Every entry that cannot allocate.  std::string / std::vector can."""
        t = self.return_type()
        return "std::string" not in t and "std::vector" not in t


def _entries(members: list[HostMember], man: dict) -> list[Entry]:
    by = {m.name: m for m in members}
    return [Entry(by[e["vtable"]], e) for e in man["expose"]]


# ── Artefact 1: the C++ header ───────────────────────────────────────────────

_HPP_HEAD = '''\
/* jce_script_api.hpp -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * A header-only C++ wrapper over the script-facing C ABI.
 *
 * THE C BINDING IS NOT HERE, BECAUSE IT ALREADY EXISTS.  <jce/script_api/
 * jce_script_api.h>, generated beside this file from the same manifest, IS the
 * C binding of the scripting surface: plain C99, an opaque handle, one
 * exported symbol per manifest entry.  A C consumer includes that header and
 * links jce_script_api, and needs nothing from this directory.  This file adds
 * only what C cannot express.
 *
 * WHAT IT ADDS, and nothing else:
 *   * RAII.  `Api` owns the `JceScriptApi *`, closes it in ~Api, and is
 *     move-only.  The handle is the only resource on this surface.
 *   * The seven closed shapes as C++ TYPES rather than out parameters:
 *     std::optional<T>, std::array<float,N>, std::vector<Entity>, and
 *     std::optional<std::string> for the owned-string shape -- which is the
 *     one that could leak, and now cannot, because no char * and no buffer
 *     size ever reach the caller.
 *   * The manifest's `optional` defaults, as C++ default arguments where C++
 *     allows them (they must be trailing) and as named constants in
 *     `jce::script::defaults` where it does not.
 *   * The manifest's `index_base` floor, restored.  This is the ONLY place the
 *     wrapper adds behaviour instead of sugar; see get_touch below.
 *
 * WHAT IT DOES NOT ADD:  the seven hand-written manifest entries, excluded as
 * a class exactly as the C ABI excludes them.  Their reasons are printed
 * below, verbatim from the manifest.  Reaching around them to a raw host
 * member is the sandbox escape the manifest calls P0-2, and a C++ convenience
 * wrapper is the most tempting place in the tree to do it.
 *
 * LANGUAGE LEVEL.  REQUIRES C++17 -- and requires it in the enforced sense:
 * tests/scripting/cpp/test_jce_script_cpp17_floor.cpp compiles this header at
 * -std=c++17 and runs it, so the claim fails a build rather than a code
 * review.  C++20 is USED, never required: when <span> is available the
 * entity-table entries gain an additional caller-buffer overload, guarded on
 * __cpp_lib_span.  An SDK consumer on C++17 loses that overload and nothing
 * else.
 *
 * ABSENT MEMBERS, and a DEFAULT-CONSTRUCTED Api.  Every entry point of the C
 * ABI already answers `!api` and `!api->host.<member>` with the absent value,
 * so calling any method on a closed or never-opened `Api` is defined and
 * returns exactly what the Lua binding pushes with no host: nullopt / 0 /
 * false / an empty array.  Nothing here needs to re-check the handle, and
 * [doctest] "an empty handle answers like an absent host" is what fails if
 * that stops being true.
 */
#ifndef JCE_SCRIPT_API_HPP
#define JCE_SCRIPT_API_HPP

#include <jce/script_api/jce_script_api.h>

/* MSVC reports 199711L in __cplusplus unless /Zc:__cplusplus is passed, and
 * reports the real level in _MSVC_LANG either way.  Reading only __cplusplus
 * would reject every default MSVC build of a perfectly conforming C++17
 * consumer. */
#if defined(_MSVC_LANG)
#  define JCE_SCRIPT_CPP_LANG _MSVC_LANG
#else
#  define JCE_SCRIPT_CPP_LANG __cplusplus
#endif

#if JCE_SCRIPT_CPP_LANG < 201703L
#  error "jce_script_api.hpp requires C++17 or later"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/* C++20's std::span, USED and not required.  __cpp_lib_span is the library
 * feature-test macro, which is the only thing that answers "can I include
 * <span>"; a bare __cplusplus >= 202002L test is a compile error on a toolchain
 * whose front end is C++20 and whose library is not. */
#if defined(__has_include)
#  if __has_include(<version>)
#    include <version>
#  endif
#endif
#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L
#  include <span>
#  define JCE_SCRIPT_CPP_HAS_SPAN 1
#else
#  define JCE_SCRIPT_CPP_HAS_SPAN 0
#endif

namespace jce {
namespace script {

/* The entity id as scripts see it. */
using Entity = JceScriptEntity;

/* raycast's POD result, unchanged -- restating it would be a second
 * declaration of an engine type, which this layer does not do. */
using RaycastHit = JceScriptRaycastHit;

/* A NUL-terminated string argument.
 *
 * Converts implicitly from `const char *` (including a literal) and from
 * `const std::string &` -- c_str() is already NUL-terminated, so neither
 * conversion copies.  It deliberately does NOT convert from std::string_view:
 * a view is not NUL-terminated, the C ABI needs a `const char *`, and the only
 * way to bridge that is to copy the bytes to add a NUL -- a heap allocation
 * per call, in on_update.  The omission is the point.
 *
 * A default-constructed CStr is a null pointer, which is what the Lua binding
 * passes for an omitted nil-able string argument (send_message, broadcast,
 * rpc_send). */
class CStr {
public:
    constexpr CStr() noexcept : p_(nullptr) {}
    constexpr CStr(const char *s) noexcept : p_(s) {}       /* NOLINT: implicit */
    CStr(const std::string &s) noexcept : p_(s.c_str()) {}  /* NOLINT: implicit */
    constexpr const char *c_str() const noexcept { return p_; }
    constexpr explicit operator bool() const noexcept { return p_ != nullptr; }
private:
    const char *p_;
};

/* The first_and_count shape.  Lua returns (first_or_nil, count); the same two
 * facts, with the "or nil" in the type instead of in a convention.  `count` is
 * the host's own answer and MAY exceed the manifest's out_capacity -- that is
 * the point of the shape: a scene director can reject duplicate authored names
 * it did not receive. */
struct FirstAndCount {
    std::optional<Entity> first;
    int                   count = 0;
};

'''

_HPP_API_HEAD = '''\
/* The RAII handle.
 *
 * Move-only: two owners would close one handle twice.  A default-constructed
 * Api is EMPTY and every method on it is still callable -- see the header
 * banner.  There is no throwing constructor and no exception on this surface;
 * `open` reports failure by returning an empty Api, which `operator bool`
 * answers. */
class Api {
public:
    Api() noexcept = default;

    /* Bind to a host.
     *
     * `host_size` defaults to the CALLER's sizeof(JceScriptHost) -- and it is
     * the caller's precisely BECAUSE this wrapper is header-only.  The default
     * argument is evaluated in the consumer's translation unit, against the
     * consumer's own copy of the engine header.  A compiled wrapper would bake
     * ITS sizeof into the library and hand the C ABI a number the caller never
     * agreed to, defeating the min(caller, engine) copy that makes a short host
     * safe.  What fails if this becomes a fixed number is that the caller's
     * host_size stops being the number that reaches the C ABI -- the property
     * the C++ suite measures.  That suite is not part of this repository, so
     * it is named here and not cited: an unresolvable citation reads the same
     * whether the test is missing or renamed.
     *
     * Returns an empty Api when the host is null, the size is 0, or
     * `script_api_min` is newer than the loaded library. */
    static Api open(const JceScriptHost &host,
                    std::size_t host_size = sizeof(JceScriptHost),
                    std::uint32_t script_api_min = JCE_SCRIPT_API_VERSION) noexcept
    {
        return Api(jce_script_api_open(&host, host_size, script_api_min));
    }

    ~Api() { close(); }

    Api(Api &&other) noexcept : h_(other.h_) { other.h_ = nullptr; }

    Api &operator=(Api &&other) noexcept
    {
        if (this != &other) {
            close();
            h_ = other.h_;
            other.h_ = nullptr;
        }
        return *this;
    }

    Api(const Api &) = delete;
    Api &operator=(const Api &) = delete;

    void close() noexcept
    {
        if (h_ != nullptr) {
            jce_script_api_close(h_);
            h_ = nullptr;
        }
    }

    explicit operator bool() const noexcept { return h_ != nullptr; }

    /* The raw handle, for a caller that must reach an entry point this
     * wrapper does not name.  Ownership does not move. */
    JceScriptApi *get() const noexcept { return h_; }

    /* How many bytes of an owned string are taken on the stack before the
     * wrapper has to ask the host a second time.  Public because that second
     * ask is an OBSERVABLE second host call, and a caller tuning for a
     * frame budget is entitled to know where the edge is. */
    static constexpr int kOwnedStringInlineBytes = @INLINE@;

'''

_HPP_TAIL = '''\
private:
    explicit Api(JceScriptApi *h) noexcept : h_(h) {}

    JceScriptApi *h_ = nullptr;
};

}  /* namespace script */
}  /* namespace jce */

#endif /* JCE_SCRIPT_API_HPP */
'''


def _method_comment(e: Entry) -> list[str]:
    L = [f"    /* jce.{e.name} -- shape: {e.shape}, since {e.ent['since']}"]
    if e.ent.get("doc"):
        L += _wrap(e.ent["doc"], "     * ")
    if e.bind:
        bound = ", ".join(f"{k}={v}" for k, v in e.bind.items())
        L += _wrap(f"The C ABI supplies {bound} (manifest bind_args), so it is "
                   f"not a parameter here either.", "     * ")
    if e.ent.get("index_base"):
        L += _wrap(f"INDEX BASE {e.ent['index_base']}: the Lua binding takes a "
                   f"{e.ent['index_base']}-based index and answers nil for "
                   f"anything below it WITHOUT calling the host. This wrapper "
                   f"is 0-based like the C ABI, so the same refusal sits at 0 "
                   f"-- and it is added HERE, because the C ABI passes the "
                   f"index straight through.", "     * ")
    if isinstance(e.ent.get("absent_value"), dict):
        L += _wrap(f"Absent host, or a host that answers NULL: returns "
                   f"`{e.ent['absent_value']['param']}` itself. The C entry "
                   f"point returns NULL in the second case; normalising it is "
                   f"this wrapper's job, and the differential's MOCK_FAIL "
                   f"sweep is what fails if it stops doing it.", "     * ")
    elif e.ent.get("absent_value") is not None:
        L += _wrap(f"When the host member is absent this returns "
                   f"{e.ent['absent_value']}, not 0.", "     * ")
    if "clamp_min" in e.ent:
        L += _wrap(f"Clamped by the C ABI to a minimum of {e.ent['clamp_min']}.",
                   "     * ")
    if e.ent.get("strict"):
        L += _wrap(f"The manifest marks {', '.join(e.ent['strict'])} `strict` "
                   f"-- Lua type-checks it because Lua cannot do so at compile "
                   f"time. Here the parameter IS a bool and the compiler is the "
                   f"check, so nothing is emitted for it.", "     * ")
    for p in e.ins:
        if p.name in e.opt and p.name not in e.defaulted:
            L += _wrap(f"`{p.name}` is optional in the manifest but is followed "
                       f"by a required parameter, and a C++ default argument "
                       f"must be trailing. Pass "
                       f"defaults::{e.name}_{p.name} for the manifest value.",
                       "     * ")
    if e.shape == "owned_string_release":
        L += _wrap(f"Returns an owned copy, or nullopt when the host has no "
                   f"answer. Nothing to free: the C ABI released the host's "
                   f"string through `{e.ent['release']}` before returning. A "
                   f"result longer than kOwnedStringInlineBytes-1 costs a "
                   f"SECOND call to the host -- the string was already released "
                   f"and there is no other way to reach the tail.", "     * ")
    L.append("     */")
    return L


def _emit_defaults(entries: list[Entry]) -> list[str]:
    """Every `optional` default, as a named constant.

    ALL of them, including the ones that also become C++ default arguments:
    one mechanism means the trailing / non-trailing split changes where the
    value is USED and never whether it exists."""
    L = ["/* The manifest's `optional` defaults.  Named because C++ default",
         " * arguments must be trailing and some of these are not -- and named",
         " * for ALL of them, so which ones those are can change without the",
         " * others moving. */",
         "namespace defaults {"]
    n = 0
    for e in entries:
        for p in e.ins:
            if p.name not in e.opt:
                continue
            v = e.opt[p.name]
            t = _CPP_IN[p.c_type]
            L.append(f"inline constexpr {t} {e.name}_{p.name} = "
                     f"{_cpp_default(p.c_type, v)};")
            n += 1
    L += ["}  /* namespace defaults */", ""]
    return L if n else []


def _emit_result_structs(entries: list[Entry]) -> list[str]:
    L: list[str] = []
    for e in entries:
        if not e.needs_result_struct():
            continue
        L += [f"/* {e.name}: {len(e.outs)} out parameters, so the success value is a",
              " * struct.  FIELD ORDER IS THE DECLARATION ORDER OF THE HOST'S OWN OUT",
              " * PARAMETERS, which is also the order the Lua binding pushes them; the",
              " * differential compares the two orders slot by slot.",
              " */",
              f"struct {_result_struct(e.name)} {{"]
        for o in e.outs:
            L.append(f"    {_CPP_OUT[o.c_type]} {o.name} = "
                     f"{'false' if o.c_type == 'bool' else '0'};")
        L += ["};", ""]
    return L


def _signature(e: Entry, ret: str, extra_params: list[str] | None = None,
               noexcept: bool | None = None) -> str:
    extra = extra_params or []
    # A default cannot precede a non-defaulted parameter in C++, and `extra`
    # (the out span) is always non-defaulted, so the defaults have to go when
    # one is appended.
    decl = e.params_decl(allow_defaults=not extra) + extra
    head = f"    {'[[nodiscard]] ' if ret != 'void' else ''}{ret} {e.name}("
    ne = e.is_noexcept() if noexcept is None else noexcept
    tail = f") const{' noexcept' if ne else ''}"
    one = head + ", ".join(decl) + tail
    if len(one) <= 78:
        return one
    pad = " " * len(head)
    lines = []
    cur = head + decl[0] if decl else head
    for p in decl[1:]:
        if len(cur) + 2 + len(p) > 78:
            lines.append(cur + ",")
            cur = pad + p
        else:
            cur += ", " + p
    lines.append(cur + tail)
    return "\n".join(lines)


def _emit_method(e: Entry) -> list[str]:
    L = _method_comment(e)
    s = e.shape
    ret = e.return_type()

    if s == "void_call":
        L.append(_signature(e, ret))
        L.append("    {")
        L.append(f"        {e.c_fn}({', '.join(e.c_call_args({}))});")
        L.append("    }")
        return L

    if s == "value_return":
        L.append(_signature(e, ret))
        L.append("    {")
        call = f"{e.c_fn}({', '.join(e.c_call_args({}))})"
        if e.m.ret == "const char *":
            absent = e.ent.get("absent_value")
            if isinstance(absent, dict):
                fb = f"{absent['param']}.c_str()"
                L += [f"        const char *s = {call};",
                      f"        if (s == nullptr) s = {fb};",
                      "        return s != nullptr ? std::string(s) : std::string();"]
            else:
                L += [f"        const char *s = {call};",
                      "        return s != nullptr ? std::string(s) : std::string();"]
        else:
            L.append(f"        return {call};")
        L.append("    }")
        return L

    if s == "void_out_array":
        o = e.outs[0]
        L.append(_signature(e, ret))
        L += ["    {",
              f"        {ret} out{{}};",
              f"        {e.c_fn}({', '.join(e.c_call_args({o.name: 'out.data()'}))});",
              "        return out;",
              "    }"]
        return L

    if s == "fallible_out":
        L.append(_signature(e, ret))
        L.append("    {")
        if e.ent.get("index_base"):
            first = e.ins[0].name
            L += [f"        if ({first} < 0)",
                  "            return std::nullopt;"]
        st = e.success_type()
        exprs: dict[str, str] = {}
        if len(e.outs) == 1:
            o = e.outs[0]
            L.append(f"        {st} v{{}};")
            exprs[o.name] = "v.data()" if o.arity > 0 else "&v"
        else:
            L.append(f"        {st} v;")
            for o in e.outs:
                exprs[o.name] = f"&v.{o.name}"
        L += [f"        if (!{e.c_fn}({', '.join(e.c_call_args(exprs))}))",
              "            return std::nullopt;",
              "        return v;",
              "    }"]
        return L

    if s == "first_and_count":
        cap = e.ent["out_capacity"]
        L.append(_signature(e, ret))
        o = e.outs[0]
        L += ["    {",
              f"        Entity found[{cap}] = {{}};",
              f"        const int n = {e.c_fn}("
              f"{', '.join(e.c_call_args({o.name: 'found', '__capacity__': str(cap)}))});",
              "        FirstAndCount r;",
              "        r.count = n;",
              "        if (n > 0)",
              "            r.first = found[0];",
              "        return r;",
              "    }"]
        return L

    if s == "entity_table":
        cap = e.ent["out_capacity"]
        o = e.outs[0]
        L.append(_signature(e, ret))
        L += ["    {",
              f"        std::vector<Entity> out(static_cast<std::size_t>({cap}));",
              f"        int n = {e.c_fn}("
              f"{', '.join(e.c_call_args({o.name: 'out.data()', '__capacity__': str(cap)}))});",
              "        /* Clamped on BOTH ends, and the upper clamp is not",
              "         * symmetry: the Lua binding does not have it, so a host",
              "         * that returns more than it was given overruns a stack",
              "         * array there and merely loses entries here.  Stated",
              "         * rather than silently differing.",
              "         */",
              "        if (n < 0)",
              "            n = 0;",
              f"        if (n > {cap})",
              f"            n = {cap};",
              "        out.resize(static_cast<std::size_t>(n));",
              "        return out;",
              "    }"]
        L += ["",
              "#if JCE_SCRIPT_CPP_HAS_SPAN",
              "    /* C++20 only.  The allocation-free form: the CALLER owns the",
              "     * buffer and chooses its size, which is the C ABI's own shape.",
              "     * The return value is the host's count and may exceed the span",
              "     * -- ask again with a bigger one.",
              "     */"]
        L.append(_signature(e, "int", ["std::span<Entity> out"], noexcept=True))
        L += ["    {",
              f"        return {e.c_fn}("
              f"{', '.join(e.c_call_args({o.name: 'out.data()', '__capacity__': 'static_cast<int>(out.size())'}))});",
              "    }",
              "#endif"]
        return L

    if s == "owned_string_release":
        L.append(_signature(e, ret))
        args_small = e.c_call_args({}, ["inl", "static_cast<int>(sizeof inl)"])
        args_big = e.c_call_args({}, ["big.data()", "n + 1"])
        L += ["    {",
              "        char inl[kOwnedStringInlineBytes];",
              f"        const int n = {e.c_fn}({', '.join(args_small)});",
              "        if (n < 0)",
              "            return std::nullopt;",
              "        if (n < kOwnedStringInlineBytes)",
              "            return std::string(inl, static_cast<std::size_t>(n));",
              "        /* The inline buffer was too small.  The C ABI copied what",
              "         * fitted and RELEASED the host's string before returning,",
              "         * so the tail is unreachable and the only way to get it is",
              "         * to ask again.  That second ask is a second HOST CALL and",
              "         * is visible in the differential's trace.  What that",
              "         * second ask costs is measured by the C++ suite, which is",
              "         * not part of this repository -- so the property is named",
              "         * here rather than cited: an unresolvable name reads the",
              "         * same whether the test is missing or renamed.",
              "         */",
              "        std::string big(static_cast<std::size_t>(n), '\\0');",
              f"        int m = {e.c_fn}({', '.join(args_big)});",
              "        if (m < 0)",
              "            return std::nullopt;",
              "        if (m > n)",
              "            m = n;   /* the host answered longer the second time */",
              "        big.resize(static_cast<std::size_t>(m));",
              "        return big;",
              "    }"]
        return L

    raise SystemExit(f"error: {e.name}: emit_cpp has no body for shape {s!r}")


def emit_hpp(members: list[HostMember], man: dict) -> str:
    entries = _entries(members, man)
    L = [_HPP_HEAD.rstrip("\n")]
    L += _emit_result_structs(entries)
    L += _emit_defaults(entries)

    L.append("/* ------------------------------------------------------------------ *")
    L.append(" *  NOT wrapped: the manifest's hand-written entries.")
    L.append(" *")
    L.append(" *  Excluded as a CLASS, by the same rule the C ABI excludes them by,")
    L.append(" *  and for the same reason: three are Lua-VM machinery with no meaning")
    L.append(" *  outside a lua_State, and the two asset readers carry sandbox policy")
    L.append(" *  that lives in `static` functions inside jce_script.c.  A C++")
    L.append(" *  convenience wrapper is the most tempting place in this tree to")
    L.append(" *  \"finish the job\" by calling the raw read_file member instead --")
    L.append(" *  that is the escape the manifest itself calls P0-2.  The manifest's")
    L.append(" *  own reasons, verbatim:")
    L.append(" *")
    for e in man["hand_written"]:
        L.append(f" *    {e['name']}")
        L += _wrap(e["reason"], " *        ")
    L.append(" * ------------------------------------------------------------------ */")
    L.append("")
    L.append(_HPP_API_HEAD.replace("@INLINE@",
                                   str(INLINE_OWNED_STRING_BYTES)).rstrip("\n"))
    L.append("")
    for e in entries:
        L += _emit_method(e)
        L.append("")
    L.append(_HPP_TAIL.rstrip("\n"))
    return "\n".join(L) + "\n"


# ── Artefact 2: the cross-language differential ──────────────────────────────
#
# Argument values.  DISTINCT PER SLOT on purpose: a wrapper that transposes two
# arguments of the same C type is invisible to the result comparison (the host
# answers the same either way) and visible in the host-call trace only if the
# two values differ.  The distinctness is the whole reason the trace is worth
# recording.  Every float is exactly representable (k + 0.5), so `%.9g` prints
# the same 9 significant digits from the Lua literal and from the C++ literal
# and the comparison is about behaviour rather than about printf.

def _arg_values(e: Entry) -> list[tuple[Param, list[tuple[str, str]]]]:
    """[(param, [(lua_literal, cpp_literal), ...])] -- one pair per STACK SLOT.

    An arity-N const array is N Lua slots and one C++ std::array, so the list
    per parameter is N long for those and 1 long for everything else."""
    out: list[tuple[Param, list[tuple[str, str]]]] = []
    slot = 0
    for p in e.ins:
        vals: list[tuple[str, str]] = []
        if p.arity > 0:
            for _ in range(p.arity):
                v = slot + 0.5
                vals.append((f"{v:.9g}", f"{v:.9g}f"))
                slot += 1
        elif p.c_type in ("float", "double"):
            v = slot + 0.5
            vals.append((f"{v:.9g}", f"{v:.9g}f" if p.c_type == "float"
                         else f"{v:.9g}"))
            slot += 1
        elif p.c_type == "bool":
            vals.append(("true", "true"))
            slot += 1
        elif p.c_type == "const char *":
            s = f"{e.name}_{p.name}"
            vals.append((f"'{s}'", f'"{s}"'))
            slot += 1
        elif p.c_type == "JceScriptEntity":
            v = 41 + slot * 10
            vals.append((str(v), f"static_cast<Entity>({v})"))
            slot += 1
        elif p.c_type == "uint32_t":
            v = 7 + slot
            vals.append((str(v), f"static_cast<std::uint32_t>({v})"))
            slot += 1
        else:                                     # int
            v = 3 + slot
            vals.append((str(v), str(v)))
            slot += 1
        out.append((p, vals))
    return out


def _lua_call(e: Entry, drop_trailing_defaults: bool = False,
              index_override: str | None = None) -> str:
    """The Lua expression for one case.

    IDENTICAL VALUES TO THE C++ CALL, with exactly one manifest-derived
    adjustment: an `index_base` entry's index parameter is BASE-SHIFTED,
    because the two languages count from different places on purpose.  Lua's
    binding takes a 1-based index and subtracts the base before calling the
    host; the C ABI and this wrapper are 0-based.  Feeding both sides the same
    integer would compare `touch_get|2` against `touch_get|3` -- which is
    exactly what the first run of this differential printed, and it is a
    property of the two contracts rather than a defect in either."""
    base = e.ent.get("index_base") or 0
    args: list[str] = []
    for p, vals in _arg_values(e):
        if drop_trailing_defaults and p.name in e.defaulted:
            continue
        if p is e.ins[0] and base:
            if index_override is not None:
                args.append(index_override)
            else:
                args.append(str(int(vals[0][0]) + base))
            continue
        args += [lua for lua, _ in vals]
    return f"jce.{e.name}({', '.join(args)})"


def _cpp_call(e: Entry, drop_trailing_defaults: bool = False,
              index_override: str | None = None) -> str:
    args: list[str] = []
    for p, vals in _arg_values(e):
        if drop_trailing_defaults and p.name in e.defaulted:
            continue
        if index_override is not None and p is e.ins[0] and e.ent.get("index_base"):
            args.append(index_override)
            continue
        if p.arity > 0:
            inner = ", ".join(cpp for _, cpp in vals)
            args.append("{" + inner + "}")
        else:
            args.append(vals[0][1])
    return f"api.{e.name}({', '.join(args)})"


def _record_expr(e: Entry, call: str) -> str:
    """How the C++ RESULT reaches the comparison form.

    Every one of these lands in a HAND-WRITTEN overload keyed on the C++ TYPE
    (jce_script_cpp_differential.hpp).  Nothing per-entry is emitted, so a
    per-entry defect in the wrapper cannot be mirrored by a per-entry defect
    here -- which is the difference between a differential and a tautology."""
    if e.shape == "void_call":
        return f"        {call};"
    if e.shape in ("fallible_out",):
        miss = e.ent.get("miss_value")
        miss_s = "nil:nil" if miss is None else f"number:{_fmt_g(miss)}"
        return (f"        jce::diff::record_optional(s, {call}, \"{miss_s}\");")
    if e.shape == "owned_string_release":
        return f"        jce::diff::record_optional(s, {call}, \"nil:nil\");"
    return f"        jce::diff::record(s, {call});"


_DIFF_HEAD = '''\
/* test_jce_script_cpp_differential.gen.cpp -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * THE CROSS-LANGUAGE DIFFERENTIAL: Lua is the reference implementation.
 *
 * Batch 1 proved the generated Lua bindings equivalent to the hand-written
 * originals over 87 cases, and said in its own commit that the oracle it spent
 * "disappears the moment they are deleted and never comes back".  What did not
 * disappear is Lua ITSELF: a shipped, tested, independently generated
 * implementation of the same manifest.  So every case below is driven through
 * a real Lua VM and through the C++ wrapper over ONE recording mock host, and
 * two things are compared:
 *
 *   * the FULL RESULT, type and value per slot.  `nil` vs `0` vs `false` are
 *     three different strings, so none of them can pass for another.
 *   * the HOST-CALL TRACE.  A wrapper that produces the right value by calling
 *     the wrong member, or the right member with transposed arguments, is
 *     invisible to a result comparison and visible here.  Argument values are
 *     DISTINCT PER SLOT so a transposition has something to show.
 *
 * NOT CIRCULAR, in the one way that matters: the C++ result is projected onto
 * the comparison form by hand-written, TYPE-KEYED overloads in
 * jce_script_cpp_differential.hpp.  There is no per-entry projection, so a
 * per-entry defect in the wrapper has nothing to be mirrored by.
 *
 * WHAT IT CANNOT SEE, stated rather than left to be discovered:
 *   * an entry the manifest omits entirely -- neither side has it.  The gate's
 *     totality condition covers that.
 *   * a wrong value both implementations agree on because the MOCK is wrong.
 *   * the seven hand-written entries.  Neither the C ABI nor this wrapper
 *     exposes them, by rule; only Lua has them.
 *   * a transposition inside _arg_values in emit_cpp.py, which permutes both
 *     sides identically.  That is the M1 shape from batch 1 and it is why the
 *     values are per-slot distinct: the wrapper is what this compares, and a
 *     transposition THERE is red on the trace.
 *
 * The C ABI in the middle is being exercised too, and by more than its own
 * suite: tests/scripting/c_abi covers ten entry points by hand, and this file
 * drives all @NENTRY@ of them.
 */
'''


def _mock_value(idx: int) -> float:
    return 9.5 + idx


def _emit_mock(member: HostMember, ent: dict, idx: int) -> list[str]:
    """One recording mock host callback, from the vtable member itself.

    EVERY OUT SLOT GETS A DISTINCT VALUE, tracked by a running counter rather
    than by the parameter's own index.  get_touch fills four scalars; had they
    all carried one value, a wrapper that transposed `x` and `y` would produce
    an identical result stack on both sides and the differential would pass."""
    name = member.name
    decl = []
    for p in member.params[1:]:
        if p.arity > 0:
            decl.append(f"{p.c_type} {p.name}[{p.arity}]")
        elif p.arity < 0:
            decl.append(f"{p.c_type} *{p.name}")
        else:
            decl.append(f"{p.c_type} {p.name}")
    head = f"static {member.ret} mk_{name}(void *user"
    L = [head + "".join(", " + d for d in decl) + ")", "{", "    (void)user;",
         f'    jce::diff::trace_add("{name}");']
    for p in in_params(member):
        if p.arity > 0:
            for i in range(p.arity):
                L.append(f"    jce::diff::trace_num({p.name}[{i}]);")
        elif p.c_type == "const char *":
            L.append(f"    jce::diff::trace_str({p.name});")
        elif p.c_type == "bool":
            L.append(f"    jce::diff::trace_num({p.name} ? 1 : 0);")
        else:
            L.append(f"    jce::diff::trace_num(static_cast<double>({p.name}));")
    L.append("    jce::diff::trace_end();")

    shape = ent["shape"]
    outs = out_params(member)
    ok = "jce::diff::g_mode != jce::diff::MOCK_FAIL"

    # One counter for every scalar this member writes, so no two agree.
    slot = [0]

    def nextf() -> str:
        v = _mock_value(idx * 16 + slot[0])
        slot[0] += 1
        return f"{v:.9g}f"

    def nexti(base: int) -> str:
        v = base + idx * 16 + slot[0]
        slot[0] += 1
        return str(v)

    def fill(o: Param) -> list[str]:
        if o.arity > 0:
            return [f"    {o.name}[{i}] = {nextf()};" for i in range(o.arity)]
        if o.c_type in _POD_OUT:
            out = [f"    {o.name}->entity = "
                   f"static_cast<JceScriptEntity>({nexti(200)});"]
            out += [f"    {o.name}->point[{i}] = {nextf()};" for i in range(3)]
            out += [f"    {o.name}->normal[{i}] = {nextf()};" for i in range(3)]
            out += [f"    {o.name}->distance = {nextf()};"]
            return out
        if o.c_type == "bool":
            return [f"    *{o.name} = true;"]
        if o.c_type in ("uint64_t", "uint32_t", "int", "JceScriptEntity"):
            return [f"    *{o.name} = static_cast<{o.c_type}>({nexti(300)});"]
        return [f"    *{o.name} = {nextf()};"]

    if shape in ("first_and_count", "entity_table"):
        o = outs[0]
        n_ok = 2 if shape == "first_and_count" else 3
        L += ["    if (jce::diff::g_mode == jce::diff::MOCK_FAIL)",
              "        return 0;",
              "    if (jce::diff::g_mode == jce::diff::MOCK_NEGATIVE)",
              "        return -3;",
              f"    for (int i = 0; i < {n_ok}; ++i)",
              f"        {o.name}[i] = static_cast<{o.c_type}>({100 + idx} + i);",
              f"    return {n_ok};"]
        L.append("}")
        return L

    if member.ret == "void":
        for o in outs:
            L += fill(o)
        L.append("}")
        return L

    if member.ret == "bool":
        L += [f"    if (!({ok}))",
              "        return false;"]
        for o in outs:
            L += fill(o)
        L += ["    return true;", "}"]
        return L

    if member.ret == "int":
        L += ["    if (jce::diff::g_mode == jce::diff::MOCK_FAIL)",
              "        return 0;",
              "    if (jce::diff::g_mode == jce::diff::MOCK_NEGATIVE)",
              "        return -3;",
              f"    return {4 + idx % 5};", "}"]
        return L

    if member.ret in ("float", "double"):
        suffix = "f" if member.ret == "float" else ""
        L += [f"    return {_mock_value(idx):.9g}{suffix};", "}"]
        return L

    if member.ret == "JceScriptEntity":
        L += [f"    if (!({ok}))", "        return 0;",
              f"    return static_cast<JceScriptEntity>({400 + idx});", "}"]
        return L

    if member.ret == "const char *":
        L += [f"    if (!({ok}))",
              "        return NULL;   /* a host that answers NULL, not an absent host */",
              f'    return "{member.name}_value";', "}"]
        return L

    if member.ret in ("char *", "char*"):
        L += [f"    if (!({ok}))", "        return NULL;",
              "    if (jce::diff::g_mode == jce::diff::MOCK_LONG)",
              "        return jce::diff::dup_long_json();",
              f'    return jce::diff::dup("{{\\"{member.name}\\":{idx}}}");', "}"]
        return L

    raise SystemExit(f"error: emit_cpp: no mock for return type {member.ret!r} "
                     f"on host member {member.name}")


def emit_differential(members: list[HostMember], man: dict) -> str:
    entries = _entries(members, man)
    by = {m.name: m for m in members}

    L = [_DIFF_HEAD.replace("@NENTRY@", str(len(entries))).rstrip("\n"), ""]
    L += ['#include "jce_script_cpp_differential.hpp"', "",
          '#include "doctest.h"', "",
          "#include <jce/middleware/script/jce_script.h>", "",
          "#include <cstdio>", "#include <cstring>", "",
          "using jce::script::Api;", "using jce::script::Entity;", ""]

    # ---- the recording mock host ---------------------------------------
    L += ["/* ------------------------------------------------------------------ *",
          " *  The recording mock host.  ONE host, driven by both languages.",
          " *",
          " *  extern \"C\" because JceScriptHost's members are C function pointers;",
          " *  a C++-linkage function assigned to one is a type mismatch every",
          " *  toolchain happens to tolerate and none is required to.",
          " *",
          " *  `log` is NOT traced: it is how the Lua side reports its result, so",
          " *  tracing it would put the answer inside the evidence.",
          " * ------------------------------------------------------------------ */",
          'extern "C" {', ""]
    L += ["static void mk_log(void *user, const char *msg)",
          "{",
          "    (void)user;",
          "    jce::diff::capture().log += (msg != NULL ? msg : \"(null)\");",
          "    jce::diff::capture().log += '\\n';",
          "}", ""]

    emitted: dict[str, HostMember] = {}
    order: list[tuple[HostMember, dict]] = []
    for e in entries:
        if e.m.name not in emitted:
            emitted[e.m.name] = e.m
            order.append((e.m, e.ent))
    for e in entries:
        rel = e.ent.get("release")
        if rel and rel not in emitted:
            emitted[rel] = by[rel]
            order.append((by[rel], {"shape": "__release__"}))

    for idx, (m, ent) in enumerate(order):
        if ent["shape"] == "__release__":
            L += [f"/* the release member of the owned-string shape.  Traced, because",
                  " * 'the string was released' is a fact about the CALL SEQUENCE and",
                  " * nothing in the result can show it. */",
                  f"static void mk_{m.name}(void *user, char *s)",
                  "{",
                  "    (void)user;",
                  f'    jce::diff::trace_add("{m.name}");',
                  "    jce::diff::trace_str(s);",
                  "    jce::diff::trace_end();",
                  "    jce::diff::release(s);",
                  "}", ""]
            continue
        L += _emit_mock(m, ent, idx)
        L.append("")

    L += ['}  /* extern "C" */', ""]

    L += ["/* Every member any manifest `expose` entry reaches, and nothing else.",
          " * The members only the seven hand-written entries reach stay NULL: this",
          " * wrapper does not expose them, so there is nothing to compare. */",
          "void jce::diff::install_recording_host(JceScriptHost *h)",
          "{",
          "    std::memset(h, 0, sizeof *h);",
          "    h->user = &jce::diff::capture();",
          "    h->log = mk_log;"]
    for m, _ in order:
        L.append(f"    h->{m.name} = mk_{m.name};")
    L += ["}", ""]

    L += ["/* `log` and nothing else: the state a host built against an older",
          " * header leaves the tail of the table in, and the state every member",
          " * of a host that simply does not implement a subsystem is in.  `log`",
          " * survives because it is how the Lua side reports its answer; it is",
          " * not part of the surface under comparison. */",
          "void jce::diff::install_log_only_host(JceScriptHost *h)",
          "{",
          "    std::memset(h, 0, sizeof *h);",
          "    h->user = &jce::diff::capture();",
          "    h->log = mk_log;",
          "}", ""]

    # ---- the C++ case bodies -------------------------------------------
    cases: list[tuple[str, str, str, str]] = []   # label, mode, lua, fn
    fn_bodies: list[str] = []

    def add_case(label: str, mode: str, lua: str, body: str) -> None:
        fn = f"cpp_case_{len(cases)}"
        fn_bodies.append(f"static void {fn}(Api &api, jce::diff::Slots &s)\n"
                         f"{{\n    (void)api; (void)s;\n{body}\n}}\n")
        cases.append((label, mode, lua, fn))

    for e in entries:
        add_case(e.name, "MOCK_OK", _lua_call(e),
                 _record_expr(e, _cpp_call(e)))
        add_case(e.name + " [host says no]", "MOCK_FAIL", _lua_call(e),
                 _record_expr(e, _cpp_call(e)))
        if e.defaulted:
            add_case(e.name + " [trailing defaults omitted]", "MOCK_OK",
                     _lua_call(e, drop_trailing_defaults=True),
                     _record_expr(e, _cpp_call(e, drop_trailing_defaults=True)))
        # A negative count. Reaches `clamp_min`, and reaches the one place the
        # two languages could plausibly have disagreed: entity_table.
        #
        # I predicted a divergence here and I was WRONG, and the wrong
        # prediction is worth keeping because it is the reason the case exists.
        # The generated Lua binding passes the host's raw count to
        # lua_createtable(), so -3 looked like a request for ~4 billion slots
        # and a LUA_ERRMEM.  MEASURED: lua_createtable guards its hint with
        # `narray > 0`, so a negative count makes an EMPTY table and the loop
        # that follows does not run -- the same empty answer the C++ wrapper's
        # clamp produces.  They agree, so this belongs in the sweep and not in
        # a divergence note.  The hand-written expectation -- a negative
        # entity_table count is the empty list on both sides -- lives in the
        # C++ suite, which this repository does not track, so it is stated here
        # rather than cited by name.  Two implementations agreeing on a wrong
        # value is still a wrong value, and the sweep structurally cannot say
        # which.
        if "clamp_min" in e.ent or e.shape in ("first_and_count", "entity_table"):
            add_case(e.name + " [host returns a negative count]", "MOCK_NEGATIVE",
                     _lua_call(e), _record_expr(e, _cpp_call(e)))
        if e.ent.get("index_base"):
            base = e.ent["index_base"]
            add_case(e.name + " [below the index base]", "MOCK_OK",
                     _lua_call(e, index_override=str(base - 1)),
                     _record_expr(e, _cpp_call(e, index_override="-1")))

    L += ["/* ------------------------------------------------------------------ *",
          " *  One function per case: the C++ call, and the projection of its",
          " *  result onto the comparison form.  The projection is a call into a",
          " *  hand-written, type-keyed overload -- never a rendering emitted",
          " *  here.",
          " * ------------------------------------------------------------------ */"]
    L += fn_bodies

    L += ["struct DiffCase {",
          "    const char       *label;",
          "    jce::diff::MockMode mode;",
          "    const char       *lua;",
          "    void            (*run_cpp)(Api &, jce::diff::Slots &);",
          "};", ""]
    L.append("static const DiffCase g_cases[] = {")
    for label, mode, lua, fn in cases:
        esc = lua.replace("\\", "\\\\").replace('"', '\\"')
        L.append(f'    {{ "{label}", jce::diff::{mode}, "{esc}", {fn} }},')
    L += ["};", ""]
    L += ["#define JCE_DIFF_CASES ((int)(sizeof g_cases / sizeof g_cases[0]))",
          "",
          "/* THE COVERAGE COUNT IS ANCHORED TO A DIFFERENT GENERATOR'S ARTEFACT,",
          " * and the first version of this file got that wrong.",
          " *",
          " * It compared JCE_DIFF_CASES against a literal emit_cpp.py had",
          " * printed from the same list it built g_cases from.  Those two",
          " * cannot disagree: it was an assertion that could not fail, which is",
          " * the twelfth of its kind this campaign has caught and the first I",
          " * wrote.  JCE_SCRIPT_API_ENTRY_COUNT comes from",
          " * scripting/c_abi/include/jce/script_api/jce_script_api.h, emitted by",
          " * tools/scriptgen/gen_script_c_abi.py -- a different tool reading the",
          " * same manifest.  If THIS backend silently stops emitting a case per",
          " * entry, that number does not move with it.",
          " */",
          "static int cases_with_label(const char *suffix)",
          "{",
          "    int n = 0;",
          "    for (int i = 0; i < JCE_DIFF_CASES; ++i) {",
          "        const char *bracket = std::strchr(g_cases[i].label, '[');",
          "        if (suffix == NULL) {",
          "            if (bracket == NULL)",
          "                ++n;",
          "        } else if (bracket != NULL && std::strcmp(bracket, suffix) == 0) {",
          "            ++n;",
          "        }",
          "    }",
          "    return n;",
          "}",
          ""]

    L += ['''\
/* ------------------------------------------------------------------ *
 *  The comparison.
 * ------------------------------------------------------------------ */

static std::string run_cpp_case(const DiffCase &c, const JceScriptHost *host,
                                std::size_t host_size)
{
    jce::diff::Slots s;
    jce::diff::trace_reset();
    if (host != NULL) {
        Api api = Api::open(*host, host_size);
        c.run_cpp(api, s);
    } else {
        Api api;                      /* never opened: the empty handle */
        c.run_cpp(api, s);
    }
    return s.str();
}

TEST_CASE("every C ABI entry point has a case, in both directions")
{
    /* One BARE-labelled case per entry (the full-argument, host-succeeds
     * case), and one "[host says no]" case per entry.  Both counted against
     * the C ABI header's own macro, which this backend does not produce. */
    CHECK(cases_with_label(NULL) == JCE_SCRIPT_API_ENTRY_COUNT);
    CHECK(cases_with_label("[host says no]") == JCE_SCRIPT_API_ENTRY_COUNT);
    CHECK(JCE_DIFF_CASES > 2 * JCE_SCRIPT_API_ENTRY_COUNT);
}

TEST_CASE("Lua and C++ agree on every case, in result and in host calls")
{
    for (int i = 0; i < JCE_DIFF_CASES; ++i) {
        const DiffCase &c = g_cases[i];
        INFO("case: " << std::string(c.label));

        JceScriptHost ha;
        jce::diff::install_recording_host(&ha);
        jce::diff::g_mode = c.mode;
        std::string lua_out = jce::diff::run_lua_case(&ha, sizeof ha, c.lua);
        std::string lua_trace = jce::diff::trace_text();

        JceScriptHost hb;
        jce::diff::install_recording_host(&hb);
        jce::diff::g_mode = c.mode;
        std::string cpp_out = run_cpp_case(c, &hb, sizeof hb);
        std::string cpp_trace = jce::diff::trace_text();

        /* The Lua chunk must RUN.  Two sides erroring identically would
         * satisfy every comparison below; that is the shape of a test that
         * cannot fail, and batch 1's harness shipped exactly it once. */
        INFO("lua said: " << lua_out);
        REQUIRE(jce::diff::last_run_ok());
        CHECK(lua_out == cpp_out);
        CHECK(lua_trace == cpp_trace);
    }
    jce::diff::g_mode = jce::diff::MOCK_OK;
}

TEST_CASE("Lua and C++ agree when every host member is absent")
{
    for (int i = 0; i < JCE_DIFF_CASES; ++i) {
        const DiffCase &c = g_cases[i];
        INFO("absent-host case: " << std::string(c.label));

        JceScriptHost ha;
        jce::diff::install_log_only_host(&ha);
        jce::diff::g_mode = c.mode;
        std::string lua_out = jce::diff::run_lua_case(&ha, sizeof ha, c.lua);
        std::string lua_trace = jce::diff::trace_text();

        JceScriptHost hb;
        jce::diff::install_log_only_host(&hb);
        jce::diff::g_mode = c.mode;
        std::string cpp_out = run_cpp_case(c, &hb, sizeof hb);
        std::string cpp_trace = jce::diff::trace_text();

        INFO("lua said: " << lua_out);
        REQUIRE(jce::diff::last_run_ok());
        CHECK(lua_out == cpp_out);
        CHECK(lua_trace == cpp_trace);
        CHECK(lua_trace.empty());   /* there was nothing to call */
    }
    jce::diff::g_mode = jce::diff::MOCK_OK;
}

/* Not a differential: a C++-side invariant the header's banner promises.
 * A never-opened handle and a host whose members are all NULL are different
 * situations, and every entry point must answer them identically. */
TEST_CASE("an empty handle answers like an absent host")
{
    for (int i = 0; i < JCE_DIFF_CASES; ++i) {
        const DiffCase &c = g_cases[i];
        INFO("empty-handle case: " << std::string(c.label));

        JceScriptHost h;
        jce::diff::install_log_only_host(&h);
        jce::diff::g_mode = c.mode;
        std::string absent = run_cpp_case(c, &h, sizeof h);
        std::string empty = run_cpp_case(c, NULL, 0);
        CHECK(absent == empty);
    }
    jce::diff::g_mode = jce::diff::MOCK_OK;
}
''']
    return "\n".join(L).rstrip("\n") + "\n"


# Every citation of a doctest case, in this module's own source, in the two
# artefacts it emits, and in scripting/cpp/README.md, is spelled
#
#     the marker word, then the TEST_CASE name in double quotes, verbatim
#
# (spelled out rather than shown, because CPP4 scans THIS FILE too and an
#  example would be a citation of a test named after a placeholder --
#  which is exactly what the first draft of this comment was, and the
#  condition caught it on its first run.)
#
# so that CPP4 can find it without a hand-kept list.  See CPP4 for why.
_CITE_RE = re.compile(r'\[doctest\] "([^"\n]+)"')

_CPP_TEST_DIR = REPO_ROOT / "tests/scripting/cpp"


def _check_citations(members: list[HostMember], man: dict) -> list[str]:
    """CPP4: no citation may name a doctest case that does not exist.

    The differential is compared against its FRESHLY RENDERED text rather than
    against the committed file, because a citation and the TEST_CASE it names
    can land in the same emit -- checking the stale copy on disk would fail a
    correct change.  The hand-written suites are read from disk, which is where
    they live."""
    haystack = emit_differential(members, man)
    for f in sorted(_CPP_TEST_DIR.glob("*.cpp")):
        if f.name.endswith(".gen.cpp"):
            continue                      # already present, freshly rendered
        haystack += f.read_text(encoding="utf-8")

    sources = {
        "tools/scriptgen/emit_cpp.py": Path(__file__).read_text(encoding="utf-8"),
        "scripting/cpp/include/jce/script_api/jce_script_api.hpp":
            emit_hpp(members, man),
        "tests/scripting/cpp/test_jce_script_cpp_differential.gen.cpp": haystack,
    }
    readme = REPO_ROOT / "scripting/cpp/README.md"
    if readme.is_file():
        sources["scripting/cpp/README.md"] = readme.read_text(encoding="utf-8")

    problems: list[str] = []
    cited = 0
    for where, text in sources.items():
        for name in _CITE_RE.findall(text):
            cited += 1
            if f'TEST_CASE("{name}")' not in haystack:
                problems.append(
                    f"cpp: {where} cites [doctest] \"{name}\", and no "
                    f"TEST_CASE of that name exists under "
                    f"tests/scripting/cpp/ -- a citation that resolves to "
                    f"nothing is worse than none, because grep answers "
                    f"'absent' for both a missing test and a wrong name")
    if cited == 0:
        problems.append(
            "cpp: CPP4 found no [doctest] citations at all. Either every one "
            "was deleted, or the marker was changed in one place and not the "
            "other -- and a condition that scans nothing passes for free")
    return problems


# ── The backend ──────────────────────────────────────────────────────────────

class CppBackend(ScriptBackend):
    name = "cpp"

    def artefacts(self, members: list[HostMember], man: dict) -> list[Artefact]:
        return [Artefact(HPP, emit_hpp),
                Artefact(DIFF_CPP, emit_differential)]

    def validate(self, members: list[HostMember], man: dict,
                 c_text: str) -> list[str]:
        """This backend's OWN conditions.

        The first is the load-bearing one and it is the reason there is no
        separate C backend: the C binding of the scripting surface is
        scripting/c_abi's generated header, and this wrapper is built ON it.
        "C is already done" is therefore checked here rather than asserted in
        prose -- if the C ABI ever stops declaring an entry, this fails by
        name instead of the C++ wrapper failing to compile later, less
        clearly."""
        problems: list[str] = []
        by_name = {m.name: m for m in members}

        # -- CPP1: the C binding really does cover the surface this wraps.
        if not C_ABI_HEADER.is_file():
            problems.append(
                f"cpp: {C_ABI_HEADER.relative_to(REPO_ROOT).as_posix()} is "
                f"missing -- the C++ wrapper wraps the C ABI, and there is no "
                f"C ABI. Regenerate with: python "
                f"tools/scriptgen/gen_script_c_abi.py --write")
        else:
            text = C_ABI_HEADER.read_text(encoding="utf-8")
            for e in man["expose"]:
                if f"{C_PREFIX}{e['name']}(" not in text:
                    problems.append(
                        f"cpp: expose[{e['name']}] has no {C_PREFIX}{e['name']} "
                        f"declaration in the C ABI header -- the C++ wrapper "
                        f"would emit a call to a function that does not exist")
            for meta in ("open", "close", "version"):
                if f"{C_PREFIX}{meta}(" not in text:
                    problems.append(
                        f"cpp: the C ABI header declares no {C_PREFIX}{meta} -- "
                        f"jce::script::Api's RAII depends on it")
            # The hand-written seven must stay out of BOTH surfaces.  A C ABI
            # that grew one of them is a policy change this wrapper would
            # inherit silently.
            for e in man["hand_written"]:
                if f"{C_PREFIX}{e['name']}(" in text:
                    problems.append(
                        f"cpp: the C ABI header declares {C_PREFIX}{e['name']}, "
                        f"a hand_written entry excluded as a class -- the C++ "
                        f"wrapper is built on the assumption that it is not "
                        f"there (manifest reason: {e.get('reason', '?')})")

        # -- CPP2: every entry becomes a legal, non-colliding C++ method.
        seen: dict[str, str] = {}
        for e in man["expose"]:
            n = e["name"]
            if not _IDENT_RE.match(n):
                problems.append(f"cpp: expose[{n}] is not a legal C++ identifier")
            if n in _RESERVED_METHOD_NAMES:
                problems.append(
                    f"cpp: expose[{n}] collides with a C++ keyword or with one "
                    f"of jce::script::Api's own members -- rename the binding "
                    f"or this backend emits a method that shadows the handle")
            if n in seen:
                problems.append(f"cpp: expose[{n}] is declared twice")
            seen[n] = n

        # -- CPP3: every C type this backend must render has a rendering.  The
        #    emitters raise on a miss, but a raise inside --write is a stack
        #    trace; this is the same fact as a named gate failure.
        for e in man["expose"]:
            m = by_name.get(e["vtable"])
            if m is None:
                continue                      # condition 3 already reported it
            if e["shape"] not in ("void_call", "value_return", "void_out_array",
                                  "fallible_out", "first_and_count",
                                  "entity_table", "owned_string_release"):
                problems.append(f"cpp: expose[{e['name']}]: unhandled shape "
                                f"{e['shape']!r}")
                continue
            if e["shape"] == "value_return" and m.ret not in _CPP_OUT:
                problems.append(
                    f"cpp: expose[{e['name']}] returns {m.ret!r}, which this "
                    f"backend has no C++ rendering for -- add one here or mark "
                    f"the binding hand_written")
            for p in script_in_params(m, e):
                if p.name in (e.get("bind_args") or {}):
                    continue
                base = p.c_type[len("const "):] if p.c_type.startswith("const ") \
                    else p.c_type
                if (p.c_type if p.arity <= 0 else base) not in _CPP_IN:
                    problems.append(
                        f"cpp: expose[{e['name']}] parameter '{p.name}' has C "
                        f"type {p.c_type!r}, which this backend has no C++ "
                        f"rendering for")
            for o in out_params(m):
                if o.c_type not in _CPP_OUT and o.c_type not in _POD_OUT:
                    problems.append(
                        f"cpp: expose[{e['name']}] out parameter '{o.name}' has "
                        f"C type {o.c_type!r}, which this backend has no C++ "
                        f"rendering for")

        # -- CPP4: every doctest case this backend CITES actually exists.
        #
        # This condition exists because the tree it was written into contained
        # FOUR citations that resolved to nothing -- `test_a_long_owned_string_
        # costs_a_second_host_call` and three like it, snake_case names in the
        # Unity style this directory does not use.  The tests were real; the
        # names were not, so a reader who grepped one found an empty result and
        # had no way to tell whether the test or the sentence was missing.
        #
        # "A false sentence beside a gated table is this repository's
        # most-repeated failure" -- so the citations now carry a MARKER and are
        # checked against the suite rather than against a hand-kept list.  A
        # list would be a second place to forget.
        #
        # GUARDED ON THE MANIFEST BEING RENDERABLE AT ALL, and the guard is the
        # point rather than defensive habit.  CPP4 renders the differential in
        # order to search it, and rendering resolves every expose entry's
        # `vtable` against the host members -- so on a manifest naming a member
        # that does not exist, this raised KeyError and took the whole
        # generator down with a traceback.
        #
        # The NEUTRAL condition reports that same manifest by name, which is
        # strictly the better message, and
        # test_script_bindings_gate.TestManifestGate.
        # test_an_unknown_vtable_name_fails_by_name asserts exactly that it
        # arrives.  It could not: this backend's exception pre-empted it, so a
        # gate whose whole purpose is "fails BY NAME" ERRORed instead --
        # a crash reporting less than a red test, which is the failure this
        # campaign has now recorded four times.
        #
        # Skipping here forfeits nothing.  The run is already failing on a
        # better sentence, and a citation check against a manifest that cannot
        # be rendered has no meaning to lose.
        member_names = {m.name for m in members}
        if all(e["vtable"] in member_names for e in man["expose"]):
            problems += _check_citations(members, man)
        return problems


BACKEND = CppBackend()
