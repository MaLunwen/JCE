#!/usr/bin/env python3
"""
cdecl.py — the one C-declaration parser this repository has.

Lifted verbatim out of check_abi_snapshot.py, which had the only working
implementation: a balanced-brace walk over `typedef struct [tag] { ... }`
bodies, a top-level `;` split for struct fields, and a top-level `,` split
for parameter lists that keeps the array suffix with its type.

It is shared, not copied, because gen_script_bindings.py needs exactly the
same reading of exactly the same headers.  Two parsers over one header is a
two-sided contract, and this repository has been bitten three times in two
weeks by one side of one going stale.

Consumers, both in the tree today:
  check_abi_snapshot.py        — imports split_params
  tools/scriptgen/scriptgen_core.py        — imports split_params_raw

The second consumer used to be gen_script_bindings.py, which was one 948-line
file until it was split into a language-neutral core plus one emit_*.py per
target language.  The core is the importer now; the emitters never touch a
header directly.

They import DIFFERENT normalisations of the same split on purpose; see
split_params_raw's docstring for why, and do not "unify" them back.

Its tests: test_cdecl.py
"""

from __future__ import annotations

import re


# JCE_API / JCE_CALL are export/calling-convention macros, not part of the
# signature's meaning for diff purposes — drop them so a macro rename does not
# look like 3000 ABI changes.
_DECOR = re.compile(r"\b(JCE_API|JCE_CALL|extern)\b")


# A balanced-brace walk, NOT a regex over the body.  The old
#   typedef\s+struct\s*(?:\w+\s*)?\{(?P<body>[^{}]*)\}\s*(?P<name>\w+)\s*;
# could not span a nested anonymous struct or union, so EVERY public record
# containing one was invisible to this gate: JceInputFrame (the record/replay
# WIRE FORMAT), JceEvent (the tagged union an app's on_event receives),
# JceCoroScheduler, JceAudioEffectDesc, JceFootIkOutput, JceAidSolvedField.
# Six records that could be reordered, widened or shrunk with no diff at all --
# and no way to tell that from "no ABI break".
TYPEDEF_STRUCT_OPEN_RE = re.compile(r"typedef\s+struct\s*(?:\w+\s*)?\{")
TYPEDEF_STRUCT_TAIL_RE = re.compile(r"\s*(?P<name>\w+)\s*;")


def strip_comments_and_strings(text: str) -> str:
    """Blank out comments; keep string bodies (they can hold macro values)."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            out.append(" ")
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
            out.append(" ")
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def norm_ws(s: str) -> str:
    return re.sub(r"\s+", " ", s).strip()


def _matching_brace(text: str, open_idx: int) -> int:
    """Index of the '}' closing the '{' at open_idx, or -1 if unterminated."""
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def iter_typedef_struct_bodies(text: str):
    """Yield (name, body) for every `typedef struct [tag] { ... } Name;`."""
    for m in TYPEDEF_STRUCT_OPEN_RE.finditer(text):
        open_idx = m.end() - 1
        close_idx = _matching_brace(text, open_idx)
        if close_idx < 0:
            continue                      # unterminated: header mid-edit
        tail = TYPEDEF_STRUCT_TAIL_RE.match(text, close_idx + 1)
        if not tail:
            continue                      # `} Name, Other;` / not a typedef
        yield tail.group("name"), text[open_idx + 1:close_idx]


def split_struct_fields(body: str) -> list[str]:
    """Split a struct body at TOP-LEVEL semicolons.

    A nested anonymous struct/union stays ONE field entry with its member list
    intact, so growing the nested member changes the emitted line -- which is
    exactly the break the gate must score."""
    fields, depth, cur = [], 0, []
    for ch in body:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        if ch == ";" and depth == 0:
            f = norm_ws("".join(cur))
            if f:
                fields.append(f)
            cur = []
        else:
            cur.append(ch)
    f = norm_ws("".join(cur))
    if f:
        fields.append(f)
    return fields


def split_params_raw(params: str) -> list[str]:
    """Split a parameter list at top-level commas, KEEPING parameter names.

    The raw split.  `split_params` below is this plus a name-dropping
    normalisation for ABI-signature comparison, where `float out_xyz[3]` and
    `float coords[3]` must compare equal.  A binding generator needs the
    opposite: it has to know the parameter is called `out_xyz`, because that
    name becomes the emitted local.

    So the two consumers need different normalisations of ONE split, and this
    is that split.  Do not grow a second comma-walker for the other consumer --
    the whole reason cdecl.py exists is that two walkers over one header drift.

    Guarded by test_cdecl.py::test_split_params_is_split_params_raw_with_names_dropped,
    which fails if the two ever stop agreeing (measured: replacing this body
    with params.split(",") makes it report 6 != 4)."""
    parts, depth, cur = [], 0, []
    for ch in params:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur))
    return parts


def split_params(params: str) -> list[str]:
    """Split a parameter list at top-level commas and drop parameter NAMES.

    NOT usable by a binding generator: the names it drops are exactly what an
    emitter needs, and the array suffix it hoists onto the type turns
    `float out_xyz[3]` into `float[3]`, which re-parses as a parameter NAMED
    "float" with an EMPTY type.  Use split_params_raw for that."""
    out = []
    for p in split_params_raw(params):
        p = norm_ws(_DECOR.sub("", p))
        if not p:
            continue
        if p in ("void", "..."):
            out.append(p)
            continue
        # Array suffix belongs to the type: "char foo[32]" -> "char[32]".
        arr = ""
        m = re.search(r"(\[[^\]]*\])\s*$", p)
        if m:
            arr = m.group(1)
            p = p[: m.start()].rstrip()
        # Drop a trailing identifier (the parameter name), keeping pointers.
        m = re.match(r"^(?P<type>.*?[\s\*])(?P<name>\w+)$", p)
        if m and m.group("name") not in ("void", "int", "char", "float",
                                         "double", "long", "short", "unsigned",
                                         "signed", "bool"):
            p = m.group("type")
        out.append(norm_ws(p) + arr)
    return out
