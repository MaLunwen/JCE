#!/usr/bin/env python3
"""Retain a public shadow debug-view observation; never pixel acceptance."""
import argparse
from pathlib import Path
import re

from gbuffer_asset_suite import image, sha256_file, write
from tools.benchmark_suite import source_identity
from tools.jce_determinism import assert_backend
from tools.perf_bench import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--scene", type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "vulkan", "gl"), required=True)
    parser.add_argument("--view", choices=("mask", "cascade", "depth"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.scene = args.scene.resolve()
    args.exe = args.exe.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    exe_hash, scene_hash = sha256_file(args.exe), sha256_file(args.scene)
    env = {"JCE_BACKEND": args.backend, "JCE_GPU_TIER": "high",
        "JCE_SCENE": str(args.scene), "JCE_BENCH_SHADOW_VIEW": args.view,
        "JCE_PRIM_INSTANCE": "0", "JCE_BENCH_ASSET_STILL": "1",
        "JCE_BENCH_SHOT_FRAME": "50",
        "JCE_BENCH_PLAYER_SHOT": str(args.out / "target.png"),
        "JCE_DBG_CSM_STATS": "1",
        "JCE_DBG_CSM_LOG": str(args.out / "csm_uniforms.log"),
        "JCE_DBG_SHPROF": "1", "JCE_DISABLE_SHADOWCACHE": "1"}
    result = {"status": "INCOMPLETE", "scope": "Debug view observation only; no shadow correctness or performance acceptance",
        "executable": str(args.exe), "executable_sha256": exe_hash,
        "scene": str(args.scene), "scene_sha256": scene_hash,
        "source": source_identity(), "backend": args.backend, "view": args.view,
        "environment": env}
    write(args.out / "observation.json", result)
    try:
        process = run(env, args.out / "engine.log", frames=200, exe=args.exe)
        result["process"] = process
        log = (args.out / "engine.log").read_text(encoding="utf-8")
        assert_backend(process.get("stdout", "") + "\n" + log, args.backend)
        if "] ERROR -" in log:
            raise ValueError("consumer error in debug capture")
        if "benchmark-shadow-view: frame=50 mode=" + args.view not in log:
            raise ValueError("requested debug view not observed at capture frame")
        inventory = re.search(r"benchmark-shadow-targets: frame=50 count=(\d+)", log)
        targets = re.findall(r"benchmark-shadow-target: frame=50 index=(\d+) name=(\S+) width=(\d+) height=(\d+) texture=(\d+) kind=(\d+)", log)
        if not inventory or len(targets) != int(inventory.group(1)):
            raise ValueError("missing or incomplete actual target inventory")
        if image(args.out / "target.png").shape != (720, 1280, 4):
            raise ValueError("unexpected debug capture dimensions")
        if sha256_file(args.exe) != exe_hash or sha256_file(args.scene) != scene_hash:
            raise ValueError("source scene or executable changed during observation")
        result["targets"] = [{"index": int(i), "name": name, "width": int(w),
            "height": int(h), "texture": int(texture), "kind": int(kind)}
            for i, name, w, h, texture, kind in targets]
        retained = ["engine.log", "target.png"]
        if (args.out / "csm_uniforms.log").is_file():
            retained.append("csm_uniforms.log")
        result["files"] = {name: {"path": str(args.out / name),
            "sha256": sha256_file(args.out / name)} for name in retained}
        result["status"] = "OBSERVATION_ONLY"
    except Exception as error:
        result.update(status="FAIL", error=str(error))
        raise
    finally:
        write(args.out / "observation.json", result)


if __name__ == "__main__":
    main()
