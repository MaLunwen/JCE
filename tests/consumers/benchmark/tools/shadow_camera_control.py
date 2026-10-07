#!/usr/bin/env python3
"""Asymmetric public-scene overhead-camera coordinate witness; no timings."""
import argparse
from pathlib import Path
import numpy as np
from shadow_suite import scene, WIDTH, HEIGHT
from gbuffer_asset_suite import image, sha256_file, write
from tools.perf_bench import run
from tools.jce_determinism import assert_backend

MARKERS = (("Red", -5, -7, 0), ("Green", 3, 2, 1), ("Blue", 7, -3, 2))


def fixture():
    result = scene("empty-off")
    result["rendering"]["shadows"]["distance"] = 0
    result["entities"] = result["entities"][:2]
    for name, x, z, channel in MARKERS:
        color = [0., 0., 0.]
        color[channel] = 1.
        result["entities"].append({"id": len(result["entities"])+1, "name": name,
            "components": [{"type": "Transform", "posX": x, "posY": 0, "posZ": z,
                "scaleX": 1, "scaleY": 1, "scaleZ": 1},
                {"type": "MeshRenderer", "meshShape": 0, "visible": True,
                 "baseColorR": color[0], "baseColorG": color[1], "baseColorB": color[2],
                 "emissiveR": color[0], "emissiveG": color[1], "emissiveB": color[2],
                 "roughness": 1, "metallic": 0}]})
    return result


def centers(pixels):
    if pixels.shape != (HEIGHT, WIDTH, 4):
        raise ValueError("unexpected capture dimensions")
    rows = []
    rgb = pixels[:, :, :3].astype(float)
    for name, x, z, channel in MARKERS:
        others = np.max(np.delete(rgb, channel, axis=2), axis=2)
        selected = (rgb[:,:,channel] > 40) & (rgb[:,:,channel] > others + 30)
        ys, xs = np.where(selected)
        if len(xs) < 100:
            raise ValueError("missing unique marker")
        rows.append({"marker": name, "world_x": x, "world_z": z,
            "observed_x": float(xs.mean()), "observed_y": float(ys.mean()),
            "canonical_expected_x": WIDTH/2 + x*HEIGHT/28-.5,
            "canonical_expected_y": HEIGHT/2 + z*HEIGHT/28-.5,
            "pixels": len(xs)})
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--backend", required=True, choices=("d3d11","d3d12","vulkan","gl"))
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    exe_hash = sha256_file(args.exe)
    write(args.out / "scene.json", fixture())
    env = {"JCE_BACKEND": args.backend, "JCE_GPU_TIER": "high", "JCE_LOG_CAMERA": "1",
        "JCE_SCENE": str(args.out / "scene.json"), "JCE_BENCH_ASSET_STILL": "1",
        "JCE_BENCH_SHOT_FRAME": "50", "JCE_BENCH_PLAYER_SHOT": str(args.out / "target.png")}
    process = run(env, args.out / "engine.log", frames=200, exe=args.exe.resolve())
    log = (args.out / "engine.log").read_text(encoding="utf-8")
    actual = assert_backend(log, args.backend)
    if process["process"].get("returncode") != 0 or process["process"].get("timed_out") or "] ERROR -" in log:
        raise ValueError("consumer failed")
    if sha256_file(args.exe) != exe_hash:
        raise ValueError("executable changed")
    write(args.out / "camera.json", {"scope": "coordinate witness only; no formal timing",
        "convention": "right-handed Euler -90 X rotates local up +Y to world -Z; top-left image coordinates",
        "executable": str(args.exe.resolve()), "executable_sha256": exe_hash,
        "backend_actual": actual, "environment": env, "process": process,
        "markers": centers(image(args.out / "target.png")),
        "files": {name: {"path": str(args.out / name), "sha256": sha256_file(args.out / name)}
            for name in ("scene.json", "engine.log", "target.png")}})


if __name__ == "__main__":
    main()
