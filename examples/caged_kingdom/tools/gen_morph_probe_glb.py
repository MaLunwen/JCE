#!/usr/bin/env python3
"""gen_morph_probe_glb.py -- author the blendshape measurement fixture.

WHY THIS EXISTS.  This tree had morph-target import, a CPU evaluator, an
inspector, a scene component and a renderer path for blendshapes, and NOT ONE
glTF asset anywhere in it that carries a morph target -- measured: zero of the
.glb files under editor/, examples/caged_kingdom/, tests/ and engine/ has a `targets`
array.  So the whole feature had never been RENDERED, only unit-tested against
an in-memory glTF string, and any claim about how it looks was unsupportable.

The fixture is deliberately crude and deliberately LOUD.  It is a vertical
quad with one morph target that shears its top edge 1.5 units along +X; at
weight 1 the silhouette moves by a large fraction of the frame, so a capture
where the deform did not run is not a subtle difference to argue over.  It is
SKINNED (one joint, identity bind, every vertex fully weighted to it) because
the renderer's morph resolve is gated on the entity having a SkeletalAnimator
-- a separate limitation, recorded on its own, not worked around here.

Regenerate with:
    python examples/caged_kingdom/tools/gen_morph_probe_glb.py

Output is byte-deterministic: no timestamps, no floats that depend on the host.
"""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "resources/assets/models/morph_probe.glb"

# ── geometry ───────────────────────────────────────────────────────────
# A quad in the XY plane facing +Z, 2 x 2 metres, sitting on y = 0.
POSITIONS = [
    (-1.0, 0.0, 0.0),
    ( 1.0, 0.0, 0.0),
    (-1.0, 2.0, 0.0),
    ( 1.0, 2.0, 0.0),
]
NORMALS = [(0.0, 0.0, 1.0)] * 4
INDICES = [0, 1, 2, 2, 1, 3]

# The morph target: the TOP edge only, sheared hard along +X, with the normal
# tilted to match so the lighting moves too (a deform that moved positions and
# not normals would otherwise look right in silhouette and wrong in shading,
# and this fixture has to be able to tell those apart).
DPOS = [
    (0.0, 0.0, 0.0),
    (0.0, 0.0, 0.0),
    (1.5, 0.0, 0.0),
    (1.5, 0.0, 0.0),
]
DNRM = [
    (0.0, 0.0, 0.0),
    (0.0, 0.0, 0.0),
    (-0.6, 0.0, 0.0),
    (-0.6, 0.0, 0.0),
]

JOINTS = [(0, 0, 0, 0)] * 4          # unsigned byte
WEIGHTS = [(1.0, 0.0, 0.0, 0.0)] * 4  # float

IDENTITY = [1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0]


def pad4(b: bytes, fill: bytes = b"\x00") -> bytes:
    return b + fill * ((4 - len(b) % 4) % 4)


def main() -> int:
    blob = b""
    views = []
    accessors = []

    def add_view(data: bytes, target=None):
        nonlocal blob
        blob = pad4(blob)
        off = len(blob)
        blob += data
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target is not None:
            v["bufferTarget"] = target
        views.append(v)
        return len(views) - 1

    def add_vec3(vals, with_minmax=False):
        data = b"".join(struct.pack("<3f", *v) for v in vals)
        a = {"bufferView": add_view(data), "componentType": 5126,
             "count": len(vals), "type": "VEC3"}
        if with_minmax:
            a["min"] = [min(v[i] for v in vals) for i in range(3)]
            a["max"] = [max(v[i] for v in vals) for i in range(3)]
        accessors.append(a)
        return len(accessors) - 1

    def add_vec4f(vals):
        data = b"".join(struct.pack("<4f", *v) for v in vals)
        accessors.append({"bufferView": add_view(data), "componentType": 5126,
                          "count": len(vals), "type": "VEC4"})
        return len(accessors) - 1

    def add_vec4b(vals):
        data = b"".join(struct.pack("<4B", *v) for v in vals)
        accessors.append({"bufferView": add_view(data), "componentType": 5121,
                          "count": len(vals), "type": "VEC4"})
        return len(accessors) - 1

    def add_scalar_u16(vals):
        data = b"".join(struct.pack("<H", v) for v in vals)
        accessors.append({"bufferView": add_view(data), "componentType": 5123,
                          "count": len(vals), "type": "SCALAR"})
        return len(accessors) - 1

    def add_mat4(mats):
        data = b"".join(struct.pack("<16f", *m) for m in mats)
        accessors.append({"bufferView": add_view(data), "componentType": 5126,
                          "count": len(mats), "type": "MAT4"})
        return len(accessors) - 1

    a_pos  = add_vec3(POSITIONS, with_minmax=True)
    a_nrm  = add_vec3(NORMALS)
    a_joint = add_vec4b(JOINTS)
    a_weight = add_vec4f(WEIGHTS)
    a_idx  = add_scalar_u16(INDICES)
    a_dpos = add_vec3(DPOS, with_minmax=True)
    a_dnrm = add_vec3(DNRM)
    a_ibm  = add_mat4([IDENTITY])

    gltf = {
        "asset": {"version": "2.0", "generator": "jce tools/gen_morph_probe_glb.py"},
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [
            {"mesh": 0, "skin": 0, "name": "morph_probe"},
            {"name": "joint0"},
        ],
        "skins": [{"inverseBindMatrices": a_ibm, "joints": [1]}],
        "meshes": [{
            "name": "morph_probe_mesh",
            # The base weight is 0: the SCENE decides, through its
            # MorphWeights component, so the on/off control is one field in
            # one file rather than two assets that could differ in other ways.
            "weights": [0.0],
            "primitives": [{
                "attributes": {
                    "POSITION": a_pos,
                    "NORMAL": a_nrm,
                    "JOINTS_0": a_joint,
                    "WEIGHTS_0": a_weight,
                },
                "indices": a_idx,
                "targets": [{"POSITION": a_dpos, "NORMAL": a_dnrm}],
            }],
        }],
        "materials": [],
        "buffers": [{"byteLength": len(blob)}],
        "bufferViews": [{k: v for k, v in bv.items() if k != "bufferTarget"}
                        for bv in views],
        "accessors": accessors,
    }

    js = json.dumps(gltf, separators=(",", ":"), sort_keys=True).encode("utf-8")
    js = pad4(js, b" ")
    bin_chunk = pad4(blob)

    total = 12 + 8 + len(js) + 8 + len(bin_chunk)
    out = b"glTF" + struct.pack("<II", 2, total)
    out += struct.pack("<I", len(js)) + b"JSON" + js
    out += struct.pack("<I", len(bin_chunk)) + b"BIN\x00" + bin_chunk

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(out)
    print("wrote %s (%d bytes, %d verts, 1 morph target)"
          % (OUT.relative_to(ROOT), len(out), len(POSITIONS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
