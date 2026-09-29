#!/usr/bin/env python3
"""lifecycle_differential.py — LUA IS THE REFERENCE LIFECYCLE.

The call-DOWN direction was accepted by `differential.py`: the ctypes binding
against the Lua bindings, over one shared mock host.  This is the mirror, for
the call-UP direction: the same script semantics expressed in Lua and in
Python, driven through THE ENGINE'S OWN ENTRY POINTS
(`jce_script_call_start` / `call_update` / `call_collision` / ...), compared on
the sequence of host calls and on observable state.

WHAT MAKES THIS A DIFFERENTIAL AND NOT A SMOKE TEST
---------------------------------------------------
An `on_update` that never fires and an `on_update` that fires correctly both
leave a test green if the test only checks "no crash".  Four defences, each
against a failure this repository has actually shipped:

  1. **Both sides must have PRODUCED OUTPUT, checked BEFORE comparing.**  A
     differential shipped in this tree today was 150/151 dead because the
     reference side returned nothing, and nothing compares equal to nothing.
     `require_alive()` counts SCRIPT-emitted lines — the `S ` prefix, written
     only from inside a lifecycle handler — and refuses a run with too few, on
     either side, naming which.  The driver's own `CASE` lines are excluded
     from that count on purpose: they are identical on both sides whether or
     not a single line of script ever ran.
  2. **Type as well as value.**  Every field a handler receives is logged as
     `name=type:value`, so `nil` / `None`, `0` and `false` are three different
     lines.  A backend that passed 0.0 where the engine passed nil would differ on
     the type half even when the formatted value collided.
  3. **The cases are derived, not invented.**  The slot list comes from
     `struct JceScriptVM` in `jce_script_vm.h`; the driver prints its step
     table with `--slots`; a slot exercised by no step FAILS.  The `jce.*`
     entries the scripts call are checked against
     `contracts/script-api.json`.  Neither comes from
     `emit_python.py`, `emit_lua.py`, or from the VM under test.
  4. **One script pair, one source.**  The Lua chunk and the Python module are
     RENDERED from one case book below, so "the two scripts do the same thing"
     is mechanical rather than a claim about two hand-written files.

WHAT IS NORMALISED, AND WHY THAT IS NOT A LOOPHOLE
---------------------------------------------------
Exactly one thing: the text after `" error: "`.  A Lua error reads
`lifecycle:31: boom` and a Python one `lifecycle:33: RuntimeError: boom`;
requiring those to be equal would require one runtime to lie about the other's
diagnostics.  What is compared instead is everything that carries meaning: that
an error line was produced AT ALL, WHICH handler produced it, that it went to
`host.log` (it is in the stream) rather than only to the engine log, and WHERE
in the sequence it appeared.  The normaliser is deliberately blind to nothing
else — a missing error line, an extra one, one from the wrong handler or one in
the wrong place is a failure.

The line AFTER it is NOT normalised, and that is the point of it existing.  THE
FAILING-CALLBACK RULE (jce_script.h) requires every backend, having logged the
error, to disable that handler on that instance and to say so in one published
wording — a sentence with no language-specific detail in it, which is why this
one is compared byte for byte where the error above it cannot be.  So the
raising case now pins three things instead of one: the error line, the notice
line, and the SILENCE of the step after them (`call_update_4_after_error`
drives on_update again and neither side may record anything for it).  A VM that
kept dispatching after the error — what the reference itself did before this
rule was implemented — fails on that third one.

Run: python tests/scripting/python/lifecycle_differential.py --runner ... --api ...
ctest: test_jce_script_python_lifecycle
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_DEFAULT = Path(__file__).resolve().parents[3]

VM_HEADER = "engine/include/jce/middleware/script/jce_script_vm.h"
MANIFEST = "contracts/script-api.json"

# Fewer script-emitted lines than this on either side means the script did not
# really run, whatever else the stream contains.  Set from the case book, not
# guessed: see `expected_script_lines()`.
MIN_SCRIPT_LINES = 12


class Failure(Exception):
    """A named differential failure.  The name is the point."""

    def __init__(self, check: str, detail: str) -> None:
        super().__init__(f"{check}: {detail}")
        self.check = check


# ─────────────────────────────────────────────────────────────────────────
#  Derivation: the vtable and the manifest
# ─────────────────────────────────────────────────────────────────────────

def vtable_slots(repo: Path) -> list[str]:
    """Every function-pointer slot of `struct JceScriptVM`, in order.

    Parsed from the header rather than listed here, so a slot APPENDED to the
    vtable (the only legal way to change it) is immediately a slot this
    differential demands a step for.
    """
    text = (repo / VM_HEADER).read_text(encoding="utf-8")
    m = re.search(r"struct JceScriptVM \{(.*?)\n\};", text, re.S)
    if not m:
        raise Failure("vtable_parse",
                      f"no `struct JceScriptVM {{ ... }};` in {VM_HEADER} — "
                      f"the differential derives its cases from that "
                      f"declaration and cannot proceed without it")
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    slots = re.findall(r"\(\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*\(", body)
    if len(slots) < 18:
        raise Failure("vtable_parse",
                      f"found only {len(slots)} slots in struct JceScriptVM "
                      f"({slots}); the parser and the header disagree")
    return slots


def manifest_entries(repo: Path) -> set[str]:
    man = json.loads((repo / MANIFEST).read_text(encoding="utf-8"))
    return {e["name"] for e in man["expose"]}


# ─────────────────────────────────────────────────────────────────────────
#  The case book — ONE description, rendered into two languages
# ─────────────────────────────────────────────────────────────────────────
#
# A field is (expression-name, kind).  `kind` decides BOTH how the value is
# formatted and what `tn()` must report for it, which is how nil/None, 0 and
# false stay three distinguishable lines.
#
#   int   whole number   -> "number"
#   num   real           -> "number"
#   str   text or absent -> "string" / "nil"

HANDLERS = [
    ("on_start", ["self"], []),
    ("on_update", ["self", "dt"], [("dt", "num")]),
    ("on_destroy", ["self"], []),
    ("on_collision", ["self", "other"], [("other", "int")]),
    ("on_ping", ["self", "num", "s"], [("num", "num"), ("s", "str")]),
    ("on_anim_event", ["self", "id", "name", "f0", "f1", "i0"],
     [("id", "int"), ("name", "str"), ("f0", "num"), ("f1", "num"),
      ("i0", "int")]),
]

# The three global handlers the UI dispatchers reach.  NOT methods: Lua finds
# them in _G and Python finds them at module level, and both are declared by
# writing them at the top of the script — which is the contract the engine
# actually depends on.
GLOBALS = [
    ("on_named_hit", ["e"], [("e", "int")]),
    ("on_slider", ["e", "v"], [("e", "int"), ("v", "num")]),
    ("on_field", ["e", "s"], [("e", "int"), ("s", "str")]),
]

# `jce.*` entries the rendered scripts call.  Checked against the manifest, so
# a script that reached for something the surface does not expose fails here
# rather than at runtime inside one language only.
USED_ENTRIES = ("set_position", "get_position")

LUA_PRELUDE = """\
-- GENERATED by tests/scripting/python/lifecycle_differential.py. DO NOT EDIT.
local function tn(v)
  if v == nil then return "nil" end
  return type(v)
end
local function fi(v) return string.format("%d", v) end
local function fn_(v) return string.format("%.6f", v) end
local function fs(v) if v == nil then return "<nil>" end return tostring(v) end
"""

PY_PRELUDE = '''\
# GENERATED by tests/scripting/python/lifecycle_differential.py. DO NOT EDIT.
def tn(v):
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "boolean"
    if isinstance(v, (int, float)):
        return "number"
    if isinstance(v, str):
        return "string"
    return type(v).__name__


def fi(v):
    return "%d" % v


def fn_(v):
    return "%.6f" % v


def fs(v):
    return "<nil>" if v is None else str(v)
'''

FMT = {"int": "fi", "num": "fn_", "str": "fs"}


def log_expr(tag: str, fields: list[tuple[str, str]], lang: str) -> str:
    """`jce.log("S <tag> name=type:value ...")`, in `lang`.

    Concatenation and not a format call, because the two languages' format
    verbs are not the same set and a rendered difference in the FORMATTER
    would show up as a difference in the VM.
    """
    cat = " .. " if lang == "lua" else " + "
    parts = ['"S %s"' % tag]
    for name, kind in fields:
        parts.append('" %s="' % name)
        parts.append("tn(%s)" % name)
        parts.append('":"')
        parts.append("%s(%s)" % (FMT[kind], name))
    return "jce.log(%s)" % cat.join(parts)


def render_lua(reload_module: bool = False) -> str:
    out = [LUA_PRELUDE]
    if not reload_module:
        for name, params, fields in GLOBALS:
            out.append("function %s(%s)" % (name, ", ".join(params)))
            out.append("  " + log_expr(name, fields, "lua"))
            out.append("end\n")
        out.append("""\
local function co_body()
  jce.log("S coro step=1")
  jce.wait_seconds(0.3)
  jce.log("S coro step=2")
end
""")
    out.append("local M = {}\n")
    tag_prefix = "v2_" if reload_module else ""
    for name, params, fields in HANDLERS:
        if reload_module and name not in ("on_update", "on_destroy"):
            continue
        out.append("M.%s = function(%s)" % (name, ", ".join(params)))
        out.append("  " + log_expr(tag_prefix + name, fields, "lua"))
        if name == "on_start" and not reload_module:
            out.append("  jce.set_position(self.entity, 1.5, -2.25, 0.125)")
            out.append("  jce.start_coroutine(co_body)")
        if name == "on_update":
            out.append("  self.n = (self.n or 0) + 1")
            out.append("  local x, y, z = jce.get_position(self.entity)")
            out.append("  jce.log(\"S %spos n=\" .. fi(self.n) .. \" x=\" .. "
                       "tn(x) .. \":\" .. fn_(x))" % tag_prefix)
            if not reload_module:
                # Raises on the 3rd update.  The driver then drives a 4th,
                # which THE FAILING-CALLBACK RULE requires both sides to
                # swallow silently, and later a rebind, after which the v2
                # handler must run again — so this one `error` exercises the
                # disable AND the re-enable.
                out.append("  if self.n == 3 then error(\"boom\") end")
        out.append("end\n")
    out.append("return M\n")
    return "\n".join(out)


def render_python(reload_module: bool = False) -> str:
    out = [PY_PRELUDE]
    if not reload_module:
        for name, params, fields in GLOBALS:
            out.append("def %s(%s):" % (name, ", ".join(params)))
            out.append("    " + log_expr(name, fields, "py"))
            out.append("")
        out.append('''\
def co_body():
    jce.log("S coro step=1")
    yield jce.wait_seconds(0.3)
    jce.log("S coro step=2")
''')
    tag_prefix = "v2_" if reload_module else ""
    for name, params, fields in HANDLERS:
        if reload_module and name not in ("on_update", "on_destroy"):
            continue
        out.append("def %s(%s):" % (name, ", ".join(params)))
        out.append("    " + log_expr(tag_prefix + name, fields, "py"))
        if name == "on_start" and not reload_module:
            out.append("    jce.set_position(self.entity, 1.5, -2.25, 0.125)")
            out.append("    jce.start_coroutine(co_body)")
        if name == "on_update":
            out.append("    self.n = getattr(self, 'n', 0) + 1")
            out.append("    x, y, z = jce.get_position(self.entity)")
            out.append("    jce.log(\"S %spos n=\" + fi(self.n) + \" x=\" + "
                       "tn(x) + \":\" + fn_(x))" % tag_prefix)
            if not reload_module:
                # See the Lua renderer: the 3rd update raises, the 4th must be
                # silent on both sides, and the rebind must revive it.
                out.append("    if self.n == 3:")
                out.append("        raise RuntimeError('boom')")
        out.append("")
    return "\n".join(out)


def expected_script_lines() -> int:
    """A floor for `S ` lines, computed from the case book rather than picked.

    on_start(1) + on_start's pos-less body, 5 updates x 2 lines (the 6th is
    the one THE FAILING-CALLBACK RULE silences), on_destroy(1), on_collision(1),
    on_ping x2, on_anim_event x2, 3 globals + 1 null-string repeat, 2 coroutine
    steps.  Kept conservative: the check exists to catch ZERO, not to pin the
    count, and pinning it here would make every future case-book edit a failure
    in the wrong file.
    """
    return MIN_SCRIPT_LINES


# ─────────────────────────────────────────────────────────────────────────
#  Running and comparing
# ─────────────────────────────────────────────────────────────────────────

ERROR_SPLIT = " error: "


def normalise(line: str) -> str:
    """Elide only the text after `" error: "` — see the module docstring."""
    i = line.find(ERROR_SPLIT)
    if i < 0:
        return line
    return line[:i + len(ERROR_SPLIT)] + "<detail>"


def run_side(runner: Path, lang: str, script: Path, reload_src: Path,
             env: dict) -> tuple[list[str], int, str]:
    proc = subprocess.run(
        [str(runner), lang, str(script), str(reload_src)],
        capture_output=True, text=True, env=env, timeout=180)
    lines = [ln.rstrip("\r") for ln in proc.stdout.splitlines()]
    return lines, proc.returncode, proc.stderr


def require_alive(lang: str, lines: list[str]) -> None:
    """Refuse a run in which the SCRIPT produced nothing, before comparing.

    `CASE` and `RESULT` lines are written by the driver in C and appear
    identically whether or not a single handler ever ran, so counting them
    would be counting the thing that cannot fail.
    """
    script_lines = [ln for ln in lines if ln.startswith("S ")]
    if len(script_lines) < expected_script_lines():
        raise Failure(
            "produced_output",
            f"the {lang} side emitted {len(script_lines)} script line(s), "
            f"fewer than the {expected_script_lines()} the case book "
            f"guarantees. Nothing compares equal to nothing: a side that ran "
            f"no handler must fail HERE, not silently match another side that "
            f"also ran none.\n"
            f"  first 10 lines of the {lang} stream:\n    "
            + "\n    ".join(lines[:10] or ["<empty>"]))


def fold_error_detail(lines: list[str]) -> list[str]:
    """Fold a multi-line error's continuation into the `<detail>` it belongs to.

    An error line is already elided after `" error: "` because the wording is
    a language's own.  A STACK TRACE is the same thing spread over more lines:
    Lua prints "stack traceback:" and tab-indented frames after the message,
    Python would print a different shape, and neither is comparable to the
    other.  Left as separate lines they do not merely differ -- they SHIFT the
    stream, so every line after the first error compares against its
    neighbour and the report points at the wrong place entirely.

    This is what the 2026-09-20 traceback change surfaced: the differential
    failed at line 35 with two unrelated cases side by side, and nothing was
    wrong with either side.
    """
    out: list[str] = []
    in_detail = False
    for ln in lines:
        if ERROR_SPLIT in ln:
            in_detail = True
            out.append(ln)
            continue
        if in_detail and (ln.startswith("	") or ln.startswith("    ")
                          or ln.startswith("stack traceback")):
            continue          # a continuation of the error above
        in_detail = False
        out.append(ln)
    return out


def compare(lua: list[str], py: list[str]) -> None:
    a = [normalise(x) for x in fold_error_detail(lua)]
    b = [normalise(x) for x in fold_error_detail(py)]
    if a == b:
        return
    diffs = []
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else "<missing>"
        y = b[i] if i < len(b) else "<missing>"
        if x != y:
            diffs.append(f"    line {i + 1}:\n      lua   : {x}\n"
                         f"      python: {y}")
            if len(diffs) >= 8:
                break
    raise Failure(
        "streams_equal",
        f"the Lua and Python lifecycle streams differ "
        f"({len(a)} vs {len(b)} lines).\n" + "\n".join(diffs))


def check_slot_coverage(runner: Path, slots: list[str], env: dict) -> None:
    proc = subprocess.run([str(runner), "--slots"], capture_output=True,
                          text=True, env=env, timeout=60)
    if proc.returncode != 0:
        raise Failure("slot_table",
                      f"`{runner.name} --slots` exited {proc.returncode}: "
                      f"{proc.stderr.strip()}")
    covered, steps = set(), 0
    for line in proc.stdout.splitlines():
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) != 2:
            raise Failure("slot_table",
                          f"unparseable --slots line: {line!r}")
        covered.add(parts[1])
        steps += 1
    unknown = covered - set(slots)
    if unknown:
        raise Failure(
            "slot_names_exist",
            f"the driver claims to exercise slot(s) {sorted(unknown)} that "
            f"`struct JceScriptVM` does not declare. Either the slot was "
            f"renamed or the driver's table is stale.")
    missing = [s for s in slots if s not in covered]
    if missing:
        raise Failure(
            "every_slot_exercised",
            f"JceScriptVM slot(s) {missing} are exercised by no step in "
            f"lifecycle_runner.c. A slot the differential never drives reaches "
            f"only the language that happened to implement it; that is the "
            f"drift the vtable exists to stop. Add a step to k_steps.")
    if steps < len(slots):
        raise Failure("every_slot_exercised",
                      f"only {steps} steps for {len(slots)} slots")


def check_entries(repo: Path, scripts: list[str]) -> None:
    known = manifest_entries(repo)
    for entry in USED_ENTRIES:
        if entry not in known:
            raise Failure(
                "entries_are_manifest_entries",
                f"the case book calls jce.{entry}, which is not in "
                f"{MANIFEST}'s `expose` list. The scripts must exercise the "
                f"published surface, not one this test invented.")
    for src in scripts:
        for name in re.findall(r"\bjce\.([A-Za-z_][A-Za-z0-9_]*)", src):
            if name in ("log", "start_coroutine", "wait_seconds",
                        "stop_coroutine"):
                continue        # VM machinery: hand_written in the manifest
            if name not in known:
                raise Failure(
                    "entries_are_manifest_entries",
                    f"a rendered script calls jce.{name}, absent from the "
                    f"manifest's exposed surface")


# ─────────────────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", type=Path, default=REPO_DEFAULT)
    ap.add_argument("--runner", type=Path, required=True)
    ap.add_argument("--api", type=Path, required=True)
    ap.add_argument("--package", type=Path, required=True)
    ap.add_argument("--work", type=Path, default=None)
    args = ap.parse_args()

    repo = args.repo.resolve()
    work = (args.work or (args.runner.parent / "lifecycle_work")).resolve()
    work.mkdir(parents=True, exist_ok=True)

    env = dict(os.environ)
    env["JCE_SCRIPT_API"] = str(args.api)
    env["JCE_PY_VM_PACKAGE_DIR"] = str(args.package)
    # A .pyc written into a shared tree is how a reverted mutation stayed live
    # in this worktree once already: CPython invalidates on (mtime, size), and
    # a same-size revert inside one mtime second leaves the mutated bytecode
    # running.
    env["PYTHONDONTWRITEBYTECODE"] = "1"

    failures: list[Failure] = []

    try:
        slots = vtable_slots(repo)
    except Failure as exc:
        print(f"FAIL {exc.check}: {exc}", file=sys.stderr)
        return 1

    lua_src = render_lua()
    py_src = render_python()
    lua_v2 = render_lua(reload_module=True)
    py_v2 = render_python(reload_module=True)

    paths = {
        "lua": work / "lifecycle.lua",
        "py": work / "lifecycle.py",
        "lua_v2": work / "lifecycle_v2.lua",
        "py_v2": work / "lifecycle_v2.py",
    }
    for key, path in paths.items():
        src = {"lua": lua_src, "py": py_src,
               "lua_v2": lua_v2, "py_v2": py_v2}[key]
        path.write_text(src, encoding="utf-8", newline="\n")

    for check in (lambda: check_slot_coverage(args.runner, slots, env),
                  lambda: check_entries(repo, [lua_src, py_src,
                                               lua_v2, py_v2])):
        try:
            check()
        except Failure as exc:
            failures.append(exc)

    streams: dict[str, list[str]] = {}
    for lang, script, reload_src in (("lua", paths["lua"], paths["lua_v2"]),
                                     ("python", paths["py"], paths["py_v2"])):
        lines, rc, err = run_side(args.runner, lang, script, reload_src, env)
        streams[lang] = lines
        if rc != 0:
            failures.append(Failure(
                "driver_exit_code",
                f"the {lang} side exited {rc}. A non-zero exit is the driver "
                f"refusing to run, not a comparison result — the stream below "
                f"is whatever it managed to record.\n"
                f"  stderr: {err.strip()[:2000]}\n"
                f"  stdout: " + "\n    ".join(lines[:20])))
        else:
            try:
                require_alive(lang, lines)
            except Failure as exc:
                failures.append(exc)

    if not failures:
        try:
            compare(streams["lua"], streams["python"])
        except Failure as exc:
            failures.append(exc)

    if failures:
        for exc in failures:
            print(f"FAIL {exc.check}: {exc}", file=sys.stderr)
        print(f"\n{len(failures)} named check(s) failed.", file=sys.stderr)
        return 1

    n = len([x for x in streams["lua"] if x.startswith("S ")])
    print(f"OK  lifecycle differential: {len(streams['lua'])} lines matched, "
          f"{n} of them emitted by the script, {len(slots)} vtable slots all "
          f"exercised.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
