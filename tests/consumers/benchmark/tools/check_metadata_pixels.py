#!/usr/bin/env python3
"""Verify warmed renderer metadata after a real opaque-to-blend scene edit."""
import argparse
import json
from pathlib import Path
import sys
import uuid

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from tools.benchmark_scene import write_scene
from tools.benchmark_suite import run_visual_witness, visual_delta_files, sha256_file


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--backend", default="d3d11")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    directory = args.out.resolve() / uuid.uuid4().hex[:12]
    directory.mkdir(parents=True, exist_ok=False)
    scene = write_scene(directory / "fixture.scene.json", "drawcall", 1000)
    inputs = {"JCE_SCENE": str(scene.resolve()), "JCE_BENCH_AUTOSPAWN": "0,1000",
              "JCE_BACKEND": args.backend, "JCE_WINDOW_HIDDEN": "1"}
    results = {}
    for name in ("unchanged", "cached", "uncached"):
        target = directory / name
        target.mkdir()
        env = dict(inputs)
        if name != "unchanged":
            env["JCE_BENCH_MUTATION"] = "blend"
        if name == "uncached":
            env["JCE_DISABLE_ECULL_METADATA"] = "1"
        run_visual_witness(env, target, exe=args.exe.resolve(), timeout=600,
                           expected_workload=(0, 1000), host="player")
        log = (target / "visual.log").read_text(encoding="utf-8")
        if name != "unchanged" and log.count(
                "benchmark-mutation: mode=blend frame=18 changed=1000") != 1:
            raise ValueError("scene edit did not affect the requested mesh population")
        results[name] = {"image": str(target / "frame.png"), "environment": env,
                         "sha256": sha256_file(target / "frame.png")}
    changed = visual_delta_files(results["cached"]["image"], results["unchanged"]["image"])
    from PIL import Image
    import numpy as np
    with Image.open(results["cached"]["image"]) as a, Image.open(results["uncached"]["image"]) as b:
        delta = np.abs(np.asarray(a.convert("RGB"), dtype=np.int16) -
                       np.asarray(b.convert("RGB"), dtype=np.int16))
    difference = float(delta.mean())
    maximum = int(delta.max())
    passed = changed >= 0.02 and difference <= 0.01 and maximum <= 2
    result = {"status": "PASS" if passed else "FAIL", "results": results,
              "executable_sha256": sha256_file(args.exe), "backend_requested": args.backend,
              "changed_fraction": changed, "cache_mean_error": difference, "cache_max_error": maximum,
              "scope": "frame-20 scene edit witness, not continuous fidelity"}
    (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"directory": str(directory), **{k: result[k] for k in
                     ("status", "changed_fraction", "cache_mean_error", "cache_max_error")}}, indent=2))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
