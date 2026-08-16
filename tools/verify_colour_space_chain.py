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
        glb = None
        for cand in ROOT.rglob("*.glb"):
            if "build" in cand.parts or "dist" in cand.parts:
                continue
            glb = cand
            break
        if glb is None:
            print("  skip: no .glb in the tree to import")
        else:
            work = tmp / "import"
            work.mkdir()
            local = work / glb.name
            shutil.copyfile(glb, local)
            r = subprocess.run([str(cook_exe), "--extract-material", str(local)],
                               capture_output=True)
            if r.returncode != 0:
                fail(f"--extract-material failed: {r.stderr.decode(errors='replace')[:200]}")
            sidecars = sorted(work.glob("*.import.json"))
            if not sidecars:
                fail(f"{glb.name} produced no colour-space sidecar")
            bad = []
            for s in sidecars:
                v = json.loads(s.read_text(encoding="utf-8")).get("colorSpace")
                if v not in ("srgb", "linear"):
                    bad.append((s.name, v))
                print(f"  ok    {s.name:<44s} colorSpace {v!r}")
            if bad:
                fail(f"sidecars with an unusable value: {bad}")

        print()
        print("colour-space chain: PASS")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
