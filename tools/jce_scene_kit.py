#!/usr/bin/env python3
"""
jce_scene_kit.py — measure JCE 3D assets, inspect scene data, and lint scene
files so scenes are designed against real asset specs and the real component
schema (every component + every field) instead of guesses.

It auto-extracts the component schema from the engine source
(engine/src/middleware/scene/jce_scene_components_json.c) — the parse_* readers
(j_num/j_bool/j_str/j_num2) and ser_* writers (cJSON_Add*ToObject) — so the tool
never drifts from the engine. That lets `check` flag unknown component types and
unknown/misspelled fields (e.g. a BoxCollider uses centerX/sizeX but a
CapsuleCollider uses cx/cy/cz — easy to get wrong by hand), and lets `dump`
print every component's data for debugging.

Commands:
    measure <file_or_dir>...   [--target-height M]      asset size/pivot/pack/scale
    schema  [--type NAME] [--engine FILE]                all components + fields
    dump    <scene.json> [--type T] [--name SUBSTR]      entity tree + all field data
    check   <scene.json> [--assets-root DIR] [--engine FILE]   lint (errors=non-zero)

`check` validates: JSON well-formed; unique ids; valid parents; one primary
Camera; a CharacterController (playable); every MeshRenderer has an albedoTex/
materialPath (else the editor draws a pink/black "missing texture" checker in
SHADED/TEXTURED view — use textures/white.png to just show baseColor); referenced
mesh/texture files exist; models seated on the ground (correct posY for pivot)
and inside the ground footprint; and every component type + field is recognised
by the engine schema.

INERT FIELDS.  `schema` marks, and `check` warns about, fields that serialise
and appear in the Inspector but that no engine code reads — the repository's own
standing list, tools/lint/component_field_baseline.json.  Without this an
agent authoring a scene sees ~700 field names with no sign that some of them do
nothing, sets one, and gets a scene that loads, saves and renders exactly as if
the field had never been written.  That failure has no symptom, which is why the
knowledge has to reach the authoring step rather than only the Inspector.

Bounds need `trimesh` (pip install trimesh); everything else runs without it.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# TRIMESH LOADS ON FIRST USE, NOT AT IMPORT.
#
# MEASURED 2026-09-20 in this process: a module-level `import trimesh` takes
# Windows COMMIT CHARGE from 12 MB to 1554 MB -- numpy +779 MB, scipy +760 MB
# (trimesh/grouping.py pulls scipy.spatial.cKDTree) -- while the working set
# stays near 110 MB.  Almost none of it is ever touched; it is reserved.
#
# Every consumer of this file paid that, including the ones that never measure
# a mesh: the automation suite imports this kit for project.* and so committed
# 1.6 GB per run.  Six concurrent runs left the machine with 0.8 GB of commit
# charge out of 39.6 GB -- with 12.9 GB of PHYSICAL memory still free -- and
# surfaced as MemoryError, 0xC0000142, LNK1102 and
# `cl : Command line error D8027 : cannot execute c2.dll` across two sessions.
# Not one of those names memory as the problem, which is what made it cost a
# day.
#
# The failure is cached as well as the success: a missing trimesh must not be
# re-attempted once per mesh.
_trimesh_mod = None
_trimesh_tried = False


def _trimesh():
    """The trimesh module, or None if it is not installed."""
    global _trimesh_mod, _trimesh_tried
    if not _trimesh_tried:
        _trimesh_tried = True
        try:
            import warnings
            import logging
            warnings.filterwarnings("ignore")
            logging.getLogger("trimesh").setLevel(logging.ERROR)
            import trimesh
            _trimesh_mod = trimesh
        except Exception:
            _trimesh_mod = None
    return _trimesh_mod


def have_trimesh() -> bool:
    """Whether mesh measurement is available -- WITHOUT paying for it.

    find_spec locates the package without executing it, so a status line can
    answer this for the price of a directory listing.  Answering it with a
    real import would reintroduce the whole cost above for the sake of
    printing one word.
    """
    if _trimesh_tried:
        return _trimesh_mod is not None
    import importlib.util
    try:
        return importlib.util.find_spec("trimesh") is not None
    except Exception:
        return False

MESH_EXTS = (".glb", ".gltf", ".obj", ".fbx", ".dae")
_bounds_cache: dict[str, object] = {}


def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def _default_engine_file() -> Path:
    return (Path(__file__).resolve().parent.parent /
            "engine" / "src" / "middleware" / "scene" / "jce_scene_components_json.c")


# ──────────────────────────────────────────────────────────────────────
# Component schema extraction (from the engine C source)
# ──────────────────────────────────────────────────────────────────────

# field reads inside a parse_* body:  j_num(c, "fov", 60.0) / j_num2(c, "a","b", x)
_FIELD_RE = re.compile(r'\bj_(\w+)\(\s*([A-Za-z_]\w*)\s*,\s*"([^"]*)"(?:\s*,\s*"([^"]*)")?')
# function bodies (closing brace at column 0).  NOT anchored on `static`:
# the parse_/ser_ pair for most components lives in the sibling
# jce_scene_components_*.c files and is declared in the internal header, so
# requiring `static` skipped every one of them.
_FN_RE = re.compile(r'^\w[\w\s\*]*?\b(parse_\w+|ser_\w+)\s*\([^)]*\)\s*\{(.*?)\n\}',
                    re.S | re.M)
_SER_FIELD_RE = re.compile(r'cJSON_Add(\w+?)ToObject\(\s*(\w+)\s*,\s*"([^"]+)"')


# A cJSON that is a ROW OF AN ARRAY rather than the component's own object.
# Keys read off one of these belong to the nested shape: Script's `params`
# rows carry name / kind / number / entity / text, and before this they were
# advertised as top-level Script fields that the loader discards without a
# word.
#
# TWO NARROW RULES, because the obvious wide one ("anything created locally")
# is wrong: a ser_ body builds the COMPONENT'S OWN object locally as well, and
# excluding that dropped 33 real fields across 21 components -- measured, not
# reasoned about.
#
#   parse side   the variable bound by cJSON_ArrayForEach IS a row.
#   ser side     an object added to an array that was ALSO built locally is a
#                row.  The component's own object is added to the array the
#                CALLER passed in, which is a parameter and so never appears
#                as a local CreateArray.
_ROW_FOREACH_RE = re.compile(r"cJSON_ArrayForEach\(\s*(\w+)\s*,")
_LOCAL_ARRAY_RE = re.compile(r"cJSON\s*\*\s*(\w+)\s*=\s*cJSON_CreateArray")
_ADD_TO_ARRAY_RE = re.compile(
    r"cJSON_AddItemToArray\(\s*(\w+)\s*,\s*(\w+)\s*\)")


def _nested_objects(body: str) -> set:
    out = set(_ROW_FOREACH_RE.findall(body))
    local_arrays = set(_LOCAL_ARRAY_RE.findall(body))
    for arr, item in _ADD_TO_ARRAY_RE.findall(body):
        if arr in local_arrays:
            out.add(item)
    return out

# INDEXED FIELDS.  Several components build their key names in a loop --
#
#     snprintf(key, sizeof(key), "distance%d", i);
#     lg.distances[i] = (float)j_num(c, key, 0.0);
#
# -- so both the reader and the writer name the field through a VARIABLE, and
# the two regexes above, which require a string literal, see neither.
#
# MEASURED: LODGroup.distance0..7 / meshPath0..7 and Terrain.layerMaskPath0..3,
# layerNormalPath0..3, layerNormalScale0..3 -- 52 real fields that `check`
# reported as "unknown field (engine ignores it)" when the engine reads every
# one of them.  The message was not merely unhelpful, it was backwards.  Found
# by comparing this table against the engine's own answer
# (tools/lint/check_introspection_parity.py).
#
# The bound is NOT resolved: it is a macro (JCE_LOD_COMP_MAX_LEVELS) and
# chasing it through headers would add a second, fragile, dependency for no
# gain.  The field is recorded as the PREFIX plus '#', and a consumer matches
# it against any digit suffix.  The engine clamps the index itself, so a
# too-large one is already handled where it should be.
_INDEXED_RE = re.compile(r'snprintf\(\s*(\w+)\s*,[^;]*?"(\w+)%d"')
INDEX_MARK = "#"

# A THIRD SPELLING: a table of literal key names, indexed by the loop.
#
#     const char *mask_keys[4] = { "layerMaskPath0", ..., "layerMaskPath3" };
#     ...  j_str(c, mask_keys[i], "")
#
# The names ARE literals, but they are in the initialiser rather than in the
# accessor call, so the two regexes above -- which look for the literal INSIDE
# the call -- see none of them.  Measured: Terrain.layerMaskPath0..3,
# layerNormalPath0..3 and layerNormalScale0..3, twelve fields the engine reads
# and writes and `check` called unknown.
_KEYTABLE_RE = re.compile(
    r'const\s+char\s*\*\s*(\w+)\s*\[\s*\d*\s*\]\s*=\s*\{([^}]*)\}')
_STRLIT_RE = re.compile(r'"([^"]+)"')


def _kind_from_helper(name: str) -> str:
    if name.startswith("num") or name.startswith("int") or name.startswith("float"):
        return "number"
    if name.startswith("bool"):
        return "bool"
    if name.startswith("str"):
        return "string"
    return "other"


def _kind_from_add(name: str) -> str:
    return {"Number": "number", "Bool": "bool", "String": "string"}.get(name, "other")


def _reg_rows(text: str):
    """Yield (canonical, [aliases], parse_fn, ser_fn) from the REG() table.

    THE REGISTRY IS THE AUTHORITY on which component types exist and what
    they are called.  The previous extractor inferred both from a
    `strcmp(type, "...")` dispatcher that the engine replaced with this table,
    and from `ser_` bodies for the canonical name -- so it recognised SIX real
    component types plus nine things that are not components at all
    (disabled_components, entity_cb, one_component, vfx_graph_migrate ...).
    `check` then reported 41 unknown-component errors on caged_kingdom's own
    showcase scene: 13 of its 16 types, every one of them correct.

    REG(NAME, A0, A1, A2, FLAG, HAS, REMOVE, PARSE, SER, GET, SET, SIZE)
    """
    for m in re.finditer(r"\bREG\s*\(", text):
        i, depth, args, start = m.end(), 1, [], m.end()
        while i < len(text) and depth:
            ch = text[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    args.append(text[start:i])
                    break
            elif ch == "," and depth == 1:
                args.append(text[start:i])
                start = i + 1
            i += 1
        args = [x.strip() for x in args]
        if len(args) < 9 or not args[0].startswith(chr(34)):
            continue
        lit = lambda x: x.strip(chr(34)) if x.startswith(chr(34)) else None
        canon = lit(args[0])
        aliases = [x for x in (lit(args[1]), lit(args[2]), lit(args[3])) if x]
        parse_fn = args[7] if args[7] != "NULL" else None
        ser_fn = args[8] if args[8] != "NULL" else None
        yield canon, aliases, parse_fn, ser_fn


def load_component_schema(engine_file: Path):
    """Return (registry, alias_map).

    registry[Canonical] = {"aliases": set[str], "fields": {field: kind}}
    alias_map[alias] = Canonical
    """
    reg_text = engine_file.read_text(encoding="utf-8", errors="ignore")

    # Bodies come from EVERY jce_scene_components_*.c, not just the one
    # holding the registry -- animation, physics, render, audio, net,
    # terrain, ui and world each carry their own parse_/ser_ pair.
    bodies: dict = {}
    for src in sorted(engine_file.parent.glob("jce_scene_components_*.c")):
        txt = src.read_text(encoding="utf-8", errors="ignore")
        for fm in _FN_RE.finditer(txt):
            bodies[fm.group(1)] = fm.group(2)
    if len(bodies) < 40:
        raise SystemExit(
            "jce_scene_kit: only %d parse_/ser_ bodies found across %s -- the "
            "source shape changed and the schema would silently cover almost "
            "nothing" % (len(bodies), engine_file.parent))

    def fields_of(fn):
        """Fields a hook reads or writes.  serw_X wraps ser_X, which is where
        the cJSON_Add* calls actually are."""
        out: dict = {}
        if not fn:
            return out
        names = [fn]
        if fn.startswith("serw_"):
            names.append("ser_" + fn[len("serw_"):])
        # Follow ONE level of shared sub-parsers: parse_ui_image and friends
        # delegate their anchor/pivot keys to parse_rect_transform, so without
        # this every UI component reports eight real fields as unknown.
        seen = set()
        i = 0
        while i < len(names):
            n = names[i]; i += 1
            if n in seen:
                continue
            seen.add(n)
            b = bodies.get(n)
            if b:
                for cm in re.finditer(r"\b((?:parse|ser)_\w+)\s*\(", b):
                    if cm.group(1) != n:
                        names.append(cm.group(1))
        for n in names:
            body = bodies.get(n)
            if not body:
                continue
            nested = _nested_objects(body)
            for fm in _FIELD_RE.finditer(body):
                helper, obj, f1, f2 = (fm.group(1), fm.group(2),
                                       fm.group(3), fm.group(4))
                if obj in nested:
                    continue          # a row of an array, not the component
                k = _kind_from_helper(helper)
                if f1:
                    out[f1] = k
                if f2 and helper.endswith("2"):
                    out[f2] = k
            for sm in _SER_FIELD_RE.finditer(body):
                add, obj, field = sm.group(1), sm.group(2), sm.group(3)
                if obj in nested:
                    continue
                if field == "type":
                    continue
                out[field] = _kind_from_add(add)
            # Keys built in a loop (see _INDEXED_RE).  The kind comes from the
            # accessor that USES the variable, looked for just after the
            # snprintf -- `j_num(c, key, 0.0)` or `cJSON_AddStringToObject(o,
            # key, ...)`.  Not from the prefix: "meshPath0" is a string and
            # "distance0" is a number, and guessing from the name would be a
            # second, worse, source of the same answer.
            for im in _INDEXED_RE.finditer(body):
                var, prefix = im.group(1), im.group(2)
                tail = body[im.end():im.end() + 400]
                kind = None
                jm = re.search(r"\bj_(\w+)\(\s*\w+\s*,\s*%s\b" % re.escape(var),
                               tail)
                if jm:
                    kind = _kind_from_helper(jm.group(1))
                else:
                    cm2 = re.search(
                        r"cJSON_Add(\w+?)ToObject\(\s*\w+\s*,\s*%s\b"
                        % re.escape(var), tail)
                    if cm2:
                        kind = _kind_from_add(cm2.group(1))
                if kind:
                    out[prefix + INDEX_MARK] = kind
            # Key tables (see _KEYTABLE_RE).  The kind comes from the accessor
            # that indexes the table, for the same reason as above: the names
            # say nothing about their types.
            for km in _KEYTABLE_RE.finditer(body):
                var, init = km.group(1), km.group(2)
                keys = _STRLIT_RE.findall(init)
                if not keys:
                    continue
                kind = None
                jm = re.search(r"\bj_(\w+)\(\s*\w+\s*,\s*%s\s*\["
                               % re.escape(var), body)
                if jm:
                    kind = _kind_from_helper(jm.group(1))
                else:
                    cm2 = re.search(
                        r"cJSON_Add(\w+?)ToObject\(\s*\w+\s*,\s*%s\s*\["
                        % re.escape(var), body)
                    if cm2:
                        kind = _kind_from_add(cm2.group(1))
                if kind:
                    for k in keys:
                        out[k] = kind
        return out

    registry: dict = {}
    for canon, aliases, parse_fn, ser_fn in _reg_rows(reg_text):
        f = fields_of(parse_fn)
        f.update(fields_of(ser_fn))
        reg = registry.setdefault(canon, {"aliases": set(), "fields": {}})
        reg["aliases"].add(canon)
        reg["aliases"].update(aliases)
        reg["fields"].update(f)

    if len(registry) < 40:
        raise SystemExit(
            "jce_scene_kit: the REG() table yielded only %d component "
            "type(s); the macro shape changed and `check` would report every "
            "real component as unknown" % len(registry))

    alias_map: dict = {}
    for canon, reg in registry.items():
        for a in reg["aliases"]:
            alias_map[a] = canon
    return registry, alias_map

# ──────────────────────────────────────────────────────────────────────
# Asset measurement
# ──────────────────────────────────────────────────────────────────────


def field_kind(known: dict, field: str):
    """The kind of `field` on a component, or None when it has no such field.

    The one place that knows how an indexed field is recorded, so no consumer
    has to re-derive it -- `check`, the Automation API's write validation and
    the schema-parity gate all ask here.  A second implementation of this
    matching rule is how one of them would start rejecting `distance0` again
    while the others accepted it.
    """
    if field in known:
        return known[field]
    m = re.match(r"^(.*?)(\d+)$", field)
    if m:
        return known.get(m.group(1) + INDEX_MARK)
    return None


def expand_indexed(known: dict, concrete) -> set:
    """The field names in `known`, with each PREFIX# replaced by whichever of
    `concrete` it matches.  For comparing this table against a source that
    enumerates the real names (the engine's own answer)."""
    out = set()
    for f in known:
        if not f.endswith(INDEX_MARK):
            out.add(f)
            continue
        prefix = f[:-len(INDEX_MARK)]
        hit = {c for c in concrete
               if c.startswith(prefix) and c[len(prefix):].isdigit()}
        out |= (hit or {f})
    return out


def load_bounds(path: Path):
    key = str(path)
    if key in _bounds_cache:
        return _bounds_cache[key]
    tm = _trimesh()
    if tm is None:
        return None
    try:
        scene = tm.load(str(path), force="scene")
        b = scene.bounds
        res = ((float(b[0][0]), float(b[0][1]), float(b[0][2])),
               (float(b[1][0]), float(b[1][1]), float(b[1][2])),
               len(scene.geometry))
    except Exception as e:
        res = ("ERR", str(e))
    _bounds_cache[key] = res
    return res


def _iter_mesh_files(targets):
    for t in targets:
        p = Path(t)
        if p.is_dir():
            for ext in MESH_EXTS:
                yield from sorted(p.rglob("*" + ext))
        elif p.is_file():
            yield p


def cmd_measure(args) -> int:
    if _trimesh() is None:
        print("error: trimesh not installed (pip install trimesh)", file=sys.stderr)
        return 2
    files = list(_iter_mesh_files(args.targets))
    if not files:
        print("no mesh files found", file=sys.stderr)
        return 1
    th = args.target_height
    print(f"{'file':30} {'objs':>4} {'W':>8} {'H':>8} {'D':>8} {'pivotY':>7} "
          f"{'pack?':>5} {'scale@' + str(th) + 'm':>10}")
    print("-" * 92)
    for f in files:
        b = load_bounds(f)
        if not b or (isinstance(b, tuple) and b and b[0] == "ERR"):
            print(f"{f.name:30} ERROR {b[1] if b else ''}")
            continue
        (mn, mx, ngeo) = b
        w, h, d = mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]
        is_pack = ngeo > 5 or max(w, h, d) > 10.0
        scale = (th / h) if h > 1e-6 else 1.0
        print(f"{f.name:30} {ngeo:4d} {w:8.2f} {h:8.2f} {d:8.2f} {mn[1]:7.2f} "
              f"{'YES' if is_pack else '  -':>5} {scale:10.4f}")
    return 0


# ──────────────────────────────────────────────────────────────────────
# Schema dump
# ──────────────────────────────────────────────────────────────────────


# ──────────────────────────────────────────────────────────────────────
# Inert fields: authored, serialised, editable, and read by nothing
# ──────────────────────────────────────────────────────────────────────

_ACC_RE = re.compile(r"REG_ACCESSORS(?:_\w+)?\(\s*(\w+)\s*,\s*(\w+)\s*\)")
# Like _FN_RE but it also captures the serw_ spelling, which _FN_RE does not:
# `ser_\w+` cannot match "serw_avatar".  The schema extractor must NOT use this
# one.  Widening it there was measured: it loses nothing and gains seven keys,
# and all seven are CONTAINERS -- "properties" and "layers", the sub-objects a
# serw_ wrapper builds -- so `check` would start accepting `"properties": 3` as
# a real field.  Here only the ser side is wanted, and the containers are
# harmless because the join asks for a specific C field.
_FN_ANY_RE = re.compile(
    r"^\w[\w\s\*]*?\b(parse_\w+|serw?_\w+)\s*\([^)]*\)\s*\{(.*?)\n\}",
    re.S | re.M)
_SER_KV_RE = re.compile(
    r"cJSON_Add\w+ToObject\s*\(\s*\w+\s*,\s*\"([^\"]+)\"\s*,([^;]*);")


def _wrap_reason(text, width=72):
    """Reasons are paragraphs, and a paragraph on one terminal line is a
    paragraph nobody reads."""
    words, line, out = text.split(), "", []
    for w in words:
        if line and len(line) + 1 + len(w) > width:
            out.append(line); line = w
        else:
            line = (line + " " + w) if line else w
    if line:
        out.append(line)
    return out


def load_inert_fields(engine_file: Path):
    """{canonical: {json_key: c_field}} for fields the ENGINE never reads.

    WHY THIS IS HERE.  An agent authoring a scene through this kit sees ~700
    field names and no sign that some of them do nothing.  The repository
    already knows which ones -- tools/lint/component_field_baseline.json is
    the gate's standing record of "authored, serialised, editable, read by
    nothing" -- and until now that knowledge reached the Inspector (as an
    unwired badge) and nowhere else.  A tool that lists a dead knob beside a
    live one is the same defect as an Inspector control that does nothing.

    THE JOIN IS TAKEN FROM SOURCE, NOT GUESSED.  The baseline speaks C
    (JceReflectionProbeComponent.custom_hdr_path); this kit speaks JSON
    (ReflectionProbe / hdrPath).  Normalising names bridges 24 of 30 and
    silently drops the rest -- NetworkAnimator is JceNetAnimatorComponent,
    hdrPath is custom_hdr_path, and the VirtualCamera key is plain `name`.
    So both halves come from the code that already pairs them:

      REG_ACCESSORS(short, JceFooComponent)  +  REG("Json", ..., reg_get_short)
          -> struct  <->  canonical JSON name
      cJSON_Add*ToObject(o, "key", c->c_field)
          -> JSON key <->  C field

    Entries that do not join are REPORTED, never swallowed: a field with no
    JSON key at all cannot be authored, which makes it a question about the
    baseline rather than about this tool.
    """
    base = _repo_root() / "tools/lint/component_field_baseline.json"
    if not base.is_file():
        return {}, []
    try:
        rows = json.loads(base.read_text(encoding="utf-8")).get("fields", [])
    except Exception:
        return {}, []

    src_dir = engine_file.parent
    texts = {}
    for p in sorted(src_dir.glob("jce_scene_components*.c")):
        texts[p.name] = p.read_text(encoding="utf-8", errors="ignore")
    reg_text = texts.get(engine_file.name, "")

    acc = {m.group(1): m.group(2) for m in _ACC_RE.finditer(reg_text)}
    # _reg_rows() drops the accessor columns, so the raw args are walked here.
    struct_json, struct_ser = {}, {}
    for m in re.finditer(r"\bREG\s*\(", reg_text):
        i, depth, args, start = m.end(), 1, [], m.end()
        while i < len(reg_text) and depth:
            ch = reg_text[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    args.append(reg_text[start:i])
                    break
            elif ch == "," and depth == 1:
                args.append(reg_text[start:i])
                start = i + 1
            i += 1
        args = [a.strip() for a in args]
        if len(args) < 11 or not args[0].startswith(chr(34)):
            continue
        g = re.match(r"reg_get_(\w+)", args[9] or "")
        st = acc.get(g.group(1)) if g else None
        if st:
            struct_json[st] = args[0].strip(chr(34))
            struct_ser[st] = args[8]

    bodies = {}
    for txt in texts.values():
        for fm in _FN_ANY_RE.finditer(txt):
            bodies[fm.group(1)] = fm.group(2)

    def key_of(ser_fn, c_field):
        seen, stack = set(), [ser_fn]
        while stack:
            n = stack.pop()
            if not n or n == "NULL" or n in seen:
                continue
            seen.add(n)
            if n.startswith("serw_"):
                stack.append("ser_" + n[len("serw_"):])
            b = bodies.get(n)
            if not b:
                continue
            for c in re.finditer(r"\b(ser_\w+)\s*\(", b):
                stack.append(c.group(1))
            for km in _SER_KV_RE.finditer(b):
                ids = re.findall(r"[A-Za-z_]\w*", km.group(2))
                if ids and ids[-1] == c_field:
                    return km.group(1)
        return None

    reasons = {}
    try:
        reasons = json.loads(base.read_text(encoding="utf-8")).get("reasons") or {}
    except Exception:
        reasons = {}

    out, unmapped = {}, []
    for entry in rows:
        st, _, c_field = entry.partition(".")
        canon = struct_json.get(st)
        key = key_of(struct_ser.get(st), c_field) if canon else None
        if canon and key:
            # (C field, WHY it is inert).  The why is the half that tells an
            # author whether to wait for a wire or design around a missing
            # subsystem, and check_component_field_consumed.py fails when a
            # baselined field has none -- so it is always there to print.
            out.setdefault(canon, {})[key] = (c_field, reasons.get(entry, ""))
        else:
            unmapped.append(entry)
    return out, unmapped


def cmd_schema(args) -> int:
    engine = Path(args.engine) if args.engine else _default_engine_file()
    if not engine.is_file():
        print(f"error: engine source not found: {engine}", file=sys.stderr)
        return 2
    registry, _ = load_component_schema(engine)
    inert, unmapped = load_inert_fields(engine)
    items = sorted(registry.items())
    if args.type:
        items = [(k, v) for k, v in items if k.lower() == args.type.lower()
                 or args.type in v["aliases"]]
        if not items:
            print(f"unknown component type: {args.type}", file=sys.stderr)
            return 1
    n_inert = sum(len(v) for v in inert.values())
    print(f"{len(items)} component type(s) — source: {engine.name}")
    print(f"{n_inert} field(s) marked INERT from "
          f"tools/lint/component_field_baseline.json\n")
    for canon, reg in items:
        aliases = sorted(a for a in reg["aliases"] if a != canon)
        alias_s = f"  (aliases: {', '.join(aliases)})" if aliases else ""
        print(f"● {canon}{alias_s}")
        dead = inert.get(canon, {})
        for f, k in sorted(reg["fields"].items()):
            mark = "   INERT — the engine reads nothing here" if f in dead else ""
            print(f"    {f:22} {k}{mark}")
            if f in dead and dead[f][1]:
                for line in _wrap_reason(dead[f][1]):
                    print(f"        {line}")
        if not reg["fields"]:
            print("    (no scalar fields)")
        print()
    if unmapped and not args.type:
        # Never swallowed: a baselined field this tool cannot place is a
        # question about the baseline, not a reason to print a shorter list.
        print(f"{len(unmapped)} baselined field(s) could not be joined to a "
              f"JSON key (no serialiser writes them, so they are not "
              f"authorable at all):")
        for u in unmapped:
            print(f"    {u}")
        print()
    return 0


# ──────────────────────────────────────────────────────────────────────
# Scene helpers + dump
# ──────────────────────────────────────────────────────────────────────


def _entities(doc):
    # supports both the simple authoring format ({entities:[...]}) and the
    # editor's normalised save format ({scene:{entities:[...]}}).
    raw = doc.get("entities") or doc.get("scene", {}).get("entities", [])
    out = []
    for e in raw:
        if not isinstance(e, dict) or "id" not in e:
            continue
        if "parent_id" not in e and "parentId" in e:   # editor uses parentId
            e = {**e, "parent_id": e["parentId"]}
        out.append(e)
    return out


def cmd_shot(args) -> int:
    """Print the newest editor screenshot(s) so they can be read/inspected.

    F12 in the editor saves to <home>/.jce/screenshots/jce_screenshot_<ts>.png
    (falls back to <cwd>/.jce/screenshots). This locates the most recent ones."""
    # home + cwd is exactly what the docstring above promises.  A third entry
    # naming one game used to sit here, documented nowhere.
    cands = ([Path(args.dir)] if args.dir else
             [Path.home() / ".jce" / "screenshots",
              Path.cwd() / ".jce" / "screenshots"])
    files = []
    for d in cands:
        if d.is_dir():
            files += list(d.glob("*.png"))
    if not files:
        print("no screenshots found in:\n  " + "\n  ".join(str(c) for c in cands),
              file=sys.stderr)
        print("\nPress F12 in the editor to capture one.", file=sys.stderr)
        return 1
    files.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    for p in files[:args.n]:
        import datetime
        ts = datetime.datetime.fromtimestamp(p.stat().st_mtime).strftime("%Y-%m-%d %H:%M:%S")
        print(f"{ts}  {p.stat().st_size:>9}  {p}")
    return 0


def _transform(ent):
    for c in ent.get("components", []):
        if c.get("type") == "Transform":
            return c
    return None


def _fmt_val(v):
    if isinstance(v, float):
        return f"{v:g}"
    return str(v)


def cmd_dump(args) -> int:
    scene_path = Path(args.scene)
    if not scene_path.is_file():
        print(f"error: scene not found: {scene_path}", file=sys.stderr)
        return 2
    doc = json.loads(scene_path.read_text(encoding="utf-8"))
    ents = _entities(doc)
    by_parent: dict[int, list] = {}
    for e in ents:
        by_parent.setdefault(e.get("parent_id", 0), []).append(e)

    print(f"scene: {scene_path}  ({len(ents)} entities)")
    type_filter = args.type
    name_filter = args.name

    def show(ent, depth):
        comps = ent.get("components", [])
        if type_filter:
            comps = [c for c in comps if c.get("type") == type_filter]
        if name_filter and name_filter.lower() not in str(ent.get("name", "")).lower():
            pass  # still recurse, but skip printing this node's body
        pad = "  " * depth
        print(f"{pad}[{ent.get('id')}] {ent.get('name', '?')}"
              f"{'' if ent.get('parent_id', 0) == 0 else '  (parent ' + str(ent['parent_id']) + ')'}")
        for c in comps:
            t = c.get("type", "?")
            fields = {k: v for k, v in c.items() if k != "type"}
            inline = " ".join(f"{k}={_fmt_val(v)}" for k, v in fields.items())
            print(f"{pad}    · {t}: {inline}")

    def walk(pid, depth):
        for ent in by_parent.get(pid, []):
            if not name_filter or name_filter.lower() in str(ent.get("name", "")).lower() \
                    or by_parent.get(ent["id"]):
                show(ent, depth)
            walk(ent["id"], depth + 1)

    walk(0, 0)
    return 0


# ──────────────────────────────────────────────────────────────────────
# Scene linting
# ──────────────────────────────────────────────────────────────────────


def cmd_frames(args) -> int:
    """Extract still frames from the newest F9 recording (.mkv/.webm) via ffmpeg,
    so a recorded play session becomes PNGs that can be read/inspected."""
    import shutil
    import subprocess
    import tempfile
    if shutil.which("ffmpeg") is None:
        print("error: ffmpeg not on PATH", file=sys.stderr)
        return 2
    if args.rec and Path(args.rec).is_file():
        src = Path(args.rec)
    else:
        dirs = [Path.home() / ".jce" / "recordings",
                Path.cwd() / ".jce" / "recordings"]
        recs = []
        for d in dirs:
            if d.is_dir():
                recs += list(d.glob("*.mkv")) + list(d.glob("*.webm"))
        if not recs:
            print("no recordings found (press F9 in the editor to record)", file=sys.stderr)
            return 1
        recs.sort(key=lambda p: p.stat().st_mtime, reverse=True)
        src = recs[0]
    outdir = Path(args.out) if args.out else Path(tempfile.gettempdir()) / ("jce_frames_" + src.stem)
    outdir.mkdir(parents=True, exist_ok=True)
    for old in outdir.glob("frame_*.png"):
        old.unlink()
    fps = (1.0 / args.every) if args.every > 0 else 1.0
    r = subprocess.run(["ffmpeg", "-y", "-i", str(src), "-vf", f"fps={fps}",
                        str(outdir / "frame_%03d.png")], capture_output=True, text=True)
    frames = sorted(outdir.glob("frame_*.png"))
    if not frames:
        print(f"ffmpeg produced no frames\n{r.stderr[-400:]}", file=sys.stderr)
        return 1
    print(f"{src} -> {len(frames)} frame(s) (1 every {args.every}s):")
    for f in frames:
        print(f"  {f}")
    return 0


def cmd_check(args) -> int:
    scene_path = Path(args.scene)
    if not scene_path.is_file():
        print(f"error: scene not found: {scene_path}", file=sys.stderr)
        return 2
    try:
        doc = json.loads(scene_path.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"error: invalid JSON: {e}", file=sys.stderr)
        return 2

    root = Path(args.assets_root) if args.assets_root else (
        scene_path.parent.parent if scene_path.parent.name == "scenes"
        else scene_path.parent)

    engine = Path(args.engine) if args.engine else _default_engine_file()
    registry, alias_map = (load_component_schema(engine)
                           if engine.is_file() else ({}, {}))
    inert, _unmapped = load_inert_fields(engine) if engine.is_file() else ({}, [])

    ents = _entities(doc)
    errors: list[str] = []
    warns: list[str] = []

    # structural
    ids = [e["id"] for e in ents]
    if len(set(ids)) != len(ids):
        errors.append("duplicate entity ids")
    idset = set(ids) | {0}
    for e in ents:
        if e.get("parent_id", 0) not in idset:
            errors.append(f"'{e.get('name')}' has unknown parent_id {e.get('parent_id')}")
    cams = [e for e in ents for c in e.get("components", [])
            if c.get("type") == "Camera" and c.get("primary")]
    ccs = [e for e in ents for c in e.get("components", [])
           if c.get("type") == "CharacterController"]
    if len(cams) == 0:
        errors.append("no primary Camera (scene has no view)")
    elif len(cams) > 1:
        warns.append(f"{len(cams)} primary cameras (expected 1)")
    if len(ccs) == 0:
        warns.append("no CharacterController (not first-person playable)")

    # schema validation: every component type + field recognised
    if registry:
        for e in ents:
            for c in e.get("components", []):
                t = c.get("type")
                canon = alias_map.get(t)
                if canon is None:
                    errors.append(f"'{e.get('name')}': unknown component type '{t}'")
                    continue
                known = registry[canon]["fields"]
                dead = inert.get(canon, {})
                for f in c:
                    if f == "type":
                        continue
                    # field_kind(), not `f in known`: a field whose key the
                    # engine builds in a loop is recorded as PREFIX# and a
                    # plain membership test rejects every real one of them.
                    # Measured: LODGroup.distance0 and 51 others were reported
                    # as "engine ignores it" while the engine read all of them.
                    if field_kind(known, f) is None:
                        warns.append(f"'{e.get('name')}'/{t}: unknown field '{f}' "
                                     f"(engine ignores it — typo? wrong component?)")
                    elif f in dead:
                        # A WARNING, not an error: the value round-trips and the
                        # Inspector shows it, so the scene is not malformed --
                        # it just will not do what the author expects.  This is
                        # the same fact the Inspector's unwired badge carries,
                        # said where a scene is actually authored.
                        c_field, why = dead[f]
                        warns.append(
                            f"'{e.get('name')}'/{t}: '{f}' is INERT — it "
                            f"round-trips and the Inspector shows it, but no "
                            f"engine code reads {c_field}, so setting it "
                            f"changes nothing at runtime."
                            + (f"  {why}" if why else ""))

    # ground extent for footprint containment
    ground_half = None
    for e in ents:
        if str(e.get("name", "")).lower().startswith("ground"):
            t = _transform(e)
            if t:
                ground_half = (abs(t.get("scaleX", 1)) / 2.0, abs(t.get("scaleZ", 1)) / 2.0)

    # renderer / placement
    for e in ents:
        name = e.get("name", "?")
        t = _transform(e)
        for c in e.get("components", []):
            if c.get("type") != "MeshRenderer":
                continue
            mesh = c.get("meshPath", "")
            albedo = c.get("albedoTex", "")
            material = c.get("materialPath", "")
            if albedo or material:
                for rel in (albedo, material):
                    if rel and not (root / rel).is_file():
                        errors.append(f"'{name}': '{rel}' not found under {root}")
            else:
                warns.append(f"'{name}': no albedoTex/materialPath → pink-black checker "
                             f"in editor SHADED/TEXTURED (use textures/white.png)")
            if not mesh:
                continue
            if not (root / mesh).is_file():
                errors.append(f"'{name}': meshPath '{mesh}' not found under {root}")
                continue
            b = load_bounds(root / mesh)
            if not b or (isinstance(b, tuple) and b and b[0] == "ERR") or t is None:
                continue
            (mn, mx, _n) = b
            sx, sy, sz = t.get("scaleX", 1), t.get("scaleY", 1), t.get("scaleZ", 1)
            px, py, pz = t.get("posX", 0), t.get("posY", 0), t.get("posZ", 0)
            wmin_y = mn[1] * sy + py
            wmin_x, wmax_x = mn[0] * sx + px, mx[0] * sx + px
            wmin_z, wmax_z = mn[2] * sz + pz, mx[2] * sz + pz
            if wmin_y < -0.5:
                warns.append(f"'{name}': sunk {wmin_y:.2f}m (set posY≈{-mn[1] * sy:.2f})")
            elif wmin_y > 0.5:
                warns.append(f"'{name}': floating {wmin_y:.2f}m (set posY≈{-mn[1] * sy:.2f})")
            if ground_half:
                gx, gz = ground_half
                if wmin_x < -gx or wmax_x > gx or wmin_z < -gz or wmax_z > gz:
                    warns.append(f"'{name}': footprint x[{wmin_x:.0f},{wmax_x:.0f}] "
                                 f"z[{wmin_z:.0f},{wmax_z:.0f}] exceeds ground ±({gx:.0f},{gz:.0f})")

    print(f"scene: {scene_path}")
    print(f"assets root: {root}")
    print(f"entities: {len(ents)} | primary cameras: {len(cams)} | "
          f"character controllers: {len(ccs)} | "
          f"schema: {len(registry)} types | "
          f"trimesh: {'yes' if have_trimesh() else 'NO'}")
    if errors:
        print(f"\n{len(errors)} ERROR(S):")
        for x in errors:
            print(f"  x {x}")
    if warns:
        print(f"\n{len(warns)} WARNING(S):")
        for x in warns:
            print(f"  ! {x}")
    if not errors and not warns:
        print("\nOK all checks passed")
    return 1 if errors else 0


# ──────────────────────────────────────────────────────────────────────
# CLI
# ──────────────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser(description="Measure JCE assets, dump scene data, lint scenes.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("measure", help="report size/pivot/scale for mesh assets")
    m.add_argument("targets", nargs="+")
    m.add_argument("--target-height", type=float, default=1.8)
    m.set_defaults(func=cmd_measure)

    s = sub.add_parser("schema", help="print all component types + fields from the engine")
    s.add_argument("--type", default=None, help="only this component type")
    s.add_argument("--engine", default=None, help="path to jce_scene_components_json.c")
    s.set_defaults(func=cmd_schema)

    d = sub.add_parser("dump", help="print entity tree + all component field values")
    d.add_argument("scene")
    d.add_argument("--type", default=None, help="only components of this type")
    d.add_argument("--name", default=None, help="only entities whose name contains this")
    d.set_defaults(func=cmd_dump)

    c = sub.add_parser("check", help="lint a .scene.json (errors -> non-zero exit)")
    c.add_argument("scene")
    c.add_argument("--assets-root", default=None)
    c.add_argument("--engine", default=None, help="path to jce_scene_components_json.c")
    c.set_defaults(func=cmd_check)

    sh = sub.add_parser("shot", help="print newest editor F12 screenshot path(s) to read")
    sh.add_argument("--n", type=int, default=1, help="how many recent shots to list")
    sh.add_argument("--dir", default=None, help="override screenshots dir")
    sh.set_defaults(func=cmd_shot)

    fr = sub.add_parser("frames", help="extract PNG frames from newest F9 recording (ffmpeg)")
    fr.add_argument("--every", type=float, default=1.0, help="seconds between extracted frames")
    fr.add_argument("--rec", default=None, help="specific recording file")
    fr.add_argument("--out", default=None, help="output dir for frames")
    fr.set_defaults(func=cmd_frames)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
