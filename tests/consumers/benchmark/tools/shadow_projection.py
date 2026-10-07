#!/usr/bin/env python3
"""Independent geometric orthographic shadow diagnostic; no PASS threshold."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy.ndimage import binary_dilation, distance_transform_edt

from report_lights import checked
from report_shadows import load
from shadow_suite import CASES, HEIGHT, WIDTH, image, scene, write
from gbuffer_asset_suite import sha256_file
from tools.jce_determinism import assert_backend


def hull(points):
    points = sorted(set(map(tuple, points)))
    def cross(o, a, b):
        return (a[0]-o[0])*(b[1]-o[1])-(a[1]-o[1])*(b[0]-o[0])
    lower, upper = [], []
    for chain, sequence in ((lower, points), (upper, reversed(points))):
        for point in sequence:
            while len(chain) > 1 and cross(chain[-2], chain[-1], point) <= 0:
                chain.pop()
            chain.append(point)
    return lower[:-1] + upper[:-1]


def raster(points):
    canvas = Image.new("1", (WIDTH, HEIGHT))
    # Camera overhead, local up maps to world -Z. Pixel-center convention.
    projected = [(WIDTH/2 + x*HEIGHT/28 - .5,
                  HEIGHT/2 + z*HEIGHT/28 - .5) for x, z in hull(points)]
    ImageDraw.Draw(canvas).polygon(projected, fill=1)
    return np.asarray(canvas, dtype=bool)


def oracle(authored):
    camera = authored["entities"][0]["components"]
    if camera[0]["rotX"] != -90 or not camera[1]["orthographic"] or camera[1]["orthoSize"] != 28:
        raise ValueError("only original overhead orthographic camera supported")
    sun = authored["entities"][1]["components"][1]
    direction = np.array([sun["dirX"], sun["dirY"], sun["dirZ"]], dtype=float)
    if direction[1] >= 0:
        raise ValueError("sun must travel downward")
    footprint = np.zeros((HEIGHT, WIDTH), bool)
    expected = footprint.copy()
    for entity in authored["entities"][3:]:
        t = entity["components"][0]
        if any(t.get(k, 0) for k in ("rotX", "rotY", "rotZ")):
            raise ValueError("rotated caster outside this oracle scope")
        center = np.array([t["posX"], t["posY"], t["posZ"]])
        scale = np.array([t["scaleX"], t["scaleY"], t["scaleZ"]])
        projected, ground = [], []
        for x in (-.5, .5):
            for y in (-.5, .5):
                for z in (-.5, .5):
                    vertex = center + scale*np.array([x, y, z])
                    hit = vertex - direction*(vertex[1]/direction[1])
                    projected.append((hit[0], hit[2]))
                    ground.append((vertex[0], vertex[2]))
        expected |= raster(projected)
        footprint |= raster(ground)
    floor = raster([(-20,-14),(-20,14),(20,14),(20,-14)])
    domain = floor & ~footprint
    return expected & domain, footprint, domain


def analyze(expected, domain, off, on, tolerance):
    observed = (np.mean(off[:,:,:3].astype(float)-on[:,:,:3], axis=2) > 3) & domain
    expanded = binary_dilation(expected, iterations=tolerance) if tolerance else expected
    obs_expanded = binary_dilation(observed, iterations=tolerance) if tolerance else observed
    distances = distance_transform_edt(~observed) if observed.any() else None
    return {"expected_pixels": int(expected.sum()), "observed_pixels": int(observed.sum()),
        "expected_covered_fraction": float((expected & observed).sum()/max(1, expected.sum())),
        "covered_with_tolerance_fraction": float((expected & obs_expanded).sum()/max(1, expected.sum())),
        "false_positive_pixels": int((observed & ~expanded).sum()),
        "expected_to_observed_distance_p50_px": float(np.median(distances[expected])) if distances is not None else None}, observed


def evidence(path):
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("status") == "SHADOW_PIXEL_LIVENESS_PASS":
        return load(path)[0:3:2]
    # Historical failed suite is diagnostic evidence; do not upgrade its status.
    if value.get("status") != "FAIL" or set(value["cases"]) != set(CASES):
        raise ValueError("incomplete historical evidence")
    checked(Path(value["executable"]), value["executable_sha256"])
    pixels = {}
    for label, case in value["cases"].items():
        for item in case["files"].values():
            checked(Path(item["path"]), item["sha256"])
        log = Path(case["files"]["engine.log"]["path"]).read_text(encoding="utf-8")
        assert_backend(log, value["backend"])
        if case["process"]["process"].get("returncode") != 0 or "] ERROR -" in log:
            raise ValueError("historical consumer failed")
        retained = json.loads(Path(case["files"]["scene.json"]["path"]).read_text(encoding="utf-8"))
        if retained != scene(label):
            raise ValueError("historical scene outside original fixture")
        pixels[label] = image(Path(case["files"]["target.png"]["path"]))
    return value, pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--tolerance-px", type=int, default=2)
    args = parser.parse_args()
    if not 0 <= args.tolerance_px <= 16:
        raise ValueError("invalid explicit raster tolerance")
    results = []
    args.out.mkdir(parents=True, exist_ok=False)
    for index, path in enumerate(args.suite):
        value, pixels = evidence(path)
        authored = json.loads(Path(value["cases"]["on"]["files"]["scene.json"]["path"]).read_text(encoding="utf-8"))
        expected, footprint, domain = oracle(authored)
        metrics, observed = analyze(expected, domain, pixels["off"], pixels["on"], args.tolerance_px)
        rgb = np.zeros((HEIGHT, WIDTH, 3), np.uint8)
        rgb[expected] = (255,180,0)
        rgb[observed] = (0,180,255)
        rgb[expected & observed] = (80,255,80)
        name = "projection-%02u.png" % index
        Image.fromarray(rgb).save(args.out / name)
        results.append({"suite": str(path.resolve()), "suite_sha256": sha256_file(path),
            "status_original": value["status"], "backend": value["backend"],
            "executable_sha256": value["executable_sha256"], "metrics": metrics, "visual": name})
    write(args.out / "projection.json", {"scope": "diagnostic only; no acceptance threshold",
        "pixel_tolerance": args.tolerance_px, "world_metres_per_pixel": 28/HEIGHT,
        "raster_note": "pixel centers; polygon raster boundary uncertainty; footprints excluded",
        "legend": "amber expected; cyan observed; green overlap", "evidence": results})


if __name__ == "__main__":
    main()
