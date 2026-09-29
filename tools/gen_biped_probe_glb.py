#!/usr/bin/env python3
"""gen_biped_probe_glb.py -- author the rigged fixture L1 ragdoll work is graded on.

WHY GENERATED.  Every skinned .glb in this tree (23 of them) lives under
examples/caged_kingdom/ -- the user project.  A general-engine test that loaded one
would make the engine's acceptance depend on a consumer's assets, which is the
direction this repository explicitly forbids.  So the fixture is authored here,
in the general tree, from numbers written down in this file.

WHY NOT trimesh.  trimesh reads glTF skins but does not write them, and the
thing under test IS the skin: jce_gltf_loader builds JceSkeleton from
`skin->joints`, so a .glb without a skin produces no skeleton and the ragdoll
silently never spawns.  The glTF is therefore assembled here by hand, with the
standard library only.

WHY A HEIGHT PARAMETER.  The engine's ragdoll takes ONE capsule radius for
every bone in the body, as an absolute number in metres, defaulting to 0.08.
An absolute radius is correct at exactly one scale: the same rig at a quarter
of the height has a quarter of the clearance between its thighs, and 0.08 m
then spawns the two leg capsules deep inside one another.  build_glb(height=)
is what lets a test show that, without committing a second binary fixture.

The skeleton is 19 joints of ordinary human proportion, all rotations
identity, so each joint's local translation is its world offset from its
parent and the file can be read by eye.  Every joint also carries a small
skinned box so the file is a valid skinned mesh rather than a skeleton with
nothing attached to it.

USAGE
  python tools/gen_biped_probe_glb.py                 # writes the fixture
  python tools/gen_biped_probe_glb.py --check         # verifies it is current
  python tools/gen_biped_probe_glb.py --describe      # prints the measurements

The output is byte-deterministic: same interpreter, same bytes, so the fixture
an acceptance case is graded on can be rebuilt rather than trusted.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

# Joint table: (name, parent index, world position at bind pose, in metres).
# Proportions are an ordinary adult biped, ~1.75 m to the crown.  The numbers
# that matter for a ragdoll are the ones that set CLEARANCE between bones that
# are not parent and child, because those are the pairs the solver collides:
# the thighs at x = +-0.09 are 0.18 m apart, and the upper arms clear the chest
# by 0.19 m.  A capsule radius above half the smallest of those spawns two
# bodies inside one another.
JOINTS = [
    ("Hips",       -1, (0.00, 0.95, 0.00)),
    ("Spine",       0, (0.00, 1.12, 0.00)),
    ("Chest",       1, (0.00, 1.30, 0.00)),
    ("Neck",        2, (0.00, 1.48, 0.00)),
    ("Head",        3, (0.00, 1.60, 0.00)),
    ("Shoulder_L",  2, (0.07, 1.44, 0.00)),
    ("UpperArm_L",  5, (0.19, 1.44, 0.00)),
    ("LowerArm_L",  6, (0.19, 1.16, 0.00)),
    ("Hand_L",      7, (0.19, 0.92, 0.00)),
    ("Shoulder_R",  2, (-0.07, 1.44, 0.00)),
    ("UpperArm_R",  9, (-0.19, 1.44, 0.00)),
    ("LowerArm_R", 10, (-0.19, 1.16, 0.00)),
    ("Hand_R",     11, (-0.19, 0.92, 0.00)),
    ("UpperLeg_L",  0, (0.09, 0.92, 0.00)),
    ("LowerLeg_L", 13, (0.09, 0.50, 0.00)),
    ("Foot_L",     14, (0.09, 0.08, 0.00)),
    ("UpperLeg_R",  0, (-0.09, 0.92, 0.00)),
    ("LowerLeg_R", 16, (-0.09, 0.50, 0.00)),
    ("Foot_R",     17, (-0.09, 0.08, 0.00)),
]

# Half-extents of the little box each joint is skinned to.  Purely so the file
# is a mesh; the ragdoll never reads it.
BOX_HALF = 0.035

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942


def _pad4(blob):
    return blob + b"\0" * (-len(blob) % 4)


def build_glb(height=1.75):
    """The fixture's bytes, with every joint position scaled to `height`.

    The table above is authored at 1.75 m, so the scale is height/1.75 and it
    multiplies positions, which is what makes clearance scale with it too --
    the property the radius decision is measured against.
    """
    s = float(height) / 1.75
    pos = [(p[0] * s, p[1] * s, p[2] * s) for _, _, p in JOINTS]
    n = len(JOINTS)

    # Local translation: the offset from the parent, since no joint rotates.
    local = []
    for i, (_, parent, _) in enumerate(JOINTS):
        if parent < 0:
            local.append(pos[i])
        else:
            local.append(tuple(pos[i][k] - pos[parent][k] for k in range(3)))

    # Inverse bind matrix: the inverse of a pure translation, column-major as
    # glTF stores it.
    ibm = []
    for i in range(n):
        m = [1.0, 0.0, 0.0, 0.0,
             0.0, 1.0, 0.0, 0.0,
             0.0, 0.0, 1.0, 0.0,
             -pos[i][0], -pos[i][1], -pos[i][2], 1.0]
        ibm.extend(m)

    half = BOX_HALF * s
    positions, joints_attr, weights, indices = [], [], [], []
    for i in range(n):
        base = len(positions)
        cx, cy, cz = pos[i]
        for dx in (-half, half):
            for dy in (-half, half):
                for dz in (-half, half):
                    positions.append((cx + dx, cy + dy, cz + dz))
                    joints_attr.append((i, 0, 0, 0))
                    weights.append((1.0, 0.0, 0.0, 0.0))
        # 6 faces of the 8-corner box, in the corner order produced above.
        for a, b, c in ((0, 1, 3), (0, 3, 2), (4, 7, 5), (4, 6, 7),
                        (0, 4, 5), (0, 5, 1), (2, 3, 7), (2, 7, 6),
                        (0, 2, 6), (0, 6, 4), (1, 5, 7), (1, 7, 3)):
            indices.extend((base + a, base + b, base + c))

    blocks = []

    def put(blob, target=None):
        blob = _pad4(blob)
        off = sum(len(b[0]) for b in blocks)
        blocks.append((blob, off, target))
        return off, len(blob)

    pos_off, pos_len = put(b"".join(struct.pack("<3f", *p) for p in positions),
                           34962)
    jnt_off, jnt_len = put(b"".join(struct.pack("<4H", *j) for j in joints_attr),
                           34962)
    wgt_off, wgt_len = put(b"".join(struct.pack("<4f", *w) for w in weights),
                           34962)
    idx_off, idx_len = put(b"".join(struct.pack("<H", i) for i in indices),
                           34963)
    ibm_off, ibm_len = put(struct.pack("<%df" % len(ibm), *ibm), None)

    bin_blob = b"".join(b[0] for b in blocks)

    mins = [min(p[k] for p in positions) for k in range(3)]
    maxs = [max(p[k] for p in positions) for k in range(3)]

    nodes = []
    for i, (name, _parent, _p) in enumerate(JOINTS):
        node = {"name": name, "translation": [round(v, 6) for v in local[i]]}
        kids = [k for k, (_, par, _) in enumerate(JOINTS) if par == i]
        if kids:
            node["children"] = kids
        nodes.append(node)
    mesh_node_index = len(nodes)
    nodes.append({"name": "BipedMesh", "mesh": 0, "skin": 0})

    doc = {
        "asset": {"version": "2.0",
                  "generator": "jce gen_biped_probe_glb.py"},
        "scene": 0,
        "scenes": [{"nodes": [0, mesh_node_index]}],
        "nodes": nodes,
        "skins": [{"name": "BipedRig",
                   "joints": list(range(n)),
                   "skeleton": 0,
                   "inverseBindMatrices": 4}],
        "meshes": [{"name": "Biped",
                    "primitives": [{"attributes": {"POSITION": 0,
                                                   "JOINTS_0": 1,
                                                   "WEIGHTS_0": 2},
                                    "indices": 3,
                                    "mode": 4}]}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(positions),
             "type": "VEC3",
             "min": [round(v, 6) for v in mins],
             "max": [round(v, 6) for v in maxs]},
            {"bufferView": 1, "componentType": 5123, "count": len(joints_attr),
             "type": "VEC4"},
            {"bufferView": 2, "componentType": 5126, "count": len(weights),
             "type": "VEC4"},
            {"bufferView": 3, "componentType": 5123, "count": len(indices),
             "type": "SCALAR"},
            {"bufferView": 4, "componentType": 5126, "count": n, "type": "MAT4"},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": pos_off, "byteLength": pos_len,
             "target": 34962},
            {"buffer": 0, "byteOffset": jnt_off, "byteLength": jnt_len,
             "target": 34962},
            {"buffer": 0, "byteOffset": wgt_off, "byteLength": wgt_len,
             "target": 34962},
            {"buffer": 0, "byteOffset": idx_off, "byteLength": idx_len,
             "target": 34963},
            {"buffer": 0, "byteOffset": ibm_off, "byteLength": ibm_len},
        ],
        "buffers": [{"byteLength": len(bin_blob)}],
    }

    json_blob = _pad4(json.dumps(doc, separators=(",", ":"),
                                 sort_keys=True).encode("utf-8"))
    json_blob = json_blob.replace(b"\0", b" ")     # JSON chunk pads with spaces
    bin_blob = _pad4(bin_blob)
    body = (struct.pack("<II", len(json_blob), CHUNK_JSON) + json_blob
            + struct.pack("<II", len(bin_blob), CHUNK_BIN) + bin_blob)
    return struct.pack("<III", GLB_MAGIC, 2, 12 + len(body)) + body


def describe(height=1.75):
    """The measurements a ragdoll decision is made from, printed.

    Reproduces the ENGINE's capsule construction (jce_ragdoll.c): one capsule
    per joint, centred on the JOINT ORIGIN -- not the bone midpoint -- with
    half-length equal to half the distance to the joint's first child, or to
    its parent for a leaf.
    """
    s = float(height) / 1.75
    pos = [tuple(v * s for v in p) for _, _, p in JOINTS]
    print("joint                parent        world position        bone length")
    for i, (name, parent, _) in enumerate(JOINTS):
        child = next((k for k in range(i + 1, len(JOINTS))
                      if JOINTS[k][1] == i), None)
        if child is not None:
            ln = sum((pos[child][k] - pos[i][k]) ** 2 for k in range(3)) ** 0.5
        elif parent >= 0:
            ln = sum((pos[i][k] - pos[parent][k]) ** 2 for k in range(3)) ** 0.5
        else:
            ln = 0.0
        pname = JOINTS[parent][0] if parent >= 0 else "-"
        print("  %-18s %-12s (%6.3f %6.3f %6.3f)  %.4f"
              % (name, pname, pos[i][0], pos[i][1], pos[i][2], ln))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default="tools/fixtures/biped_probe.glb")
    ap.add_argument("--height", type=float, default=1.75)
    ap.add_argument("--check", action="store_true",
                    help="verify the committed fixture matches this generator")
    ap.add_argument("--describe", action="store_true")
    args = ap.parse_args(argv)

    if args.describe:
        describe(args.height)
        return 0

    data = build_glb(args.height)
    out = Path(args.out)
    digest = hashlib.sha256(data).hexdigest()

    if args.check:
        if not out.is_file():
            print("gen_biped_probe_glb: FAIL - %s is missing" % out)
            return 1
        have = out.read_bytes()
        if have != data:
            print("gen_biped_probe_glb: FAIL - %s does not match this "
                  "generator (%d bytes / %s on disk, %d / %s regenerated)"
                  % (out, len(have), hashlib.sha256(have).hexdigest()[:16],
                     len(data), digest[:16]))
            return 1
        print("gen_biped_probe_glb: OK - %s matches (%d bytes, sha256 %s)"
              % (out, len(data), digest[:16]))
        return 0

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    again = build_glb(args.height)
    print("gen_biped_probe_glb: wrote %s (%d bytes, sha256 %s, %d joints, "
          "deterministic: %s)"
          % (out, len(data), digest[:16], len(JOINTS), again == data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
