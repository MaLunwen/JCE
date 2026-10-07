#!/usr/bin/env python3
"""SDK normal-buffer controls using original unit Box and animated Fox GLBs.

This is pixel/clock liveness, not a timing or independent skin-matrix oracle.
Inputs remain unchanged; all generated scenes, packs and captures are local.
"""
import argparse
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from tools.benchmark_suite import sha256_file, source_identity
from tools.perf_bench import run
from check_asset_pixels import fixture as skin_fixture, pack_fixture, validate_markers
from postfx_fixture import fixture as room_fixture


def image(path):
    return np.asarray(Image.open(path).convert("RGBA"))


def coverage(pixels):
    if pixels.ndim != 3 or pixels.shape[2] != 4 or min(pixels.shape[:2]) < 1:
        raise ValueError("invalid normal capture dimensions")
    return float(np.mean(pixels[:, :, 3] < 254))


def difference(first, second):
    if first.shape != second.shape:
        raise ValueError("capture dimensions differ")
    delta = np.abs(first[:, :, :3].astype(np.int16) - second[:, :, :3].astype(np.int16))
    return float(np.mean(np.max(delta, axis=2) > 2))


def validate_box(primitive, imported):
    if coverage(primitive) <= .002 or coverage(imported) <= .002:
        raise ValueError("static normal output stayed matte-clear")
    if not np.array_equal(primitive, imported):
        raise ValueError("unit Box and primitive normal outputs differ")
    return {"normal_coverage": coverage(imported), "exact_rgba_match": True}


def validate_skin(images):
    for name in ("frozen20", "frozen50", "moving20", "moving50", "repeat50"):
        if coverage(images[name]) <= .002:
            raise ValueError("skinned normal output stayed matte-clear")
    metrics = {"empty_coverage": coverage(images["empty"]),
               "normal_coverage": coverage(images["moving50"]),
               "frozen_delta": difference(images["frozen20"], images["frozen50"]),
               "moving_delta": difference(images["moving20"], images["moving50"]),
               "repeat_delta": difference(images["moving50"], images["repeat50"])}
    if metrics["empty_coverage"] > .0001 or metrics["frozen_delta"] > .0001 \
            or metrics["repeat_delta"] > .0001 or metrics["moving_delta"] <= .0001:
        raise ValueError("normal animation, frozen, empty or repeat control failed")
    return metrics


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8", newline="\n")


def box_scene(asset=None):
    scene = room_fixture("ssr", empty=True)
    mesh = {"type": "MeshRenderer", "visible": True, "roughness": .3,
            "baseColorR": .4, "baseColorG": .5, "baseColorB": .6}
    mesh.update({"meshPath": asset} if asset else {"meshShape": 0})
    scene["entities"].append({"id": 3, "name": "NormalGeometry", "components": [
        {"type": "Transform", "posX": 0, "posY": 0, "posZ": 0,
         "scaleX": 4, "scaleY": 4, "scaleZ": 4}, mesh]})
    return scene


def capture(args, identity, backend, label, scene, pak, asset=None,
            moving=False, frame=50, skin=False):
    out = args.out / backend / label
    out.mkdir(parents=True)
    scene_path = out / "scene.json"
    write(scene_path, scene)
    env = {"JCE_BACKEND": backend, "JCE_SCENE": str(scene_path.resolve()),
           "JCE_BENCH_ASSET_PAK": str(pak.resolve()), "JCE_BENCH_ASSET_STILL": "1",
           "JCE_BENCH_RT_TARGET": "gbuffer.normal", "JCE_BENCH_SHOT_FRAME": str(frame),
           "JCE_BENCH_PLAYER_SHOT": str((out / "target.png").resolve()), "JCE_GPU_TIER": "high"}
    if asset:
        env.update(JCE_BENCH_ASSET_PATH=asset, JCE_BENCH_WAIT_ASSET="1")
    if moving:
        env["JCE_BENCH_ASSET_PLAY"] = "1"
    process = run(env, out / "engine.log", frames=1000 if asset else 181,
                  exe=args.exe.resolve())
    log = (out / "engine.log").read_text(encoding="utf-8")
    if "] ERROR -" in log or "skinned normal G-buffer shader unavailable" in log:
        raise ValueError("consumer error or missing shader")
    if "intermediate target: gbuffer.normal 1280x720" not in log:
        raise ValueError("requested live normal buffer is missing")
    markers = validate_markers(log, 1, 1728, 24, 3, moving) if skin and asset else []
    pixels = image(out / "target.png")
    if pixels.shape != (720, 1280, 4):
        raise ValueError("actual normal image dimensions differ")
    if sha256_file(args.exe) != identity["executable_sha256"]:
        raise ValueError("executable changed during the suite")
    result = {"process": process, "environment": env, "markers": markers,
              "normal_coverage": coverage(pixels),
              "files": {name: {"path": str((out / name).resolve()),
                               "sha256": sha256_file(out / name)}
                        for name in ("scene.json", "target.png", "engine.log")}}
    write(out / "result.json", result)
    print(backend, label, result["normal_coverage"], flush=True)
    return pixels, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("exe", "packer", "box", "fox", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--backend", action="append", choices=("d3d11", "d3d12", "vulkan", "gl"))
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    identity = {"executable": str(args.exe.resolve()), "executable_sha256": sha256_file(args.exe),
                "sources": {key: {"path": str(path.resolve()), "sha256": sha256_file(path)}
                            for key, path in (("box", args.box), ("fox", args.fox))},
                "source": source_identity(), "rgb_threshold": 2,
                "scope": "normal output and clock liveness; no timing or independent skin/material fidelity"}
    write(args.out / "identity.json", identity)
    packs = {}
    for key in ("box", "fox"):
        directory = args.out / (key + "-pack")
        directory.mkdir()
        packs[key] = pack_fixture(args.packer.resolve(), getattr(args, key).resolve(), directory)
    for backend in args.backend or ("d3d11", "d3d12", "vulkan", "gl"):
        images, cases = {}, {}
        for label, imported in (("primitive", False), ("box", True)):
            images[label], cases[label] = capture(args, identity, backend, label,
                box_scene(args.box.name if imported else None), packs["box"],
                asset=args.box.name if imported else None)
        box = validate_box(images["primitive"], images["box"])
        skin_images = {}
        for label, count, moving, frame in (("empty", 0, False, 20), ("frozen20", 1, False, 20),
                ("frozen50", 1, False, 50), ("moving20", 1, True, 20),
                ("moving50", 1, True, 50), ("repeat50", 1, True, 50)):
            scene = skin_fixture(args.fox.name, count, "Walk", 1, .01)
            scene["rendering"] = {"postfx": {"ssr": True, "ssao": False}}
            if count:
                scene["entities"][2]["components"][1]["roughness"] = .3
            skin_images[label], cases[label] = capture(args, identity, backend, label,
                scene, packs["fox"], asset=args.fox.name if count else None,
                moving=moving, frame=frame, skin=True)
        skin = validate_skin(skin_images)
        for key, path in (("box", args.box), ("fox", args.fox)):
            if sha256_file(path) != identity["sources"][key]["sha256"]:
                raise ValueError("original GLB changed during the suite")
        write(args.out / backend / "suite.json", {**identity, "backend": backend,
              "status": "GBUFFER_ASSET_CONTROLS_PASS", "box": box, "skin": skin, "cases": cases})
        print(backend, "GBUFFER_ASSET_CONTROLS_PASS", skin, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
