#!/usr/bin/env python3
"""gen_vehicle_probe_glb.py -- author the vehicle fixture L1 physics is graded on.

WHY THIS EXISTS.  The Agent-Native spec grades AI physics authoring on a
concrete case: "import a glTF car sample, have L1 produce RigidBody / Compound
/ Wheel / Joint / centre of mass, and probe it for 10 s with no penetration, no
NaN and no rollover" (§6.4).  This tree has 693 .glb and .gltf files and not
one vehicle: the closest multi-part models are a 138-part asset pack and a
cemetery pack.  So the acceptance case could not be run, and any claim about it
would have been unsupportable.

Same reason and same shape as examples/caged_kingdom/tools/gen_morph_probe_glb.py, which exists
because the tree carried a whole blendshape pipeline and zero .glb with a
`targets` array in it.

WHAT IT PRODUCES.  A chassis box and four wheel cylinders, as five separate
nodes, at dimensions taken from an ordinary passenger car (4.2 x 1.8 x 0.8 m
body, 0.35 m wheel radius, 0.25 m tread).  Five nodes rather than one merged
mesh is the point: L1's job is to notice that four of them are wheels and one
is not, and a single merged mesh would make that question unaskable.

Node names follow the convention a real export would carry (Body, Wheel_FL,
Wheel_FR, Wheel_RL, Wheel_RR) -- AND the geometry says the same thing
independently, because a detector that only reads names is a detector that
fails on the first asset exported from a different tool.  Both signals are
present so the detector can be checked against either.

BYTE-DETERMINISTIC.  The vertices come from fixed arithmetic, the node order is
fixed, and the exporter is given no timestamp.  Two runs produce identical
bytes; the script verifies that itself with --check.
"""
from __future__ import annotations

import argparse
import hashlib
import logging
import math
import sys
import warnings
from pathlib import Path

warnings.filterwarnings("ignore")
logging.getLogger("trimesh").setLevel(logging.ERROR)

try:
    import numpy as np
    import trimesh
except ImportError:  # pragma: no cover
    print("gen_vehicle_probe_glb: needs trimesh and numpy "
          "(pip install trimesh)", file=sys.stderr)
    raise SystemExit(2)

# An ordinary passenger car, in metres.  Stated as constants rather than
# buried in the geometry so the expected mass and centre of mass can be
# computed by hand from this block when checking what L1 produced.
BODY_L, BODY_W, BODY_H = 4.20, 1.80, 0.80
BODY_CENTRE_Y = 0.75          # chassis sits above the wheel axis
WHEEL_R, WHEEL_TREAD = 0.35, 0.25
AXLE_FRONT_Z, AXLE_REAR_Z = 1.40, -1.40
TRACK_HALF = 0.80             # half the distance between left and right wheels
WHEEL_SEGMENTS = 24           # fixed: a default that changed would change bytes


def _box(lx, ly, lz, centre):
    m = trimesh.creation.box(extents=(lx, ly, lz))
    m.apply_translation(centre)
    return m


def _wheel(centre):
    """A cylinder lying on the X axis, which is how a wheel is oriented.

    Built with an explicit transform rather than trimesh's `transform=`
    convenience so the rotation is visible: a wheel generated upright is a
    wheel that will not roll, and that mistake produces a fixture which passes
    every structural check and fails the only test that matters.
    """
    m = trimesh.creation.cylinder(radius=WHEEL_R, height=WHEEL_TREAD,
                                  sections=WHEEL_SEGMENTS)
    rot = trimesh.transformations.rotation_matrix(math.pi / 2.0, [0, 1, 0])
    m.apply_transform(rot)
    m.apply_translation(centre)
    return m


def build_scene():
    scene = trimesh.Scene()
    scene.add_geometry(_box(BODY_W, BODY_H, BODY_L,
                            (0.0, BODY_CENTRE_Y, 0.0)), node_name="Body",
                       geom_name="Body")
    for name, x, z in (("Wheel_FL", -TRACK_HALF, AXLE_FRONT_Z),
                       ("Wheel_FR", +TRACK_HALF, AXLE_FRONT_Z),
                       ("Wheel_RL", -TRACK_HALF, AXLE_REAR_Z),
                       ("Wheel_RR", +TRACK_HALF, AXLE_REAR_Z)):
        scene.add_geometry(_wheel((x, WHEEL_R, z)), node_name=name,
                           geom_name=name)
    return scene


def export(path: Path) -> bytes:
    data = trimesh.exchange.gltf.export_glb(build_scene(), include_normals=True)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return data


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="tools/fixtures/vehicle_probe.glb")
    ap.add_argument("--check", action="store_true",
                    help="build twice and assert the bytes match")
    a = ap.parse_args()

    out = Path(a.out)
    data = export(out)
    digest = hashlib.sha256(data).hexdigest()
    print("wrote %s  %d bytes  sha256 %s" % (out, len(data), digest[:16]))

    scene = build_scene()
    print("parts: %s" % ", ".join(sorted(scene.geometry)))
    for name in sorted(scene.geometry):
        g = scene.geometry[name]
        e = g.bounding_box.extents
        print("  %-9s %6d tri  extents %.2f x %.2f x %.2f  volume %.4f m3  "
              "convex %s" % (name, len(g.faces), e[0], e[1], e[2],
                             g.volume, g.is_convex))

    if a.check:
        again = trimesh.exchange.gltf.export_glb(build_scene(),
                                                 include_normals=True)
        same = hashlib.sha256(again).hexdigest() == digest
        print("deterministic: %s" % same)
        return 0 if same else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
