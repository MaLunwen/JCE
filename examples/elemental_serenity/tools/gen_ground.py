#!/usr/bin/env python3
"""
gen_ground.py — emit a minimal flat ground-plane GLB (55x55 quad at y=0,
normal +Y, with UVs 0..1 for a future baked biome texture).

Writes: resources/assets/models/ground_plane55.glb

Run:  python examples/elemental_serenity/tools/gen_ground.py
"""
import struct, json, os

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
OUT  = os.path.join(PROJ, "resources", "assets", "models", "ground_plane55.glb")

H = 27.5  # half-size -> 55x55 (reference ground grid: 5x5 tiles of 11; the
          # bake covers the same extent, density-clamped beyond the 33 world)
# 4 verts CCW from above (normal +Y): -X-Z, +X-Z, +X+Z, -X+Z
positions = [(-H,0,-H),(H,0,-H),(H,0,H),(-H,0,H)]
normals   = [(0,1,0)]*4
uvs       = [(0,0),(1,0),(1,1),(0,1)]
# TANGENT (VEC4): +X (u direction) with +1 handedness. fs_pbr builds its TBN
# from this; a mesh WITHOUT tangents yields a garbage shading basis -> the
# whole plane goes unlit/dark (this is why the ground was black while the
# rocks, which ship tangents, lit fine).
tangents  = [(1,0,0,1)]*4
indices   = [0,2,1, 0,3,2]   # CCW-from-above so geometric normal = +Y (was backface-culled)

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
uv_bytes  = b"".join(struct.pack("<2f", *u) for u in uvs)
tan_bytes = b"".join(struct.pack("<4f", *t) for t in tangents)
idx_bytes = b"".join(struct.pack("<H", i) for i in indices)

# pad each section to 4-byte alignment
def pad4(b): return b + b"\x00" * ((4 - len(b) % 4) % 4)
bin_blob = pad4(pos_bytes) + pad4(nrm_bytes) + pad4(uv_bytes) + pad4(tan_bytes) + pad4(idx_bytes)
off_pos = 0
off_nrm = len(pad4(pos_bytes))
off_uv  = off_nrm + len(pad4(nrm_bytes))
off_tan = off_uv + len(pad4(uv_bytes))
off_idx = off_tan + len(pad4(tan_bytes))

gltf = {
  "asset": {"version": "2.0", "generator": "gen_ground.py"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0, "name": "Ground"}],
  "materials": [{
    "name": "ground", "doubleSided": True,
    "pbrMetallicRoughness": {"baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                             "metallicFactor": 0.0, "roughnessFactor": 1.0}
  }],
  "meshes": [{"name": "Ground", "primitives": [{
    "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "TANGENT": 4},
    "indices": 3, "material": 0, "mode": 4
  }]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
     "min": [-H,0,-H], "max": [H,0,H]},
    {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
    {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
    {"bufferView": 4, "componentType": 5126, "count": 4, "type": "VEC4"},
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": off_pos, "byteLength": len(pos_bytes), "target": 34962},
    {"buffer": 0, "byteOffset": off_nrm, "byteLength": len(nrm_bytes), "target": 34962},
    {"buffer": 0, "byteOffset": off_uv,  "byteLength": len(uv_bytes),  "target": 34962},
    {"buffer": 0, "byteOffset": off_idx, "byteLength": len(idx_bytes), "target": 34963},
    {"buffer": 0, "byteOffset": off_tan, "byteLength": len(tan_bytes), "target": 34962},
  ],
  "buffers": [{"byteLength": len(bin_blob)}],
}

def write_glb(path, gltf_doc, bin_data):
    jb = json.dumps(gltf_doc, separators=(",", ":")).encode("utf-8")
    jb += b" " * ((4 - len(jb) % 4) % 4)          # JSON chunk pads with 0x20
    bb = bin_data + b"\x00" * ((4 - len(bin_data) % 4) % 4)
    def chunk(ctype, data):
        return struct.pack("<II", len(data), ctype) + data
    body = chunk(0x4E4F534A, jb) + chunk(0x004E4942, bb)
    hdr = struct.pack("<III", 0x46546C67, 2, 12 + len(body))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(hdr + body)
    print(f"wrote {path}: {12+len(body)} bytes")

write_glb(OUT, gltf, bin_blob)

# ---- leaf/flower card: 1x1 XZ quad, UV 0..1, centered (the reference's
# leaf.glb 'Circle' mesh carries NO texcoords, so alpha-masked textures
# sample a constant texel and the card discards entirely) ------------------
CARD = os.path.join(PROJ, "resources", "assets", "models", "card.glb")
cH = 0.5
c_pos = [(-cH,0,-cH),(cH,0,-cH),(cH,0,cH),(-cH,0,cH)]
c_bytes = b"".join(struct.pack("<3f", *p) for p in c_pos)
c_nrm   = b"".join(struct.pack("<3f", 0,1,0) for _ in range(4))
c_uv    = b"".join(struct.pack("<2f", *u) for u in [(0,0),(1,0),(1,1),(0,1)])
c_idx   = b"".join(struct.pack("<H", i) for i in [0,1,2, 0,2,3])
c_bin = pad4(c_bytes) + pad4(c_nrm) + pad4(c_uv) + pad4(c_idx)
o_nrm = len(pad4(c_bytes)); o_uv = o_nrm + len(pad4(c_nrm)); o_idx = o_uv + len(pad4(c_uv))
card = {
  "asset": {"version": "2.0", "generator": "gen_ground.py"},
  "scene": 0, "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0, "name": "Card"}],
  "materials": [{"name": "card", "alphaMode": "MASK", "alphaCutoff": 0.4,
                 "doubleSided": True,
                 "pbrMetallicRoughness": {"baseColorFactor": [1,1,1,1],
                                          "metallicFactor": 0.0, "roughnessFactor": 1.0}}],
  "meshes": [{"name": "Card", "primitives": [{
      "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
      "indices": 3, "material": 0, "mode": 4}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
     "min": [-cH,0,-cH], "max": [cH,0,cH]},
    {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
    {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0,     "byteLength": len(c_bytes), "target": 34962},
    {"buffer": 0, "byteOffset": o_nrm, "byteLength": len(c_nrm),   "target": 34962},
    {"buffer": 0, "byteOffset": o_uv,  "byteLength": len(c_uv),    "target": 34962},
    {"buffer": 0, "byteOffset": o_idx, "byteLength": len(c_idx),   "target": 34963}],
  "buffers": [{"byteLength": len(c_bin)}],
}
write_glb(CARD, card, c_bin)
