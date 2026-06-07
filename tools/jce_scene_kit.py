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

Bounds need `trimesh` (pip install trimesh); everything else runs without it.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

try:
    import warnings
    import logging
    warnings.filterwarnings("ignore")
    logging.getLogger("trimesh").setLevel(logging.ERROR)
    import trimesh
    _HAVE_TRIMESH = True
except Exception:
    _HAVE_TRIMESH = False

MESH_EXTS = (".glb", ".gltf", ".obj", ".fbx", ".dae")
_bounds_cache: dict[str, object] = {}


def _default_engine_file() -> Path:
    return (Path(__file__).resolve().parent.parent /
            "engine" / "src" / "middleware" / "scene" / "jce_scene_components_json.c")


# ──────────────────────────────────────────────────────────────────────
# Component schema extraction (from the engine C source)
# ──────────────────────────────────────────────────────────────────────

# field reads inside a parse_* body:  j_num(c, "fov", 60.0) / j_num2(c, "a","b", x)
_FIELD_RE = re.compile(r'\bj_(\w+)\(\s*[A-Za-z_]\w*\s*,\s*"([^"]*)"(?:\s*,\s*"([^"]*)")?')
# function bodies (closing brace at column 0)
_FN_RE = re.compile(r'static\s+\w[\w\s\*]*?\b(parse_\w+|ser_\w+)\s*\([^)]*\)\s*\{(.*?)\n\}', re.S)
# serializer: canonical type name + each emitted field
_SER_TYPE_RE = re.compile(r'AddStringToObject\(\s*\w+\s*,\s*"type"\s*,\s*"([^"]+)"')
_SER_FIELD_RE = re.compile(r'cJSON_Add(\w+?)ToObject\(\s*\w+\s*,\s*"([^"]+)"')


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


def load_component_schema(engine_file: Path):
    """Return (registry, alias_map).

    registry[Canonical] = {"aliases": set[str], "fields": {field: kind}}
    alias_map[alias_lower_or_exact] = Canonical
    """
    text = engine_file.read_text(encoding="utf-8", errors="ignore")
    parse_fields: dict[str, dict] = {}
    ser_info: dict[str, tuple] = {}   # suffix -> (Canonical, {field:kind})

    for m in _FN_RE.finditer(text):
        fn, body = m.group(1), m.group(2)
        if fn.startswith("parse_"):
            suffix = fn[len("parse_"):]
            fields: dict[str, str] = {}
            for fm in _FIELD_RE.finditer(body):
                helper, f1, f2 = fm.group(1), fm.group(2), fm.group(3)
                k = _kind_from_helper(helper)
                if f1:
                    fields[f1] = k
                if f2 and helper.endswith("2"):   # j_num2 / j_str2 dual key
                    fields[f2] = k
            parse_fields[suffix] = fields
        else:  # ser_
            suffix = fn[len("ser_"):]
            tm = _SER_TYPE_RE.search(body)
            canonical = tm.group(1) if tm else None
            fields = {}
            for sm in _SER_FIELD_RE.finditer(body):
                add, field = sm.group(1), sm.group(2)
                if field == "type":
                    continue
                fields[field] = _kind_from_add(add)
            ser_info[suffix] = (canonical, fields)

    # dispatcher: alias strings -> parse_fn
    dispatch: dict[str, set] = {}
    pending: set = set()
    for line in text.splitlines():
        for lit in re.findall(r'strcmp\(\s*type\s*,\s*"([^"]+)"', line):
            pending.add(lit)
        mcall = re.search(r'\b(parse_\w+)\(\s*s\s*,\s*e\s*,\s*props\s*\)', line)
        if mcall and pending:
            dispatch.setdefault(mcall.group(1)[len("parse_"):], set()).update(pending)
            pending = set()

    registry: dict[str, dict] = {}
    for suffix in set(parse_fields) | set(ser_info):
        pf = parse_fields.get(suffix, {})
        canon, sf = ser_info.get(suffix, (None, {}))
        aliases = set(dispatch.get(suffix, set()))
        if not canon:
            canon = next((a for a in aliases if a[:1].isupper()),
                         (sorted(aliases)[0] if aliases else suffix))
        aliases.add(canon)
        fields = dict(pf)
        fields.update(sf)
        reg = registry.setdefault(canon, {"aliases": set(), "fields": {}})
        reg["aliases"].update(aliases)
        reg["fields"].update(fields)

    # Transform is mandatory per-entity and may bypass the component dispatcher.
    if "Transform" not in registry:
        registry["Transform"] = {"aliases": {"Transform"}, "fields": {
            k: "number" for k in ("posX", "posY", "posZ", "rotX", "rotY", "rotZ",
                                   "scaleX", "scaleY", "scaleZ")}}

    alias_map: dict[str, str] = {}
    for canon, reg in registry.items():
        for a in reg["aliases"]:
            alias_map[a] = canon
    return registry, alias_map


# ──────────────────────────────────────────────────────────────────────
# Asset measurement
# ──────────────────────────────────────────────────────────────────────


def load_bounds(path: Path):
    key = str(path)
    if key in _bounds_cache:
        return _bounds_cache[key]
    if not _HAVE_TRIMESH:
        return None
    try:
        scene = trimesh.load(str(path), force="scene")
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
    if not _HAVE_TRIMESH:
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


def cmd_schema(args) -> int:
    engine = Path(args.engine) if args.engine else _default_engine_file()
    if not engine.is_file():
        print(f"error: engine source not found: {engine}", file=sys.stderr)
        return 2
    registry, _ = load_component_schema(engine)
    items = sorted(registry.items())
    if args.type:
        items = [(k, v) for k, v in items if k.lower() == args.type.lower()
                 or args.type in v["aliases"]]
        if not items:
            print(f"unknown component type: {args.type}", file=sys.stderr)
            return 1
    print(f"{len(items)} component type(s) — source: {engine.name}\n")
    for canon, reg in items:
        aliases = sorted(a for a in reg["aliases"] if a != canon)
        alias_s = f"  (aliases: {', '.join(aliases)})" if aliases else ""
        print(f"● {canon}{alias_s}")
        for f, k in sorted(reg["fields"].items()):
            print(f"    {f:22} {k}")
        if not reg["fields"]:
            print("    (no scalar fields)")
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
    cands = ([Path(args.dir)] if args.dir else
             [Path.home() / ".jce" / "screenshots",
              Path.cwd() / ".jce" / "screenshots",
              Path("caged_kingdom") / ".jce" / "screenshots"])
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
                Path.cwd() / ".jce" / "recordings",
                Path("caged_kingdom") / ".jce" / "recordings"]
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
                for f in c:
                    if f == "type":
                        continue
                    if f not in known:
                        warns.append(f"'{e.get('name')}'/{t}: unknown field '{f}' "
                                     f"(engine ignores it — typo? wrong component?)")

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
          f"schema: {len(registry)} types | trimesh: {'yes' if _HAVE_TRIMESH else 'NO'}")
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
