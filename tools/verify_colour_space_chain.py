#!/usr/bin/env python3
"""verify_colour_space_chain.py -- acceptance for the texture colour-space chain.

WHY THIS EXISTS.  The unit tests cover the two ends: the mip filter's arithmetic
(test_jce_tex_mip_srgb) and the sidecar value parser. What they cannot reach is
the chain BETWEEN them, which crosses a process boundary twice:

    model importer  ->  <texture>.import.json  ->  cooker  ->  cooked mip bytes

Every link there was verified once, by hand, by me, on this machine. That is not
verification -- it is an anecdote. If the sidecar key is renamed on one side, if
the cooker stops reading it, if the safe default flips, or if a build simply
stops picking the reader up, all of it stays green and nobody finds out until a
scene looks wrong at a distance and somebody spends a week on shadows.

So this reruns the whole chain and asserts the BYTES.

The probe texture is a 1-texel checkerboard of pure white and pure black. That
choice is the point: a box filter over four EQUAL bytes returns that byte in any
colour space, so flat content proves nothing and this defect hid behind flat
content for as long as it existed. At maximum contrast the two answers are 61
levels apart -- 127 averaged as stored, 188 averaged in linear light -- and no
plausible bug lands between them.

  python tools/verify_colour_space_chain.py [--cook <path to jce_cook>]

Exits 0 on pass, 1 on the first failed assertion, 2 if it could not run.
"""

import argparse
import collections
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_COOK = ROOT / "build/desktop/windows-x64/tools/jce_cook.exe"

# 64x64 checkerboard -> mip 1 is a solid plane of one value, 32*32 texels.
SIZE = 64
GAMMA_ANSWER = 127          # (255+255+0+0)/4, averaged as stored
LINEAR_ANSWER = 188         # sRGB-decode, average, re-encode


def fail(msg):
    print(f"  FAIL: {msg}")
    sys.exit(1)


def write_probe(path):
    try:
        from PIL import Image
        import numpy as np
    except ImportError:
        print("verify_colour_space_chain: needs Pillow + numpy")
        sys.exit(2)
    a = np.zeros((SIZE, SIZE, 4), np.uint8)
    a[:, :, 3] = 255
    yy, xx = np.mgrid[0:SIZE, 0:SIZE]
    a[:, :, :3] = ((yy + xx) % 2 == 0)[..., None] * 255
    Image.fromarray(a, "RGBA").save(path)


def cook(cook_exe, src, dst):
    """Uncompressed RGBA8 with mips, so the mip plane can be read directly."""
    r = subprocess.run([str(cook_exe), str(src), str(dst),
                        "--mipmaps", "--rgba8", "--level", "0"],
                       capture_output=True)
    if r.returncode != 0 or not Path(dst).exists():
        fail(f"cook failed for {src.name}: {r.stderr.decode(errors='replace')[:200]}")


def mip_value(blob_path):
    """The dominant byte just past the base plane -- i.e. the mip-1 colour."""
    b = Path(blob_path).read_bytes()
    tail = b[SIZE * SIZE * 4 + 64:]
    if len(tail) < 1000:
        fail(f"{Path(blob_path).name} has no mip chain (is --mipmaps honoured?)")
    return collections.Counter(tail[:4000]).most_common(1)[0][0]


def check(label, got, want):
    ok = got == want
    print(f"  {'ok  ' if ok else 'FAIL'}  {label:<44s} mip byte {got} (expected {want})")
    if not ok:
        sys.exit(1)


def write_import_probe(path, texture):
    """Generate a textured glTF triangle; no consumer asset or directory scan."""
    positions = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
    uv = struct.pack("<6f", 0, 0, 1, 0, 0, 1)
    indices = struct.pack("<3H", 0, 1, 2)
    payload = positions + uv + indices + b"\0\0" + texture.read_bytes()
    doc = {
        "asset": {"version": "2.0"}, "scene": 0,
        "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
        "buffers": [{"byteLength": len(payload)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 24},
            {"buffer": 0, "byteOffset": 60, "byteLength": 6},
            {"buffer": 0, "byteOffset": 68, "byteLength": len(payload) - 68}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
            {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"},
            {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}],
        "images": [{"bufferView": 3, "mimeType": "image/png", "name": "probe%d" % i} for i in range(5)],
        "textures": [{"source": i} for i in range(5)],
        "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicRoughnessTexture": {"index": 1}}, "normalTexture": {"index": 2}, "occlusionTexture": {"index": 3}, "emissiveTexture": {"index": 4}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": 0}]}],
    }
    header = json.dumps(doc, separators=(",", ":")).encode()
    header += b" " * (-len(header) % 4)
    payload += b"\0" * (-len(payload) % 4)
    data = struct.pack("<III", 0x46546C67, 2, 28 + len(header) + len(payload))
    data += struct.pack("<I4s", len(header), b"JSON") + header
    data += struct.pack("<I4s", len(payload), b"BIN\0") + payload
    path.write_bytes(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cook", default=str(DEFAULT_COOK))
    args = ap.parse_args()
    cook_exe = Path(args.cook)
    if not cook_exe.exists():
        print(f"verify_colour_space_chain: jce_cook not built at {cook_exe}")
        sys.exit(2)

    tmp = Path(tempfile.mkdtemp(prefix="jce_cs_"))
    try:
        print("A. the cooker honours the sidecar, and defaults safely without one")
        # Named `_tex0` on purpose: the shape of a GLB-embedded texture, whose
        # name carries no semantic at all. 114 of this repo's textures look
        # like this, and they are the population the sidecar exists for.
        probe = tmp / "Probe_tex0.png"
        write_probe(probe)
        side = Path(str(probe) + ".import.json")

        cook(cook_exe, probe, tmp / "none.jceasset")
        check("no sidecar -> averaged as stored", mip_value(tmp / "none.jceasset"),
              GAMMA_ANSWER)

        side.write_text(json.dumps({"colorSpace": "srgb"}), encoding="utf-8")
        cook(cook_exe, probe, tmp / "srgb.jceasset")
        check('colorSpace "srgb" -> linear light', mip_value(tmp / "srgb.jceasset"),
              LINEAR_ANSWER)

        side.write_text(json.dumps({"colorSpace": "linear"}), encoding="utf-8")
        cook(cook_exe, probe, tmp / "lin.jceasset")
        check('colorSpace "linear" -> as stored', mip_value(tmp / "lin.jceasset"),
              GAMMA_ANSWER)

        # An unrecognised value must not be read as either of the two known
        # ones; the cooker keeps the answer it already had.
        side.write_text(json.dumps({"colorSpace": "rec2020"}), encoding="utf-8")
        cook(cook_exe, probe, tmp / "unk.jceasset")
        check('colorSpace "rec2020" -> unchanged', mip_value(tmp / "unk.jceasset"),
              GAMMA_ANSWER)

        print()
        print("B. the model importer records the slot it filled")
        work = tmp / "import"
        work.mkdir()
        local = work / "colour_probe.glb"
        write_import_probe(local, probe)
        result = subprocess.run([str(cook_exe), "--extract-material", str(local)], capture_output=True)
        if result.returncode != 0:
            fail("--extract-material failed: " + result.stderr.decode(errors="replace")[:200])
        sidecars = sorted(work.glob("*.import.json"))
        if len(sidecars) != 5:
            fail("generated five-slot model produced %d colour-space sidecars" % len(sidecars))
        values = [json.loads(file.read_text(encoding="utf-8")).get("colorSpace") for file in sidecars]
        if collections.Counter(values) != {"srgb": 2, "linear": 3}:
            fail("albedo/emissive must be sRGB; normal/occlusion/metal-roughness must be linear: " + repr(values))
        for file, value in zip(sidecars, values):
            print("  ok    %-44s colorSpace %r" % (file.name, value))

        print()
        print("colour-space chain: PASS")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
