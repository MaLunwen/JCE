#!/usr/bin/env python3
"""
scriptgen_core.py — everything about the scripting surface that is NOT a language.

This module holds what every binding backend must agree on, and nothing a
single backend may decide for itself:

  * the C-declaration decomposition of `struct JceScriptHost` (74 typed
    members, parameter names, arity, and the `const`-decides-direction rule);
  * the CLOSED SET OF SEVEN return shapes and the nine manifest modifiers —
    the shared vocabulary, defined ONCE so four backends cannot each grow a
    private dialect of it;
  * the manifest (`script_exposure.json`) and its validation conditions;
  * `contracts/script-api.json`, the language-neutral contract every
    backend reads;
  * the byte-identity check that keeps the committed artefacts equal to a
    fresh emit.

Backends live in sibling `emit_<language>.py` modules.  `emit_lua.py` is the
first; `emit_python.py`, `emit_java.py` and `emit_cpp.py` are expected to
arrive independently and IN PARALLEL, which is the reason this split exists:
one 948-line file cannot be edited by four authors at once, and batch 1 had to
serialise four tasks through it for exactly that reason.


REGISTERING A BACKEND — one new file, nothing else edited
─────────────────────────────────────────────────────────
A backend is a file matching `emit_*.py` in this directory that defines a
module-level name `BACKEND`.  Discovery is a sorted glob (`discover_backends`
below), so adding one touches NO existing file: not this core, not the entry
point, not another backend.  There is deliberately no central list to edit and
therefore no line for three agents to collide on.

Worked example — the whole of a new backend's registration:

    #!/usr/bin/env python3
    # tools/scriptgen/emit_python.py
    from scriptgen_core import (REPO_ROOT, Artefact, ScriptBackend,
                                script_in_params, out_params, c_literal)

    GEN_C = REPO_ROOT / "engine/src/middleware/script/jce_script_py.gen.c"

    def emit_py_c(members, man):
        by = {m.name: m for m in members}
        out = [_BANNER]
        for e in man["expose"]:                 # authored order, always
            out.append(_emit_one(by[e["vtable"]], e))
        return "".join(out)

    class PythonBackend(ScriptBackend):
        name = "python"

        def artefacts(self, members, man):
            return [Artefact(GEN_C, emit_py_c)]

        def validate(self, members, man, c_text):
            return []          # or this backend's own conditions, by name

    BACKEND = PythonBackend()

`Artefact.render` is always called as `render(members, man)`, even when a
particular artefact ignores one of them; a uniform signature is what lets the
driver treat every artefact identically in all three modes.

Two things a backend MUST NOT do, because doing either re-creates the collision
this split removes:

  * add a shape.  The set of seven is closed.  If a binding does not fit one,
    mark it `hand_written` in the manifest — that decision is recorded per
    binding and is visible to every other backend.
  * edit another backend's file, or this one, to make its own emit work.  If
    something genuinely neutral is missing here, add it here as a NEW name;
    do not change the meaning of an existing one under three other readers.


RULES THAT SURVIVE THE SPLIT — read these before writing an emitter
───────────────────────────────────────────────────────────────────
1. `vtable_index` IS EMITTED FOR TRACEABILITY ONLY.  NO BINDING MAY KEY OFF
   IT.  Nothing enforces this — it is a rule for backend authors, and this is
   the place a backend author reads.  `jce_script_create_sized` copies
   min(host_size, sizeof s->host) over a zeroed table, which is what makes a
   caller compiled against an older, SHORTER header safe; a binding that
   reached a member by slot index would defeat exactly that.  Reach members by
   NAME, always.

2. THE HOST-MEMBER GUARD IS AN ABI FACT, NOT A LUA FACT.  The same short-struct
   copy leaves any member the caller's header lacked NULL, so every call
   through `s->host.<member>` must be guarded on both `s->have_host` and the
   member pointer.  That REQUIREMENT is language-neutral and binds every
   backend that calls the host through this struct.  The SPELLING is not:
   `s->have_host && s->host.x`, its De Morgan dual in a folded early return,
   and the `JceScript *s = jce_script_self_from_upvalue(L);` prologue that
   produces `s` are all `emit_lua.py`'s, because `self_from_upvalue` is a
   lua_State upvalue mechanism.  A backend whose glue reaches the host
   differently owes the same guarantee in its own spelling — and owes a test
   that fails when it is missing (`emit_lua.py` names its own).

3. `owned_string_release` KEEPS A SEPARATE NULL CHECK ON THE RELEASE MEMBER.
   Same reason as (2), one level finer: a partial host may supply the producer
   and not the releaser.  Neutral requirement, per-backend spelling.

4. GENERATED ARTEFACTS ARE COMMITTED, NOT BUILT.  `engine/CMakeLists.txt:43`
   globs sources at CONFIGURE time, so a `.c` produced during the build joins
   the target only on the NEXT build — a first build from a clean clone would
   link a scripting layer with no bindings at all.  Every backend commits its
   output and keeps it byte-identical to a fresh emit; the default (check) mode
   is what proves it.

5. THE SPLIT IS 71/7 BY DECISION, NOT BY CAPABILITY.  `asset_read_text` and
   `asset_read_json` are hand-written PERMANENTLY: a templated `read_file`
   emits no path validator, no size cap and no bounded JSON walker, which is a
   generated sandbox escape.  Do not "finish the job" by generating them.

Python 3.12, standard library only: ci.yml:25 pins 3.12 and the fast lint tier
at ci.yml:40 runs BEFORE `pip install conan` at :54, so a gate needing
pycparser or libclang could not sit there.

Its tests: test_script_bindings_gate.py
"""

from __future__ import annotations

import difflib
import importlib.util
import json
import re
import sys
from collections import namedtuple
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTGEN_DIR = Path(__file__).resolve().parent
# cdecl.py sits beside this file, not under tools/audit/ where it was born:
# tools/audit/ is no longer tracked, and the ONE C-declaration parser this
# repo has (5e232821: "one C-declaration parser, not two") is a generator
# dependency, not an audit tool.  Its consumers are all in this directory.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from cdecl import (                                        # noqa: E402
    iter_typedef_struct_bodies,
    norm_ws,
    split_params_raw,
    split_struct_fields,
    strip_comments_and_strings,
)

HEADER = REPO_ROOT / "engine/include/jce/middleware/script/jce_script.h"
SCRIPT_C = REPO_ROOT / "engine/src/middleware/script/jce_script.c"
MANIFEST = REPO_ROOT / "engine/src/middleware/script/script_exposure.json"
API_JSON = REPO_ROOT / "contracts/script-api.json"

Param = namedtuple("Param", "name c_type arity")
HostMember = namedtuple("HostMember", "name ret params index")

# `RET (*NAME)(PARAMS)` -- the only member shape this generator understands.
# If a member ever needs more than this, mark its binding hand-written; do NOT
# deepen this into a second C front end (spec 5.2).
_MEMBER_RE = re.compile(r"^(?P<ret>.+?)\s*\(\s*\*\s*(?P<name>\w+)\s*\)\s*"
                        r"\((?P<params>.*)\)$", re.S)
_PARAM_RE = re.compile(r"^(?P<type>.*?)(?P<stars>[\s*]*)(?P<name>\w+)"
                       r"(?:\[(?P<n>\d*)\])?$", re.S)


def _parse_param(text: str) -> Param:
    """One parameter -> (name, c_type, arity).

    arity  0  by value, INCLUDING `const char *` (a string argument)
           N  `T name[N]`
          -1  a non-const pointer, i.e. an OUT parameter

    `const` is what separates an input array from an output array.  raycast
    takes two `const float[3]` INPUTS and fills one non-const struct OUTPUT; a
    parser that called `origin` an out parameter would emit a binding that
    reads no arguments and pushes fourteen values.  Likewise `const char *msg`
    left as arity -1 would marshal every string argument backwards."""
    m = _PARAM_RE.match(norm_ws(text))
    if not m:
        raise ValueError(f"unparsable parameter: {text!r}")
    base = norm_ws(m.group("type"))
    stars = m.group("stars").count("*")
    is_const = base.startswith("const ")
    if m.group("n") is not None:                 # T name[N] / T name[]
        n = m.group("n")
        return Param(m.group("name"), base, int(n) if n else -1)
    if stars == 1 and is_const:                  # const char * -> by value
        return Param(m.group("name"), base + " *", 0)
    if stars >= 1:                               # T * -> out pointer
        return Param(m.group("name"), base, -1)
    return Param(m.group("name"), base, 0)


def parse_host_members(header_text: str) -> list[HostMember]:
    """Decompose struct JceScriptHost into its function-pointer members.

    `void *user` is a data field and is skipped: counting it would put the
    totality check permanently off by one."""
    text = strip_comments_and_strings(header_text)
    body = dict(iter_typedef_struct_bodies(text)).get("JceScriptHost")
    if body is None:
        raise SystemExit("error: struct JceScriptHost not found in "
                         f"{HEADER.relative_to(REPO_ROOT).as_posix()}")
    out: list[HostMember] = []
    for field in split_struct_fields(body):
        m = _MEMBER_RE.match(field)
        if not m:
            continue                      # `void *user` and nothing else today
        # split_params_raw, NOT split_params: the ABI splitter drops parameter
        # names and hoists the array suffix onto the type, so `float out_xyz[3]`
        # reaches us as `float[3]` and re-parses as a parameter NAMED "float"
        # with an empty type -- silently, with the member count still 74.
        # test_out_array_arity_is_read_from_the_declaration is what fails.
        params = [_parse_param(p) for p in split_params_raw(m.group("params"))
                  if norm_ws(p) not in ("", "void")]
        out.append(HostMember(m.group("name"), norm_ws(m.group("ret")),
                              params, len(out)))
    return out


def flatten_pod(header_text: str, type_name: str) -> list[tuple[str, str]]:
    """Flatten a POD out-struct into (access_expression, scalar_c_type).

    raycast's out parameter is JceScriptRaycastHit*, which the hand-written
    binding pushes as eight scalars.  Reading the struct is DERIVED, so the
    manifest never has to restate a C type (spec 4.1)."""
    text = strip_comments_and_strings(header_text)
    body = dict(iter_typedef_struct_bodies(text)).get(type_name)
    if body is None:
        raise SystemExit(f"error: struct {type_name} not found in header")
    out: list[tuple[str, str]] = []
    for field in split_struct_fields(body):
        p = _parse_param(field)
        if p.arity > 0:
            out.extend((f"{p.name}[{i}]", p.c_type) for i in range(p.arity))
        else:
            out.append((p.name, p.c_type))
    return out


# ── The shared vocabulary ────────────────────────────────────────────────────
#
# SEVEN shapes, closed.  A backend that wants an eighth marks that binding
# hand_written instead -- the decision then lives in the manifest where every
# other backend can see it, rather than inside one emitter where they cannot.
SHAPES = frozenset({"void_call", "value_return", "fallible_out", "void_out_array",
                    "first_and_count", "entity_table", "owned_string_release"})

# The nine modifiers.  Spec 4.3 names four (bind_args, out_capacity,
# index_base, optional) and 4.1 names release; the other four were forced by
# MEASURED hand-written bodies -- absent_value (music_request_transition
# returns -1.0f with no host; tr echoes its key), miss_value (raycast pushes
# integer 0 on a miss, not nil), clamp_min (get_touch_count clamps a negative
# host result to 0) and strict (set_parent alone calls luaL_checktype on its
# boolean).  They describe the CONTRACT, not the Lua rendering of it, so a
# Python or Java backend reads the same key and renders it its own way.
MODIFIERS = frozenset({"bind_args", "out_capacity", "index_base", "optional",
                       "release", "absent_value", "miss_value", "clamp_min",
                       "strict"})

# Non-modifier keys an expose entry may carry.  `deprecated` is not used today
# and is admitted deliberately: check_append_only tells authors that a rename
# is "an add plus deprecated:true on the old one", and a vocabulary check that
# rejected the remedy its sibling prescribes would be a trap.
_ENTRY_KEYS = frozenset({"name", "vtable", "shape", "since", "doc", "deprecated"})


def shape_is_type_compatible(shape: str, m: HostMember) -> bool:
    """CONDITION 3 IS A TYPE-COMPATIBILITY CHECK, NOT A CORRECTNESS CHECK.

    find_by_name and find_by_prefix extract to the same signature and carry
    different Lua contracts, so this function CANNOT tell a 2-element tuple
    from a 1024-element table and must never be described as if it could.
    Only the differential harness catches a swapped shape (spec 4.2).
    test_shape_check_cannot_tell_find_by_name_from_find_by_prefix asserts that
    the swap passes here, so nobody can later mistake this for correctness."""
    ret = m.ret
    outs = [p for p in m.params[1:] if p.arity != 0]
    if shape == "void_call":
        return ret == "void"
    if shape == "value_return":
        return ret != "void" and not outs
    if shape == "fallible_out":
        return ret == "bool" and len(outs) >= 1
    if shape == "void_out_array":
        return ret == "void" and len(outs) == 1 and outs[0].arity > 0
    if shape in ("first_and_count", "entity_table"):
        return ret == "int" and len(outs) == 1 and outs[0].arity == -1
    if shape == "owned_string_release":
        return ret in ("char *", "char*") and not outs
    return False


# ── Parameter roles: which arguments does the SCRIPT supply? ─────────────────
#
# Neutral, and provably so: emit_api_json -- the published, language-neutral
# contract -- is built from script_in_params/out_params, so the answer these
# give IS the documented signature every backend must implement.  A backend
# that computed its own would be a second implementation of a two-sided
# contract, which is the failure this whole effort exists to remove.
#
# `const` decides direction.  A const array is an INPUT the script supplies
# (raycast's origin/dir); a non-const array or pointer is an OUTPUT the host
# fills.  `void *user` is m.params[0] and never reaches either.
def in_params(m: HostMember) -> list[Param]:
    return [p for p in m.params[1:]
            if p.arity == 0 or (p.arity > 0 and p.c_type.startswith("const "))]


def out_params(m: HostMember) -> list[Param]:
    return [p for p in m.params[1:]
            if p.arity < 0 or (p.arity > 0 and not p.c_type.startswith("const "))]


def capacity_param(m: HostMember, shape: str) -> Param | None:
    """The `int max` the GENERATOR supplies -- NOT an argument the script passes.

    first_and_count / entity_table members are declared

        int (*find_by_name)(void *user, const char *name,
                            JceScriptEntity *out, int max);

    `max` is by value, so in_params classifies it as an input, and it is not
    one: its value is the manifest's out_capacity and the emitter already
    appends it after `found`.  Left in the input list it does two things at
    once -- reads a second Lua argument no caller passes (so
    `jce.find_by_name("x")` raises "bad argument #2"), and calls a
    four-parameter member with five arguments.  The second half is a compile
    error, but this file is COMMITTED UNCOMPILED in the task that generates
    it, so nothing would have caught it before the harness.

    test_every_emitted_call_passes_the_declared_number_of_arguments is what
    fails when this regresses; it checks all 71, not just these two."""
    if shape not in ("first_and_count", "entity_table"):
        return None
    outs = out_params(m)
    idx = m.params.index(outs[0])
    tail = m.params[idx + 1:]
    if len(tail) != 1 or tail[0].c_type != "int" or tail[0].arity != 0:
        raise SystemExit(
            f"error: {m.name}: shape {shape!r} requires the out pointer to be "
            f"followed by exactly one `int` capacity parameter; found "
            f"{[(p.c_type, p.name) for p in tail]}.  Mark the binding "
            f"hand_written rather than widening this rule (spec 4.3).")
    return tail[0]


def script_in_params(m: HostMember, ent: dict) -> list[Param]:
    """The parameters the SCRIPT supplies: in_params minus the capacity the
    generator fills from out_capacity.  Every emitter and script-api.json read
    this, so the documented signature and the emitted one cannot disagree."""
    cap = capacity_param(m, ent["shape"])
    return [p for p in in_params(m) if cap is None or p.name != cap.name]


def c_literal(v) -> str:
    """A manifest scalar rendered as a C literal.

    C, not Lua: `bind_args`, `optional` defaults, `clamp_min` and `miss_value`
    all end up inside emitted C, and the three backends whose glue is C share
    this rendering.  A backend that emits its host glue in some other language
    simply does not call this."""
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, float):
        return repr(v)
    return str(v)


# The zero value of a C type, for INITIALISING a local before the host fills
# it -- `float out_xyz[3] = { 0.0f, 0.0f, 0.0f }`.  That use is a C fact.
#
# Its OTHER use is not: emit_lua also spends this table as the value a binding
# yields when the host member is absent, which is a per-language CONTRACT
# decision (a Python backend would answer None where Lua answers 0.0, and
# `"const char *": '""'` is a choice to hand back an empty string rather than
# nil).  Kept here because the C half is shared; a backend that wants a
# different absent-value policy writes its own table and leaves this one alone.
C_ZERO = {"float": "0.0f", "double": "0.0", "bool": "false", "int": "0",
          "uint32_t": "0", "uint64_t": "0", "JceScriptEntity": "0",
          "const char *": '""'}


# ── The backend seam ─────────────────────────────────────────────────────────

# render is ALWAYS called as render(members, man); see the module docstring.
Artefact = namedtuple("Artefact", "path render")


class ScriptBackend:
    """One target language's emitter.  Subclass, instantiate, and bind the
    instance to a module-level `BACKEND` in an `emit_*.py` file next to this
    one.  Nothing else registers it."""

    name = "<unnamed>"

    def artefacts(self, members: list[HostMember], man: dict) -> list[Artefact]:
        """The committed files this backend owns, in a stable order."""
        return []

    def validate(self, members: list[HostMember], man: dict,
                 c_text: str) -> list[str]:
        """This backend's OWN manifest conditions, as human-readable failures.

        The neutral conditions (member existence, shape legality, totality,
        the citation check, the declared totals) run in `validate()` for every
        backend and are not repeated here.  What belongs here is anything that
        reads this backend's own sources -- Lua's registration parity reads
        `register_binding(` and `l_*(lua_State *L)`, which mean nothing to a
        Python or Java TU."""
        return []


_BACKENDS_CACHE: list[ScriptBackend] | None = None


def _rel(path: Path) -> str:
    """Repo-relative if it can be, absolute otherwise.

    Path.relative_to RAISES on a path outside REPO_ROOT, so a bare
    relative_to() inside an error message turns a clear "this backend file is
    broken" into an unrelated ValueError from the reporting code itself."""
    try:
        return path.relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def _load_backend_module(path: Path):
    """Import one emit_*.py, ONCE.

    Keyed on the plain module name in sys.modules so that a caller who did
    `import emit_lua` and this loader end up with the SAME module object.  Two
    instances would mean two `_PUSH` tables and two sets of module state, which
    is the drift `cdecl.py` was extracted to end -- in the loader this time."""
    name = path.stem
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    try:
        spec.loader.exec_module(mod)
    except Exception:
        del sys.modules[name]
        raise
    return mod


def discover_backends(force: bool = False) -> list[ScriptBackend]:
    """Every emit_*.py in this directory, sorted by filename.

    A backend that fails to import, or that defines no BACKEND, is a HARD
    FAILURE naming the file -- deliberately not a skip.  A skipped backend
    cannot have its artefacts checked, so its committed output would go stale
    silently while this tool printed OK; that is the same reasoning that makes
    a MISSING generated file a failure rather than a skip in diff_artefact."""
    global _BACKENDS_CACHE
    if _BACKENDS_CACHE is not None and not force:
        return _BACKENDS_CACHE
    found: list[ScriptBackend] = []
    for path in sorted(SCRIPTGEN_DIR.glob("emit_*.py")):
        mod = _load_backend_module(path)
        backend = getattr(mod, "BACKEND", None)
        # Evict the rejected module before raising, or an author who fixes the
        # file and re-runs discovery in the same process gets the broken copy
        # back out of sys.modules and concludes the fix did not take.
        if backend is None:
            sys.modules.pop(path.stem, None)
            raise SystemExit(
                f"error: {_rel(path)} matches the backend glob emit_*.py but "
                f"defines no module-level BACKEND. Add `BACKEND = MyBackend()` "
                f"at the end of the file, or rename the file so it is not a "
                f"backend.")
        if not isinstance(backend, ScriptBackend):
            sys.modules.pop(path.stem, None)
            raise SystemExit(
                f"error: {_rel(path)}: BACKEND is {type(backend).__name__}, "
                f"not a scriptgen_core.ScriptBackend subclass -- the driver "
                f"calls .artefacts() and .validate() on it and would fail "
                f"later, less clearly.")
        found.append(backend)
    _BACKENDS_CACHE = found
    return found


# ── The manifest, and its conditions ─────────────────────────────────────────

def load_manifest() -> dict:
    return json.loads(MANIFEST.read_text(encoding="utf-8"))


def claimed_members(man: dict) -> set[str]:
    """Every member any bucket claims -- expose (plus its release member),
    hand_written (plus its also_reaches member), internal."""
    out: set[str] = set()
    for e in man["expose"]:
        out.add(e["vtable"])
        if e.get("release"):
            out.add(e["release"])
    for e in man["hand_written"]:
        if e.get("vtable"):
            out.add(e["vtable"])
        if e.get("also_reaches"):
            out.add(e["also_reaches"])
    out.update(man.get("internal", []))
    return out


# ── Condition 9: the prose's line citations, checked ─────────────────────────
#
# NOT in spec 5.3.  Added because it was measured: of the ten citations this
# manifest carried when it was first drafted (nine into jce_script.c, one into
# jce_script.h), TWO pointed at the wrong line -- `log` named :70, the bare
# `else`, for a LOG_INFO on :71, and `set_parent` named :106, a
# luaL_checkinteger, for the luaL_checktype on :108.  "The table is gated, the
# sentences around it are not" is this repository's most-repeated failure, and
# a manifest whose whole job is to justify hand-authored decisions is made of
# those sentences.
#
# The rule: the cited line must share at least one identifier with the text
# that cites it.  KNOWN CEILING, stated rather than left to be discovered: a
# citation that slips one line inside a multi-line expression can still share
# an identifier and survive.  This catches renumbering and rot, not off-by-one
# within one statement.  test_a_stale_line_citation_is_caught is what fails.
#
# The file prefix is OPTIONAL because nine of the ten citations are written
# bare, as `:417`.  A regex demanding `jce_script.c:` matched three of the ten
# and its own closure count agreed with it, which is what a gate looks like
# when it checks almost nothing; test_the_citation_check_is_not_vacuous is
# what caught that and is what keeps catching it.  A bare `:NNN` means
# jce_script.c -- the manifest may not contain an unrelated `:NNN`.
_CITE_RE = re.compile(r"(?:jce_script\.(c|h))?:(\d+)")
_IDENT_RE = re.compile(r"[A-Za-z_]\w*")

# Excluded because a line consisting only of these says nothing about WHICH
# line it is -- `else` alone would otherwise anchor any citation to any
# else-branch in the file, which is exactly the :70/:71 error.
_C_KEYWORDS = frozenset("""
auto break case char const continue default do double else enum extern float
for goto if inline int long register restrict return short signed sizeof static
struct switch typedef union unsigned void volatile while bool true false NULL
""".split())


def _idents(text: str) -> set[str]:
    return {w for w in _IDENT_RE.findall(text)
            if len(w) >= 3 and w not in _C_KEYWORDS}


def _entry_blobs(man: dict) -> list[tuple[str, str]]:
    """(label, all prose in that entry) for every citable region."""
    out = [("_comment", " ".join(man.get("_comment", [])))]
    for bucket in ("expose", "hand_written", "constants"):
        for e in man.get(bucket, []):
            text = " ".join(v for v in e.values() if isinstance(v, str))
            out.append((f"{bucket}[{e['name']}]", text))
    return out


def check_citations(man: dict, c_text: str, header_text: str) -> list[str]:
    src = {"c": c_text.split("\n"), "h": header_text.split("\n")}
    problems: list[str] = []
    seen = 0
    for label, blob in _entry_blobs(man):
        want = _idents(blob)
        for ext, num in _CITE_RE.findall(blob):
            seen += 1
            ext = ext or "c"
            n = int(num)
            lines = src[ext]
            if not (1 <= n <= len(lines)):
                problems.append(
                    f"{label}: cites jce_script.{ext}:{n}, which is past the "
                    f"end of a {len(lines)}-line file")
                continue
            line = lines[n - 1]
            if not (_idents(line) & want):
                # Every later task in this batch moves code inside
                # jce_script.c, so this WILL fire on an honest edit.  Naming
                # the nearby lines that do match turns "you broke it" into
                # "you probably meant :753" -- it is a HINT, deliberately
                # plural and unranked, not an answer to paste in unread.
                near = sorted((i + 1 for i in range(max(0, n - 61),
                                                    min(len(lines), n + 60))
                               if _idents(lines[i]) & want),
                              key=lambda k: abs(k - n))
                hint = (f"; nearest lines that do: "
                        f"{', '.join(str(k) for k in near[:4])}"
                        if near else "")
                problems.append(
                    f"{label}: cites jce_script.{ext}:{n}, but that line shares "
                    f"no identifier with the text citing it -- the line is "
                    f"{line.strip()!r}{hint}")
    total = len(_CITE_RE.findall(json.dumps(man)))
    if seen != total:
        problems.append(
            f"the citation check saw {seen} of {total} jce_script.*:NNN "
            f"citations -- one is outside expose / hand_written / constants / "
            f"_comment, where nothing checks it")
    return problems


def validate(members: list[HostMember], man: dict, c_text: str,
             header_text: str | None = None,
             backends: list[ScriptBackend] | None = None) -> list[str]:
    """Conditions 3, 4, 8 and the local citation check, plus every registered
    backend's own conditions.  Returns human-readable failures; empty is clean.

    Condition 5 -- registration parity -- USED TO LIVE HERE and does not any
    more, because it is not neutral: it reads `register_binding(` and
    `static int l_x(lua_State *L)`, both of which are Lua.  It now arrives via
    backend.validate(), in the same position in the output it always held."""
    problems: list[str] = []
    by_name = {m.name: m for m in members}
    totals = man["declared_totals"]

    # --- condition 3: the member exists, and shape is LEGAL for its signature
    for e in man["expose"]:
        m = by_name.get(e["vtable"])
        if m is None:
            problems.append(
                f"expose[{e['name']}]: names host member '{e['vtable']}', which "
                f"does not exist in struct JceScriptHost")
            continue
        if e["shape"] not in SHAPES:
            problems.append(
                f"expose[{e['name']}]: shape '{e['shape']}' is not one of the "
                f"seven closed shapes {sorted(SHAPES)}")
        elif not shape_is_type_compatible(e["shape"], m):
            problems.append(
                f"expose[{e['name']}]: shape '{e['shape']}' is not type-compatible "
                f"with `{m.ret} (*{m.name})(...)`")
        if e.get("release") and e["release"] not in by_name:
            problems.append(
                f"expose[{e['name']}]: release member '{e['release']}' does not exist")
        # The modifier vocabulary, closed the same way the shapes are.  An
        # unrecognised key is not a harmless extra: every emitter reads
        # modifiers by exact name with .get(), so `clampmin` or `optionals`
        # is SILENTLY IGNORED and the binding emits the unmodified body --
        # in four backends at once, none of which can see the typo.
        # test_an_unknown_modifier_key_fails_by_name is what fails.
        for key in sorted(set(e) - _ENTRY_KEYS - MODIFIERS):
            problems.append(
                f"expose[{e['name']}]: unknown key '{key}' -- an emitter reads "
                f"modifiers by exact name and would ignore it silently. The "
                f"nine modifiers are {sorted(MODIFIERS)}")

    # --- condition 4: TOTALITY.  The anti-drift lock.
    claimed = claimed_members(man)
    for m in members:
        if m.name not in claimed:
            problems.append(
                f"host member '{m.name}' (index {m.index}) has NO exposure "
                f"decision: add it to expose / hand_written / internal in "
                f"{MANIFEST.relative_to(REPO_ROOT).as_posix()}")
    for name in sorted(claimed - set(by_name)):
        problems.append(f"manifest claims host member '{name}', which no longer exists")

    # --- every backend's own conditions, in discovery order
    for backend in (discover_backends() if backends is None else backends):
        problems += backend.validate(members, man, c_text)

    # --- condition 9 (local): the prose's line citations still point at code
    problems += check_citations(
        man, c_text,
        HEADER.read_text(encoding="utf-8") if header_text is None else header_text)

    # --- condition 8: THE GATE PROVES IT RAN.
    counted = {
        "host_members": len(members),
        "expose": len(man["expose"]),
        "hand_written": len(man["hand_written"]),
        "constants": len(man["constants"]),
        "table_keys": len(man["expose"]) + len(man["hand_written"]) + len(man["constants"]),
    }
    gen_members = set()
    for e in man["expose"]:
        gen_members.add(e["vtable"])
        if e.get("release"):
            gen_members.add(e["release"])
    counted["members_reached_by_generated"] = len(gen_members)
    counted["members_reached_only_by_hand_written"] = len(set(by_name) - gen_members)
    for k, declared in totals.items():
        if counted.get(k) != declared:
            problems.append(
                f"declared_totals.{k} = {declared} but the gate counted "
                f"{counted.get(k)} -- a run that checked nothing must fail, not "
                f"print a cheerful OK")
    return problems


# ── The neutral artefact: contracts/script-api.json ──────────────────

def emit_api_json(members: list[HostMember], man: dict) -> str:
    """The extension_api.json analogue: every authored field plus the resolved
    C facts.  vtable_index is TRACEABILITY ONLY — no binding keys off it.

    This is the file emit_python.py / emit_java.py / emit_cpp.py are expected
    to READ, which is why it is emitted by the core rather than by any one
    backend: it is the description of the surface, not a rendering of it."""
    by = {m.name: m for m in members}

    def sig(m: HostMember) -> str:
        ps = []
        for p in m.params:
            if p.arity > 0:
                ps.append(f"{p.c_type} {p.name}[{p.arity}]")
            elif p.arity < 0:
                ps.append(f"{p.c_type} *{p.name}")
            else:
                ps.append(f"{p.c_type} {p.name}")
        return f"{m.ret} (*)({', '.join(ps)})"

    entries = []
    for e in man["expose"]:
        m = by[e["vtable"]]
        entries.append({
            **e,
            "vtable_index": m.index,
            "c_signature": sig(m),
            # script_in_params, not in_params: the out-capacity `max` is
            # supplied by the generator, and documenting it as a parameter of
            # jce.find_by_name would be a two-sided contract that disagrees
            # with the emitted body.  test_api_json_params_match_the_emitted
            # _lua_arity is what fails.
            "params": [{"name": p.name, "c_type": p.c_type, "role": "in"}
                       for p in script_in_params(m, e)],
            "out_params": [{"name": p.name, "c_type": p.c_type, "arity": p.arity}
                           for p in out_params(m)],
            "nullable_host_member": True,
        })
    doc = {
        "script_api_version": man["script_api_version"],
        "_generated_by": "python tools/scriptgen/gen_script_bindings.py --write",
        "_note": ("vtable_index is emitted for traceability only. No binding may "
                  "key off it: the append-only rule plus the min(caller, engine) "
                  "copy over a zeroed destination is what makes a short host "
                  "safe, and a slot-indexed binding would defeat it."),
        "declared_totals": man["declared_totals"],
        "constants": man["constants"],
        "hand_written": man["hand_written"],
        "expose": entries,
    }
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def core_artefacts(members: list[HostMember], man: dict) -> list[Artefact]:
    """The artefacts no backend owns.  Emitted LAST so that the driver's
    failure output keeps the order it has had since batch 1."""
    return [Artefact(API_JSON, emit_api_json)]


def check_append_only(old: dict, new: dict) -> list[str]:
    """Condition 6.  REMOVED / renamed / since-changed fail; ADDED passes."""
    problems = []
    for bucket in ("expose", "hand_written", "constants"):
        o = {e["name"]: e for e in old.get(bucket, [])}
        n = {e["name"]: e for e in new.get(bucket, [])}
        for name in sorted(set(o) - set(n)):
            problems.append(
                f"REMOVED {bucket}[{name}] — entries are never renamed or "
                f"removed; a rename is an add plus deprecated:true on the old one")
        for name in sorted(set(o) & set(n)):
            if o[name].get("since") != n[name].get("since"):
                problems.append(
                    f"CHANGED {bucket}[{name}].since "
                    f"{o[name].get('since')} -> {n[name].get('since')}")
    return problems


# ── Writing and checking committed artefacts ─────────────────────────────────

# newline="\n" on EVERY write.  Path.write_text() applies platform line-ending
# translation on Windows, so an unmarked write produces a CRLF file against an
# LF-committed one and condition 1 goes permanently red on every Windows
# machine for a reason that has nothing to do with bindings.
def write_lf(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


# The suite lives under tests/, which this repository does not track.  On a
# clone, the artefacts that land there have nowhere to be, and asking whether
# they match a fresh emit is not a question the clone can answer.
TESTS_ROOT = REPO_ROOT / "tests"


def artefact_is_unverifiable(path: Path) -> bool:
    """True only when the artefact's WHOLE tree is absent from this checkout.

    This is deliberately not "the file is missing".  An artefact missing from a
    tests/ tree that EXISTS is still a failure -- `rm <one artefact>` must not
    read as success, which is the entire reason diff_artefact has a MISSING
    branch.  Only a wholly absent tests/ qualifies, because that is a property
    of the repository rather than of the file, and the caller reports it on
    every run instead of passing over it quietly.
    """
    return not TESTS_ROOT.exists() and TESTS_ROOT in path.parents


def diff_artefact(path: Path, want: str) -> list[str]:
    """Conditions 1 and 2.  A MISSING generated file is a FAILURE, explicitly
    not a SKIP: skipping would make `rm jce_script_bindings.gen.c` read as
    success, and a generator whose check goes quiet when its output disappears
    is checking nothing.  The one case that is genuinely unanswerable rather
    than broken -- an absent tests/ tree -- is filtered by the caller through
    artefact_is_unverifiable and REPORTED, not skipped."""
    rel = path.relative_to(REPO_ROOT).as_posix()
    if not path.is_file():
        return [f"MISSING {rel} — the generated file is gone. This is a FAILURE, "
                f"not a skip. Regenerate with:\n"
                f"        python tools/scriptgen/gen_script_bindings.py --write"]
    have = path.read_text(encoding="utf-8")
    if have == want:
        return []
    d = list(difflib.unified_diff(have.splitlines(True), want.splitlines(True),
                                  fromfile=f"a/{rel}", tofile=f"b/{rel}"))
    capped = d[:80] + ([f"    ... {len(d) - 80} more diff lines\n"]
                       if len(d) > 80 else [])
    return [f"STALE {rel} — committed content differs from a fresh emit:\n"
            + "".join("        " + ln for ln in capped)]
