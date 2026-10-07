#!/usr/bin/env python3
"""Imported skinned GLB liveness with frozen, empty and repeat controls.

The supplied file stays unchanged. This project owns framing and clip policy;
the renderer loads, animates, uploads and draws through ordinary SDK components.
These images prove a visible change, not independently correct skin matrices.
"""
import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from tools.benchmark_suite import (run_visual_witness, sha256_file,
                                   source_identity)
from tools.jce_determinism import read_png

MARKER = re.compile(
    r"benchmark-asset: frame=(\d+) meshes=(\d+) animators=(\d+) vertices=(\d+) "
    r"primitives=(\d+) joints=(\d+) clips=(\d+) player=(\d+) time=([\d.]+) gpu_bytes=(\d+)")
POPULATION = re.compile(r"benchmark-asset-population: frame=(\d+) advanced=(\d+) "
                        r"min=([\d.]+) max=([\d.]+) layers=(\d+) layer_sum=(\d+)")


def transform(x=0, y=0, z=0, scale=1, yaw=0):
    return {"type": "Transform", "posX": x, "posY": y, "posZ": z,
            "rotX": 0, "rotY": yaw, "rotZ": 0,
            "scaleX": scale, "scaleY": scale, "scaleZ": scale}


def fixture(asset, count, clip, speed, scale, layers=(0,), light_mask=0):
    side = math.ceil(math.sqrt(max(count, 1)))
    camera = transform(0, 0.5, 3)
    lens = {"type": "Camera", "primary": True, "fov": 60, "nearClip": 0.1,
            "farClip": 1000, "clearMode": 1}
    if count > 1:
        camera.update(posY=side * 2 + 0.5, posZ=side * 2, rotX=-45)
        lens.update(orthographic=True, orthoSize=side * 3.5)
    entities = [{"id": 1, "name": "Camera", "components": [
        camera, lens]},
        {"id": 2, "name": "Sun", "components": [transform(),
         {"type": "DirectionalLight", "dirX": -0.5, "dirY": -1,
          "dirZ": -0.5, "intensity": 2, "lightLayerMask": light_mask}]}]
    for index in range(count):
        x = (index % side - (side - 1) / 2) * 3
        z = (index // side - (side - 1) / 2) * 3
        entities.append({"id": index + 3, "name": "Character%u" % index,
                         "layer": layers[index % len(layers)],
                         "components": [transform(x, 0, z, scale, 90),
                         {"type": "MeshRenderer", "meshPath": str(asset), "visible": True},
                         {"type": "SkeletalAnimator", "skeletonPath": str(asset),
                          "clipNames": [clip], "activeClip": 0, "loop": True,
                          "playing": False, "speed": 1}]})
    return {"entities": entities}


def validate_markers(log, count, vertices, joints, clips, moving):
    rows = [tuple(float(v) if i == 8 else int(v) for i, v in enumerate(row))
            for row in MARKER.findall(log)]
    if len(rows) != 2 or [row[0] for row in rows] != [20, 50]:
        raise ValueError("live imported model samples are missing or duplicated")
    for row in rows:
        if row[1:4] != (count, count, vertices) or row[5:8] != (joints, clips, 1) \
                or row[4] < 1 or row[9] <= 0 or not math.isfinite(row[8]):
            raise ValueError("actual imported model population/topology/player differs")
    advanced = abs(rows[1][8] - rows[0][8]) > 0.01
    if advanced != moving:
        raise ValueError("live animation clock does not match the moving/frozen control")
    return rows


def validate_population(log, count, layers, moving):
    rows = [tuple(float(v) if i in (2, 3) else int(v) for i, v in enumerate(row))
            for row in POPULATION.findall(log)]
    actual_layers = [layers[i % len(layers)] for i in range(count)]
    mask = sum(1 << layer for layer in set(actual_layers))
    if len(rows) != 2 or [row[0] for row in rows] != [20, 50]:
        raise ValueError("complete per-character clock samples are missing")
    for row in rows:
        if row[1] != (count if moving else 0) or row[4:] != (mask, sum(actual_layers)) \
                or not all(math.isfinite(v) and 0 <= v <= 1 for v in row[2:4]) \
                or abs(row[2] - row[3]) > 0.000002 or (not moving and row[3] != 0):
            raise ValueError("some character clocks or receiver layers differ from the fixture")
    return rows


def pixels_pass(differences):
    required = ("asset_coverage", "frozen_clock_difference", "moving_clock_difference", "repeat_noise")
    if any(not math.isfinite(differences.get(key, math.nan)) or differences[key] < 0
           for key in required):
        return False
    return differences["asset_coverage"] > 0.002 and \
        differences["frozen_clock_difference"] < 0.0001 and \
        differences["moving_clock_difference"] > 0.0001 and differences["repeat_noise"] < 0.0001


def validate_still_controls(log):
    marker = "benchmark-asset: still controls taa=0 ssao=0 ssr=0 motion_blur=0"
    if log.count(marker) != 1 or "taa resolve: on" in log:
        raise ValueError("static capture controls are absent or TAA remained active")
    if "anim instance cache full" in log or "animation instance allocation failed" in log:
        raise ValueError("the renderer dropped animation instances")


def image_pair_size(first, second):
    if first[:2] != second[:2] or min(first[2], second[2]) < 3:
        raise ValueError("color captures have incompatible dimensions")
    width, height = first[:2]
    if width < 1 or height < 1 or any(len(im[3]) != width * height * im[2]
                                      for im in (first, second)):
        raise ValueError("invalid capture byte count")
    return width, height


def full_pixel_difference(first, second, threshold=30):
    if not isinstance(threshold, int) or not 0 <= threshold <= 255:
        raise ValueError("RGB threshold must be an integer from 0 through 255")
    width, height = image_pair_size(first, second)
    ca, cb = first[2], second[2]
    a, b = first[3], second[3]
    changed = sum(max(abs(a[i * ca + c] - b[i * cb + c]) for c in range(3)) > threshold
                  for i in range(width * height))
    return changed / (width * height)


def cell_pixel_changes(first, second, count):
    width, height = image_pair_size(first, second)
    ca, cb = first[2], second[2]
    a, b = first[3], second[3]
    side = math.ceil(math.sqrt(count))
    # Independently project the authored -45 degree orthographic camera's
    # 3-metre cells. Each actor must fit its cell; overlap cannot certify it.
    step = height / (side * 3.5)
    changes = []
    for index in range(count):
        cx = width / 2 + (index % side - (side - 1) / 2) * 3 * step
        cy = height / 2 + (index // side - (side - 1) / 2) * 3 * math.sqrt(0.5) * step
        box = (int(cx - 1.5 * step), int(cy - 1.5 * math.sqrt(0.5) * step),
               int(cx + 1.5 * step), int(cy + 1.5 * math.sqrt(0.5) * step))
        if not 0 <= box[0] < box[2] <= width or not 0 <= box[1] < box[3] <= height:
            raise ValueError("a character cell is outside the captured viewport")
        changes.append(sum(max(abs(a[(y * width + x) * ca + c] -
                                   b[(y * width + x) * cb + c]) for c in range(3)) > 30
                           for y in range(box[1], box[3]) for x in range(box[0], box[2])))
    return changes


def cells_pass(changes, count):
    return len(changes) == count and all(isinstance(n, int) and n > 0 for n in changes)


def chromatic_fraction(image):
    width, height, channels, pixels = image
    if channels < 3 or width < 1 or height < 1 or len(pixels) != width * height * channels:
        raise ValueError("invalid color witness image")
    changed = sum(max(pixels[i:i + 3]) - min(pixels[i:i + 3]) > 30
                  for i in range(0, len(pixels), channels))
    return changed / (width * height)


def pack_fixture(packer, asset, directory):
    """Use the installed SDK packer without changing supplied asset bytes."""
    pak = directory / "fixture.pak"
    command = [str(packer.resolve()), "--resource-dir", str(asset.resolve().parent),
               "--pak-file", str(pak), "--header-file", str(directory / "fixture.h"),
               "--manifest-file", str(directory / "fixture.cmake"),
               "--json", str(directory / "asset-bom.json")]
    packed = subprocess.run(command, capture_output=True, text=True, timeout=120)
    (directory / "pack.log").write_text(packed.stdout + packed.stderr, encoding="utf-8", newline="\n")
    if packed.returncode or not pak.is_file():
        raise ValueError("standard SDK PAK packing/integrity check failed")
    return pak


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--asset", type=Path, required=True)
    parser.add_argument("--packer", type=Path, required=True)
    parser.add_argument("--clip", required=True)
    parser.add_argument("--vertices", type=int, required=True)
    parser.add_argument("--joints", type=int, required=True)
    parser.add_argument("--clips", type=int, required=True)
    parser.add_argument("--scale", type=float, required=True)
    parser.add_argument("--count", type=int, default=1)
    parser.add_argument("--layers", default="0", help="comma-separated receiver layers, cycled per character")
    parser.add_argument("--light-mask", type=lambda s: int(s, 0), default=0,
                        help="directional light mask; zero retains legacy all-layer lighting")
    parser.add_argument("--backend", default="d3d11")
    parser.add_argument("--crowd", choices=("default", "off"), default="default")
    parser.add_argument("--min-chroma", type=float, default=0,
                        help="explicit minimum colored pixel fraction for a colored asset")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        layers = tuple(int(value) for value in args.layers.split(","))
    except ValueError:
        parser.error("layers must be comma-separated integers")
    if not layers or any(not 0 <= value <= 31 for value in layers) or not 0 <= args.light_mask <= 0xffffffff:
        parser.error("receiver layers must be 0..31 and light mask must be uint32")
    if not args.asset.is_file() or not args.exe.is_file() or not args.packer.is_file() \
            or args.asset.suffix.lower() != ".glb" or not 1 <= args.count <= 1000 \
            or not math.isfinite(args.scale) or args.scale <= 0 \
            or not math.isfinite(args.min_chroma) or not 0 <= args.min_chroma <= 1 \
            or min(args.vertices, args.joints, args.clips) <= 0:
        parser.error("valid asset/executable, positive topology/scale and count 1..1000 required")
    directory = args.out.resolve() / uuid.uuid4().hex[:12]
    directory.mkdir(parents=True, exist_ok=False)
    pak = pack_fixture(args.packer, args.asset, directory)
    asset = args.asset.name
    results = {}
    for name, frame, speed, count in (("empty", 20, 0, 0), ("frozen20", 20, 0, args.count),
                                     ("frozen50", 50, 0, args.count), ("moving20", 20, 1, args.count),
                                     ("moving50", 50, 1, args.count), ("repeat50", 50, 1, args.count)):
        target = directory / name
        target.mkdir()
        scene = target / "scene.json"
        scene.write_text(json.dumps(fixture(asset, count, args.clip, speed, args.scale,
                                            layers, args.light_mask)) + "\n",
                         encoding="utf-8", newline="\n")
        env = {"JCE_SCENE": str(scene), "JCE_BACKEND": args.backend,
               "JCE_BENCH_SHOT_FRAME": str(frame), "JCE_BENCH_ASSET_PAK": str(pak),
               "JCE_BENCH_TEXT_CAPTURE": "1", "JCE_BENCH_ASSET_STILL": "1"}
        if count:
            env["JCE_BENCH_ASSET_PATH"] = asset
        if speed:
            env["JCE_BENCH_ASSET_PLAY"] = "1"
        if args.crowd == "off":
            env.update(JCE_CROWD_INSTANCE="0", JCE_CROWD_BINDPOSE="0")
        result = run_visual_witness(env, target, exe=args.exe.resolve(), timeout=600, host="player")
        log = (target / "visual.log").read_text(encoding="utf-8")
        validate_still_controls(log)
        rows = validate_markers(log,
                               count, args.vertices, args.joints, args.clips, bool(speed)) if count else []
        population = validate_population(log, count, layers, bool(speed)) if count else []
        if speed and (abs(rows[0][8] - 11 * 0.016666) > 0.00001 or
                      abs(rows[1][8] - 41 * 0.016666) > 0.00001):
            raise ValueError("animation time differs from the fixed frame-10 start")
        results[name] = {"image": str(target / "frame.png"), "environment": env,
                         "image_sha256": sha256_file(target / "frame.png"),
                         "scene_sha256": sha256_file(scene), "live_samples": rows,
                         "population_samples": population,
                         "backend_actual": result["backend_actual"]}
    def delta(a, b):
        return full_pixel_difference(read_png(results[b]["image"]), read_png(results[a]["image"]))
    differences = {"asset_coverage": delta("frozen20", "empty"),
                   "frozen_clock_difference": delta("frozen20", "frozen50"),
                   "moving_clock_difference": delta("moving20", "moving50"),
                   "repeat_noise": delta("moving50", "repeat50")}
    differences["chromatic_coverage"] = chromatic_fraction(read_png(results["moving50"]["image"]))
    passed = pixels_pass(differences) and differences["chromatic_coverage"] >= args.min_chroma
    cells = {}
    if args.count > 1:
        cells["coverage"] = cell_pixel_changes(read_png(results["frozen20"]["image"]),
                                               read_png(results["empty"]["image"]), args.count)
        cells["motion"] = cell_pixel_changes(read_png(results["moving20"]["image"]),
                                             read_png(results["moving50"]["image"]), args.count)
        passed = passed and all(cells_pass(values, args.count) for values in cells.values())
    output = {"status": "LIVENESS_PASS" if passed else "FAIL", "results": results,
              "differences": differences, "asset_sha256": sha256_file(args.asset),
              "pak_sha256": sha256_file(pak), "packer_sha256": sha256_file(args.packer),
              "minimum_chromatic_coverage": args.min_chroma,
              "pixel_metric": "full-image RGB pixels with maximum channel difference > 30",
              "receiver_layers": layers, "directional_light_mask": args.light_mask,
              "per_cell_changed_pixels": cells,
              "executable_sha256": sha256_file(args.exe), "source": source_identity(),
              "scope": "imported skeletal liveness and every component's published clock with TAA/SSAO/SSR/motion blur disabled; no independent GPU deformation oracle"}
    (directory / "result.json").write_text(json.dumps(output, indent=2) + "\n",
                                           encoding="utf-8", newline="\n")
    print(json.dumps({"directory": str(directory), "status": output["status"], **differences}, indent=2))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
