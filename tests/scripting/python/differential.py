#!/usr/bin/env python3
"""
differential.py — the cross-language differential: Lua versus Python, over one
manifest-derived case book and one shared recording mock host.

WHY THIS IS THE ACCEPTANCE, AND NOT A SUITE OF EXPECTATIONS
-----------------------------------------------------------
Batch 1 spent the one free correctness oracle this surface will ever have: the
71 hand-written Lua bindings the manifest describes were compared against the
generated ones over 87 cases, and then deleted.  Its own commit says the oracle
"disappears the moment they are deleted and never comes back for Python or
Java".

What did NOT disappear is Lua itself.  The generated Lua bindings are now the
reference implementation of this surface, so Python's acceptance is equality
with them:

    the same manifest-derived case, driven through the Lua VM and through the
    ctypes binding, over the SAME mock host, compared on the FULL result (type
    AND value per slot) and on the HOST-CALL TRACE.

Neither side's expected output is written down anywhere.  Both are measured.
An expectation generated from either emitter would be self-consistency rather
than proof, and that mistake has been caught three times in this campaign.

WHAT THE COMPARISON FORBIDS
---------------------------
The two streams are one text each, and they interleave what the script asked
for with what the host was asked to do, in order.  So a difference is any of:

  * a different value in any result slot                (RESULT line)
  * a different TYPE in any result slot -- nil vs 0 vs false vs 0.0 vs "" are
    four different strings, and Lua 5.4's integer/float subtypes map onto
    Python's int/float, so 0 and 0.0 do not compare equal either
  * a different number of result slots                  (RESULT line)
  * a host call one side makes and the other does not   (CALL line)
  * a different argument, in value or in position       (CALL line)
  * a different out-capacity                            (CALL line: `max=`)
  * a release that did not happen, or happened before the value was taken
    (the json_free CALL line's position in the stream, and LIVE)
  * an argument the binding invented or dropped         (arity of the CALL)

MODES
-----
Four host configurations, because the interesting behaviour is not all on the
success path.  The mock's mode is a property of the HOST, so each needs its own
VM and its own JceScriptApi — one process run per mode.

  ok         every member present and answering
  miss       present, answering "no": false, NULL, -3.  The negative is what
             exercises clamp_min (a mock that never answers negative leaves the
             clamp unexercised on BOTH sides, which is an immune mutation with
             a silent reason)
  absent     every member NULL but `log`.  This is where the absent-value
             policy is actually tested: Lua's `""` for a missing string, its
             zero-filled out arrays, the manifest's absent_value for `tr` and
             music_request_transition
  norelease  ok, except json_free is NULL.  NOT compared for equality: it is
             the one place the C ABI and Lua genuinely disagree, and it is
             pinned rather than hidden -- see check_norelease_divergence.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

MODES = {"ok": 0, "miss": 1, "absent": 2, "norelease": 3}

# ── Reading the contract ─────────────────────────────────────────────────────
#
# script-api.json and NOTHING ELSE.  Not emit_python, not _generated.py, not
# the mock: a case derived from the thing under test proves the thing under
# test agrees with itself.

_ARRAY_RE_CACHE: dict[tuple[str, str], int] = {}


def arity_of(sig: str, param: str) -> int:
    """N for a `const T param[N]` input, 0 for a by-value one.

    script-api.json's `params` carries name, c_type and role but NOT arity --
    raycast's `origin` arrives as c_type "const float" with the [3] only in
    c_signature.  Reading it back out here rather than assuming 3 is what keeps
    a future two-element input from silently becoming three."""
    key = (sig, param)
    if key not in _ARRAY_RE_CACHE:
        m = re.search(r"\b" + re.escape(param) + r"\s*\[\s*(\d+)\s*\]", sig)
        _ARRAY_RE_CACHE[key] = int(m.group(1)) if m else 0
    return _ARRAY_RE_CACHE[key]


def arg_value(entry: dict, index: int, param: dict):
    """One deterministic argument.

    Deterministic and DISTINCT per position: the trace records every argument,
    so two parameters that happened to carry the same value would let a
    transposition pass.  Batch 1's M1 is exactly that failure one level down --
    a permutation that perturbed nothing because both sides read the same list.
    """
    t = param["c_type"]
    n = arity_of(entry["c_signature"], param["name"])
    if n:
        return [0.5 + index + i for i in range(n)]
    if t in ("JceScriptEntity", "uint64_t"):
        return 1000 + index
    if t == "uint32_t":
        return 40 + index
    if t == "int":
        return 7 + index
    if t in ("float", "double"):
        return 1.5 + index * 0.25
    if t == "bool":
        return index % 2 == 0
    if t == "const char *":
        return f"{entry['name']}:{param['name']}"
    raise SystemExit(f"differential: no argument rule for {t!r} "
                     f"({entry['name']}.{param['name']})")


def script_params(entry: dict) -> list[dict]:
    """The parameters the SCRIPT supplies: script-api.json's `params` minus the
    ones bind_args binds.  `params` already excludes the out-capacity."""
    bind = entry.get("bind_args") or {}
    return [p for p in entry["params"] if p["name"] not in bind]


# ── The case book ────────────────────────────────────────────────────────────

class Case:
    __slots__ = ("key", "entry", "args", "mode")

    def __init__(self, key: str, entry: dict, args: list, mode: str):
        self.key = key
        self.entry = entry
        self.args = args          # `...` marks "omit from here on"
        self.mode = mode

    def __repr__(self) -> str:
        return f"<Case {self.key}>"


OMIT = object()


def build_cases(api: dict) -> list[Case]:
    """Every case, derived from the manifest alone."""
    cases: list[Case] = []
    for e in api["expose"]:
        params = script_params(e)
        full = [arg_value(e, i, p) for i, p in enumerate(params)]
        for mode in ("ok", "miss", "absent"):
            cases.append(Case(f"{e['name']}/full", e, list(full), mode))

        opt = e.get("optional") or {}
        if opt:
            # Trailing optionals omitted entirely -- the short-arity call a
            # script actually writes.
            keep = len(params)
            while keep > 0 and params[keep - 1]["name"] in opt:
                keep -= 1
            if keep < len(params):
                cases.append(Case(f"{e['name']}/short", e, full[:keep], "ok"))
            # None / nil IN PLACE, which is the only spelling available when a
            # required parameter follows an optional one (gas_apply's `op`).
            nil_in_place = [None if p["name"] in opt else v
                            for p, v in zip(params, full)]
            cases.append(Case(f"{e['name']}/nil_in_place", e, nil_in_place,
                              "ok"))

        base = e.get("index_base")
        if base is not None:
            below = list(full)
            below[0] = base - 1
            cases.append(Case(f"{e['name']}/below_base", e, below, "ok"))

        strict = set(e.get("strict") or [])
        if strict:
            bad = list(full)
            for i, p in enumerate(params):
                if p["name"] in strict:
                    bad[i] = 1          # a truthy NON-boolean
            cases.append(Case(f"{e['name']}/strict_bad", e, bad, "ok"))
        elif any(p["c_type"] == "bool" for p in params):
            # THE CONTROL.  Without it, "raised" would be a category both sides
            # could enter for unrelated reasons and the strict case would prove
            # nothing.  Every OTHER boolean on this surface takes any truthy
            # value (lua_toboolean does), so this case must NOT raise on either
            # side -- and if the emitter type-checked every bool, this is what
            # goes red.
            loose = list(full)
            for i, p in enumerate(params):
                if p["c_type"] == "bool":
                    loose[i] = 1
            cases.append(Case(f"{e['name']}/loose_bool", e, loose, "ok"))

        if e["shape"] == "owned_string_release":
            cases.append(Case(f"{e['name']}/full", e, list(full), "norelease"))
    return cases


# ── The Lua side ─────────────────────────────────────────────────────────────

def lua_literal(v) -> str:
    if v is None:
        return "nil"
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return repr(v)
    if isinstance(v, str):
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
    if isinstance(v, list):                 # a const float[N] input, flattened
        return ", ".join(lua_literal(x) for x in v)
    raise SystemExit(f"differential: no Lua literal for {v!r}")


_LUA_PRELUDE = """\
-- GENERATED BY tests/scripting/python/differential.py. Do not edit.
local function fmt(v)
  if v == nil then return 'nil' end
  local t = type(v)
  if t == 'boolean' then if v then return 'bool:true' else return 'bool:false' end end
  if t == 'number' then
    if math.type(v) == 'integer' then return string.format('int:%d', v) end
    return string.format('num:%.9g', v)
  end
  if t == 'string' then return 'str:' .. v end
  if t == 'table' then
    local p = {}
    for i = 1, #v do p[i] = fmt(v[i]) end
    return 'list[' .. #v .. ']:' .. table.concat(p, ',')
  end
  return 'other:' .. t
end

local function run(name, f)
  jce.log('CASE ' .. name)
  local packed = table.pack(pcall(f))
  if not packed[1] then
    jce.log('RESULT raised')
    return
  end
  local parts = {}
  for i = 2, packed.n do parts[#parts + 1] = fmt(packed[i]) end
  jce.log('RESULT ' .. table.concat(parts, ' '))
end

"""


def emit_lua_chunk(cases: list[Case]) -> str:
    out = [_LUA_PRELUDE]
    for c in cases:
        args = ", ".join(lua_literal(a) for a in c.args)
        out.append(f"run('{c.key}', function() "
                   f"return jce.{c.entry['name']}({args}) end)\n")
    # run_chunk requires the chunk to return a table; without this the
    # instance is 0, every case silently never runs, and the runner's own
    # hard error is what says so.
    out.append("\nlocal M = {}\nreturn M\n")
    return "".join(out)


def run_lua(runner: Path, chunk: str, mode: str, env: dict,
            long_strings: int = 0) -> str:
    with tempfile.NamedTemporaryFile("w", suffix=".lua", delete=False,
                                     newline="\n", encoding="utf-8") as fh:
        fh.write(chunk)
        path = fh.name
    try:
        proc = subprocess.run(
            [str(runner), path, str(MODES[mode]), str(long_strings)],
            capture_output=True, env=env)
        if proc.returncode != 0:
            raise SystemExit(
                f"differential: lua_case_runner exited {proc.returncode} for "
                f"mode {mode}\n--- stderr ---\n"
                f"{proc.stderr.decode('utf-8', 'replace')}\n"
                f"--- stdout ---\n{proc.stdout.decode('utf-8', 'replace')}")
        return proc.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    finally:
        os.unlink(path)


# ── The Python side ──────────────────────────────────────────────────────────

def fmt(v) -> str:
    """The canonical form of one result slot.

    bool BEFORE int, deliberately: bool is a subclass of int in Python, and
    checking int first would render True as `int:1` and make every boolean
    return compare equal to the integer 1."""
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "bool:true" if v else "bool:false"
    if isinstance(v, int):
        return "int:%d" % v
    if isinstance(v, float):
        return "num:%.9g" % v
    if isinstance(v, str):
        return "str:" + v
    if isinstance(v, (list, tuple)):
        return "list[%d]:" % len(v) + ",".join(fmt(x) for x in v)
    return "other:" + type(v).__name__


def python_slots(shape: str, r, key: str) -> list[str]:
    """The result slots, from a Python return value.

    The mapping is per SHAPE and each arm is an ASSERTION, not a coercion: Lua
    returns a variable number of values and Python returns one object, so
    something has to say how many slots that object is.  Saying it as
    `isinstance` checks means an emitter that returned the wrong KIND of thing
    (a bare float where the shape says tuple, a tuple where the shape says
    list) fails here by name instead of being flattened into agreement."""
    if shape == "void_call":
        if r is not None:
            raise AssertionError(
                f"{key}: void_call returned {r!r}; Lua pushes NOTHING, so the "
                f"Python spelling is None and only None")
        return []
    if shape == "entity_table":
        if not isinstance(r, list):
            raise AssertionError(
                f"{key}: entity_table returned {type(r).__name__}, expected "
                f"list (Lua returns ONE table)")
        return [fmt(r)]
    if shape == "first_and_count":
        if not (isinstance(r, tuple) and len(r) == 2):
            raise AssertionError(
                f"{key}: first_and_count returned {r!r}, expected a 2-tuple "
                f"(Lua pushes first-or-nil AND the count)")
        return [fmt(r[0]), fmt(r[1])]
    if shape == "void_out_array":
        if not isinstance(r, tuple):
            raise AssertionError(
                f"{key}: void_out_array returned {type(r).__name__}, "
                f"expected tuple")
        return [fmt(x) for x in r]
    if shape == "fallible_out":
        if isinstance(r, tuple):
            return [fmt(x) for x in r]
        return [fmt(r)]
    return [fmt(r)]              # value_return, owned_string_release


def run_python(api, mock, cases: list[Case]) -> None:
    for c in cases:
        mock.jce_mock_note(("CASE " + c.key).encode("utf-8"))
        method = getattr(api, c.entry["name"])
        try:
            r = method(*c.args)
        except Exception:                                  # noqa: BLE001
            # Every raise is one category, on purpose.  Comparing a Lua error
            # message against a Python exception message would be comparing two
            # unrelated strings that a careless assertion can make identical --
            # which is the defect batch 1 found in its own harness.
            mock.jce_mock_note(b"RESULT raised")
            continue
        slots = python_slots(c.entry["shape"], r, c.key)
        mock.jce_mock_note(("RESULT " + " ".join(slots)).encode("utf-8"))


# ── Loading the two shared libraries ─────────────────────────────────────────

def load_mock(mock_path: Path):
    mock = ctypes.CDLL(str(mock_path))
    mock.jce_mock_host.restype = ctypes.c_void_p
    mock.jce_mock_host.argtypes = [ctypes.c_int]
    mock.jce_mock_host_size.restype = ctypes.c_size_t
    mock.jce_mock_host_size.argtypes = []
    mock.jce_mock_sizeof_raycast_hit.restype = ctypes.c_size_t
    mock.jce_mock_sizeof_raycast_hit.argtypes = []
    mock.jce_mock_reset.restype = None
    mock.jce_mock_reset.argtypes = [ctypes.c_int]
    mock.jce_mock_note.restype = None
    mock.jce_mock_note.argtypes = [ctypes.c_char_p]
    mock.jce_mock_trace_text.restype = ctypes.c_char_p
    mock.jce_mock_trace_text.argtypes = []
    mock.jce_mock_live_strings.restype = ctypes.c_int
    mock.jce_mock_live_strings.argtypes = []
    mock.jce_mock_set_long_strings.restype = None
    mock.jce_mock_set_long_strings.argtypes = [ctypes.c_int]
    return mock


def python_stream(jce_script, lib, mock, mode: str, cases: list[Case]) -> str:
    mock.jce_mock_reset(MODES[mode])
    mock.jce_mock_set_long_strings(0)
    host = mock.jce_mock_host(MODES[mode])
    if not host:
        raise SystemExit("differential: jce_mock_host returned NULL")
    api = jce_script.open_host(int(host), int(mock.jce_mock_host_size()), lib)
    try:
        run_python(api, mock, cases)
    finally:
        text = mock.jce_mock_trace_text().decode("utf-8", "replace")
        live = mock.jce_mock_live_strings()
        jce_script.close(api)
    return text + "LIVE %d\n" % live


# ── Comparison ───────────────────────────────────────────────────────────────

def diff(lua: str, py: str, mode: str) -> list[str]:
    a, b = lua.split("\n"), py.split("\n")
    out: list[str] = []
    for i in range(max(len(a), len(b))):
        la = a[i] if i < len(a) else "<end of Lua stream>"
        lb = b[i] if i < len(b) else "<end of Python stream>"
        if la != lb:
            ctx = next((a[j] for j in range(min(i, len(a) - 1), -1, -1)
                        if a[j].startswith("CASE ")), "<no CASE line>")
            out.append(f"  mode {mode}, line {i + 1}, in {ctx}\n"
                       f"      lua    : {la}\n"
                       f"      python : {lb}")
            if len(out) >= 12:
                out.append(f"  ... (stopping after {len(out)} differences)")
                break
    return out


def check_norelease_divergence(lua: str, py: str) -> list[str]:
    """The ONE place the C ABI and Lua genuinely disagree, asserted rather than
    excused.

    With the producer present and json_free NULL, the two surfaces cannot
    agree, and neither of them is this backend's code:

      Lua      jce_script_bindings.gen.c calls the producer, pushes the string
               and skips the free -- so the script gets a value and the host
               LEAKS it (live = 2 after both entries).
      C ABI    jce_script_api.gen.c refuses: `if (!api || !api->host.<member>
               || !api->host.<release>) return -1;`.  Nothing is produced and
               nothing leaks, and the Python binding sees None.

    Pinning it here means the divergence is a recorded decision of the C ABI
    rather than something a future reader discovers as a Python bug.  If either
    side changes, this goes red and names which one moved."""
    problems: list[str] = []
    if "CALL comp_get_json" not in lua:
        problems.append(
            "norelease: the Lua side did NOT call comp_get_json — its binding "
            "guards only the producer, so it is expected to call it and leak")
    if "CALL comp_get_json" in py:
        problems.append(
            "norelease: the Python side DID call comp_get_json — the C ABI "
            "forwarder is supposed to refuse when the release member is NULL, "
            "which is the only reason nothing leaks on this path")
    if "RESULT nil" in lua:
        problems.append(
            "norelease: the Lua side answered nil — its binding guards the "
            "producer alone, so with the producer present it is expected to "
            "answer the STRING and leak it")
    if "RESULT nil" not in py:
        problems.append(
            "norelease: the Python side did not answer None — the C ABI "
            "forwarder returns -1 when the release member is NULL")
    if not re.search(r"^LIVE 2$", lua, re.M):
        problems.append(
            f"norelease: expected the Lua side to leave 2 strings live "
            f"(comp_get and render_get, both leaked); stream ends "
            f"{lua.strip().splitlines()[-1:]}")
    if not re.search(r"^LIVE 0$", py, re.M):
        problems.append(
            "norelease: expected the Python side to leave 0 strings live")
    return problems


def check_long_string_retry(jce_script, lib, mock, runner, env) -> list[str]:
    """The copy-out retry, and what it costs.

    The C ABI hands back a COPY, so a string longer than the Python binding's
    initial buffer forces a second call — and the forwarder calls the host and
    releases on EVERY call, so the retry produces and frees twice.  Lua, which
    receives the host's pointer, always does it once.

    Both halves are asserted: the VALUE must be identical across the two
    languages (a retry that returned a truncated string would be a real defect)
    and the CALL COUNTS must be exactly 1 and 2 (a retry that never happened
    would leave the path untested, and a retry that leaked would show in
    LIVE)."""
    problems: list[str] = []
    entry = {"name": "comp_get", "shape": "owned_string_release",
             "c_signature": "", "params": []}
    case = Case("comp_get/long", entry, [1000, "comp_get:type"], "ok")

    lua = run_lua(runner, emit_lua_chunk([case]), "ok", env, long_strings=1)

    mock.jce_mock_reset(MODES["ok"])
    mock.jce_mock_set_long_strings(1)
    host = mock.jce_mock_host(MODES["ok"])
    api = jce_script.open_host(int(host), int(mock.jce_mock_host_size()), lib)
    try:
        run_python(api, mock, [case])
    finally:
        py = mock.jce_mock_trace_text().decode("utf-8", "replace")
        live = mock.jce_mock_live_strings()
        jce_script.close(api)
        mock.jce_mock_set_long_strings(0)

    lua_value = _result_of(lua, "comp_get/long")
    py_value = _result_of(py, "comp_get/long")
    if lua_value != py_value:
        problems.append(
            f"long string: the two languages disagree on the VALUE, which the "
            f"retry must not change\n      lua    : {lua_value[:80]}...\n"
            f"      python : {py_value[:80]}...")
    if len(lua_value) < 9000:
        problems.append(
            f"long string: the mock produced only {len(lua_value)} characters "
            f"— shorter than the binding's initial buffer, so the retry path "
            f"was never entered and this check tested nothing")
    n_lua = lua.count("CALL comp_get_json")
    n_py = py.count("CALL comp_get_json")
    f_py = py.count("CALL json_free")
    if n_lua != 1:
        problems.append(f"long string: Lua called the producer {n_lua} times, "
                        f"expected 1 (it receives the pointer directly)")
    if n_py != 2:
        problems.append(
            f"long string: Python called the producer {n_py} times, expected "
            f"exactly 2 — one that found the buffer too small and one that "
            f"fit. This is the documented cost of the C ABI's copy-out.")
    if f_py != n_py:
        problems.append(
            f"long string: Python produced {n_py} strings and released "
            f"{f_py} — every copy-out call must release, including the one "
            f"whose result was thrown away")
    if live != 0:
        problems.append(f"long string: {live} strings still live after the "
                        f"retry; the discarded first copy was not released")
    return problems


def _result_of(stream: str, key: str) -> str:
    lines = stream.split("\n")
    for i, ln in enumerate(lines):
        if ln == "CASE " + key:
            for nxt in lines[i + 1:]:
                if nxt.startswith("RESULT "):
                    return nxt[len("RESULT "):]
    return "<no RESULT for %s>" % key


# ── The surface's own shape ──────────────────────────────────────────────────

_KEYS_CHUNK = """\
local n = {}
for k, v in pairs(jce) do n[#n + 1] = k end
table.sort(n)
jce.log('KEYS ' .. table.concat(n, ' '))
local M = {}
return M
"""


def check_key_sets(jce_script, api_json: dict, runner: Path,
                   env: dict) -> list[str]:
    """The Python surface against the LIVE Lua table, read with pairs().

    Through the reader, not through the manifest.  The manifest is what both
    were generated from, so comparing Python's names against it would prove
    only that the generator agrees with itself; reading the keys out of a
    running VM is blind to how any of them got there.

    This is also where json_null is settled.  It is IN the Lua table and it is
    NOT in the Python surface, and the difference must be accounted for by a
    non-empty reason in NOT_EXPOSED.  `jce.json_null == jce.json_null` -- the
    assertion batch 1 found comparing a field with itself -- would be true of
    any value whatever; this compares two independently produced SETS."""
    problems: list[str] = []
    out = run_lua(runner, _KEYS_CHUNK, "ok", env)
    line = next((ln for ln in out.split("\n") if ln.startswith("KEYS ")), None)
    if line is None:
        return ["key set: the Lua VM produced no KEYS line — the probe chunk "
                "did not run"]
    lua_keys = set(line[len("KEYS "):].split())
    py_names = set(jce_script.ENTRY_NAMES)

    # DERIVED, not written down.  This was a literal 81 with the arithmetic
    # spelled out beside it in a comment, so every APPEND to JceScriptHost --
    # the one change the ABI rules explicitly permit -- failed here with a
    # message that named no cause and pointed at no file.  The manifest is
    # already in scope; it is the thing both sides are checked against.
    want_keys = (len(api_json["expose"]) + len(api_json["hand_written"])
                 + len(api_json["constants"]))
    if len(lua_keys) != want_keys:
        problems.append(
            f"key set: the live jce table has {len(lua_keys)} keys, expected "
            f"{want_keys} ({len(api_json['expose'])} generated + "
            f"{len(api_json['hand_written'])} hand-written + "
            f"{len(api_json['constants'])} constant)")
    extra = sorted(py_names - lua_keys)
    if extra:
        problems.append(
            f"key set: Python exposes {extra}, which the Lua table does not "
            f"have — the Python surface must be a SUBSET of the scripting "
            f"surface, never wider than it")
    missing = sorted(lua_keys - py_names)
    unexplained = [n for n in missing if not jce_script.NOT_EXPOSED.get(n)]
    if unexplained:
        problems.append(
            f"key set: {unexplained} are in the Lua table, absent from Python, "
            f"and carry NO reason in NOT_EXPOSED — a name may not leave the "
            f"surface silently")
    stale = sorted(set(jce_script.NOT_EXPOSED) - lua_keys)
    if stale:
        problems.append(
            f"key set: NOT_EXPOSED explains {stale}, which the Lua table does "
            f"not contain — the exclusion list is stale")
    if "json_null" not in missing:
        problems.append(
            "key set: json_null was expected to be present in Lua and absent "
            "from Python; it is not")
    if len(py_names) != len(api_json["expose"]):
        problems.append(
            f"key set: ENTRY_NAMES carries {len(py_names)} names but the "
            f"manifest exposes {len(api_json['expose'])}")
    return problems


def check_struct_layout(jce_script_gen, mock) -> list[str]:
    want = int(mock.jce_mock_sizeof_raycast_hit())
    got = ctypes.sizeof(jce_script_gen._RaycastHit)
    if got != want:
        return [f"layout: ctypes.sizeof(_RaycastHit) is {got}, C says {want} — "
                f"the flattened mirror of JceScriptRaycastHit does not match "
                f"the struct the host fills, so every raycast slot is read "
                f"from the wrong offset"]
    return []


def check_mock_modes(mock_header: Path) -> list[str]:
    """The mode numbers in the C header and in MODES here are one contract.

    A comment saying "kept in sync" is a defect unless something fails when it
    is not; this is that something."""
    text = mock_header.read_text(encoding="utf-8")
    problems: list[str] = []
    for name, value in MODES.items():
        want = f"#define JCE_MOCK_MODE_{name.upper():<9} {value}"
        if not re.search(rf"#define\s+JCE_MOCK_MODE_{name.upper()}\s+{value}\b",
                         text):
            problems.append(
                f"mock modes: {mock_header.name} does not define "
                f"JCE_MOCK_MODE_{name.upper()} as {value} (differential.py's "
                f"MODES says it is)")
    defined = set(re.findall(r"#define\s+JCE_MOCK_MODE_(\w+)\s+\d+", text))
    if defined != {m.upper() for m in MODES}:
        problems.append(
            f"mock modes: the header defines {sorted(defined)} and this driver "
            f"knows {sorted(m.upper() for m in MODES)} — one of them grew a "
            f"mode the other cannot reach")
    return problems


# ── Driver ───────────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True, type=Path)
    ap.add_argument("--runner", required=True, type=Path)
    ap.add_argument("--mock", required=True, type=Path)
    ap.add_argument("--api", required=True, type=Path)
    args = ap.parse_args()
    # Absolute, always: os.add_dll_directory rejects a relative path outright
    # (WinError 87), and ctypes.CDLL on a relative name would search PATH
    # instead of the file that was asked for -- which is how a stale copy of a
    # DLL earlier on PATH gets tested instead of the one just built.
    args.repo = args.repo.resolve()
    args.runner = args.runner.resolve()
    args.mock = args.mock.resolve()
    args.api = args.api.resolve()

    for label, p in (("runner", args.runner), ("mock", args.mock),
                     ("api", args.api)):
        if not p.is_file():
            print(f"jce-python-differential: FAILED\n    the {label} was not "
                  f"built: {p}")
            return 1

    sys.path.insert(0, str(args.repo / "scripting" / "python"))
    import jce_script                                        # noqa: E402
    from jce_script import _generated                        # noqa: E402

    api_json = json.loads(
        (args.repo / "contracts/script-api.json").read_text("utf-8"))

    env = dict(os.environ)
    sep = ";" if sys.platform == "win32" else ":"
    env["PATH"] = sep.join([str(args.mock.parent), str(args.api.parent),
                            env.get("PATH", "")])

    if sys.platform == "win32" and hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(args.api.parent))
        os.add_dll_directory(str(args.mock.parent))
    lib = jce_script.load_library(args.api)
    mock = load_mock(args.mock)

    cases = build_cases(api_json)
    problems: list[str] = []
    compared = 0

    for mode in ("ok", "miss", "absent", "norelease"):
        mine = [c for c in cases if c.mode == mode]
        if not mine:
            continue
        lua = run_lua(args.runner, emit_lua_chunk(mine), mode, env)
        py = python_stream(jce_script, lib, mock, mode, mine)

        # THE GATE PROVES IT RAN.  A chunk that compiled but produced nothing,
        # or a Python loop that skipped every case, would otherwise compare two
        # empty streams and pass.
        for side, text in (("lua", lua), ("python", py)):
            n = text.count("\nCASE ") + text.startswith("CASE ")
            if n != len(mine):
                problems.append(
                    f"  mode {mode}: the {side} side recorded {n} CASE lines, "
                    f"expected {len(mine)} — a run that tested nothing must "
                    f"fail, not compare two empty streams")
        if mode != "norelease":
            # LIVE compared between the two sides is NOT enough, and that is a
            # measured gap rather than a precaution: a mutation that stopped
            # the mock accounting for releases moves BOTH sides to the same
            # wrong number and the diff stays green.  The absolute zero is what
            # makes "the release happened" an assertion instead of a
            # coincidence.
            for side, text in (("lua", lua), ("python", py)):
                if not re.search(r"^LIVE 0$", text, re.M):
                    tail = text.strip().split("\n")[-1:]
                    problems.append(
                        f"  mode {mode}: the {side} side ended with {tail} — "
                        f"every owned string this mode produced must have been "
                        f"released, and LIVE says how many were not")
        if mode == "norelease":
            problems += ["  " + p for p in check_norelease_divergence(lua, py)]
        else:
            d = diff(lua, py, mode)
            problems += d
            if not d:
                compared += len(mine)

    problems += ["  " + p for p in check_key_sets(jce_script, api_json,
                                                  args.runner, env)]
    problems += ["  " + p for p in check_struct_layout(_generated, mock)]
    problems += ["  " + p for p in check_mock_modes(
        args.repo / "tests/scripting/python/jce_script_py_mock.h")]
    problems += ["  " + p for p in check_long_string_retry(
        jce_script, lib, mock, args.runner, env)]

    if problems:
        print("jce-python-differential: FAILED")
        for p in problems:
            print(p)
        return 1

    entries = {c.entry["name"] for c in cases}
    print(f"jce-python-differential: OK — {len(cases)} cases over "
          f"{len(entries)}/{len(api_json['expose'])} entries in "
          f"{len(MODES)} host modes; {compared} streams byte-identical "
          f"between the Lua VM and the ctypes binding, "
          f"{len(jce_script.ENTRY_NAMES)} Python names against 79 live Lua "
          f"table keys")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
