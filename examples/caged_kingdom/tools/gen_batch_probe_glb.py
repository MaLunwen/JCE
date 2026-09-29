#!/usr/bin/env python3
"""gen_batch_probe_glb.py -- author the static-batch member-culling fixture.

WHY THIS EXISTS.  The member table is proven at every layer by
tests/middleware/scene/test_jce_static_batch_members.c -- the bake writes it,
the loader reads it, the run coalescer turns it into the right submits.  What
no test can say is whether it reaches the SUBMIT on a real frame, because the
tree contains no baked group to point a camera at.  This writes one.

It is a MERGED GROUP, by hand, in exactly the form jce_static_batch_bake
produces: eight quads already in world space along +X in one .glb, and a
<stem>.batch.json listing each one's index range and world box.  Writing it by
hand rather than running the bake is deliberate and safe: the bake-to-loader
agreement is what the C test already proves end to end, and what this fixture
is for is the half after that -- the renderer.

The quads are 6 metres apart so a near camera frames two or three of them and
a far one frames all eight, which is the only difference between the two
scenes this ships beside.

Regenerate with:
    python examples/caged_kingdom/tools/gen_batch_probe_glb.py
"""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GLB = ROOT / "resources/assets/models/batch_probe.glb"
SIDE = ROOT / "resources/assets/models/batch_probe.batch.json"

N = 8            # members
SPACING = 6.0    # metres between them, along +X
W, H = 2.0, 3.0  # each quad


def pad4(b: bytes, fill: bytes = b"\x00") -> bytes:
    return b + fill * ((4 - len(b) % 4) % 4)


def main() -> int:
    positions, normals, indices, members = [], [], [], []
    for i in range(N):
        x0 = -((N - 1) * SPACING) * 0.5 + i * SPACING
        base_v = len(positions) // 3
        first_i = len(indices)
        for (dx, dy) in ((-W / 2, 0.0), (W / 2, 0.0), (-W / 2, H), (W / 2, H)):
            positions += [x0 + dx, dy, 0.0]
            normals += [0.0, 0.0, 1.0]
        indices += [base_v + 0, base_v + 1, base_v + 2,
                    base_v + 2, base_v + 1, base_v + 3]
        members.append({
            "firstIndex": first_i,
            "indexCount": 6,
            "aabbMin": [x0 - W / 2, 0.0, 0.0],
            "aabbMax": [x0 + W / 2, H, 0.0],
        })

    blob = b""
    views, accessors = [], []

    def add_view(data: bytes) -> int:
        nonlocal blob
        blob = pad4(blob)
        off = len(blob)
        blob += data
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(data)})
        return len(views) - 1

    def add_vec3(vals, minmax=False) -> int:
        data = b"".join(struct.pack("<f", v) for v in vals)
        a = {"bufferView": add_view(data), "componentType": 5126,
             "count": len(vals) // 3, "type": "VEC3"}
        if minmax:
            xs, ys, zs = vals[0::3], vals[1::3], vals[2::3]
            a["min"] = [min(xs), min(ys), min(zs)]
            a["max"] = [max(xs), max(ys), max(zs)]
        accessors.append(a)
        return len(accessors) - 1

    a_pos = add_vec3(positions, minmax=True)
    a_nrm = add_vec3(normals)
    idx_data = b"".join(struct.pack("<H", v) for v in indices)
    accessors.append({"bufferView": add_view(idx_data), "componentType": 5123,
                      "count": len(indices), "type": "SCALAR"})
    a_idx = len(accessors) - 1

    gltf = {
        "asset": {"version": "2.0",
                  "generator": "jce tools/gen_batch_probe_glb.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        # ONE node, identity transform -- a merged group is already in world
        # space, which is what removed the per-object work in the first place.
        "nodes": [{"mesh": 0, "name": "batch_probe"}],
        "meshes": [{"name": "batch_probe_mesh", "primitives": [{
            "attributes": {"POSITION": a_pos, "NORMAL": a_nrm},
            "indices": a_idx,
        }]}],
        "buffers": [{"byteLength": len(blob)}],
        "bufferViews": views,
        "accessors": accessors,
    }

    js = pad4(json.dumps(gltf, separators=(",", ":"),
                         sort_keys=True).encode("utf-8"), b" ")
    bin_chunk = pad4(blob)
    total = 12 + 8 + len(js) + 8 + len(bin_chunk)
    out = b"glTF" + struct.pack("<II", 2, total)
    out += struct.pack("<I", len(js)) + b"JSON" + js
    out += struct.pack("<I", len(bin_chunk)) + b"BIN\x00" + bin_chunk

    GLB.parent.mkdir(parents=True, exist_ok=True)
    GLB.write_bytes(out)
    SIDE.write_text(json.dumps(
        {"contract": {"name": "jce.staticbatch", "major": 1, "minor": 0},
         "members": members}, indent=1) + "\n",
        encoding="utf-8", newline="\n")
    print("wrote %s (%d bytes, %d members, %d indices)"
          % (GLB.relative_to(ROOT), len(out), N, len(indices)))
    print("wrote %s" % SIDE.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
