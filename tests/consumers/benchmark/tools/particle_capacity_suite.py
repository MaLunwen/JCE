#!/usr/bin/env python3
"""Opt-in actual GPU capacity/retirement/refill witnesses; no timing certification."""
import argparse
from pathlib import Path
import re

from particle_suite import particle_asset, scene, alive_records, component_population
from gbuffer_asset_suite import sha256_file, write
from tools.benchmark_suite import source_identity
from particle_process import particle_backend, run_particle_process

CASES = {
    "zero": (1, 64, 0, 0, 1000, 0),
    "saturated": (1, 64, 4096, 0, 1000, 64),
    "rounded65": (1, 65, 4096, 0, 1000, 128),
    "retired": (1, 64, 64, 0, .01, 0),
    "refill": (1, 64, 0, 4096, .01, 64),
    "isolated8": (8, 64, 4096, 0, 1000, 64),
}


def asset_for(label):
    emitters, capacity, burst, rate, lifetime, expected = CASES[label]
    asset = particle_asset(emitters)
    asset.update(maxParticles=capacity, emitBurst=burst, emitRate=rate,
                 lifetimeMin=lifetime, lifetimeMax=lifetime)
    return asset


def validate_counts(label, log):
    emitters, _, _, _, _, expected = CASES[label]
    records = alive_records(log, emitters)
    if any(row["alive"] != expected for row in records["records"]):
        raise ValueError("unexpected actual GPU population for " + label)
    if records["total_alive"] != emitters * expected:
        raise ValueError("unexpected aggregate GPU population")
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "vulkan", "gl"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    executable = args.exe.resolve()
    identity = sha256_file(executable)
    result = {"status": "INCOMPLETE", "source": source_identity(), "cases": {},
              "executable": str(executable), "executable_sha256": identity,
              "backend": args.backend, "scope": "Post-allocation frame50 exact GPU populations only; not CPU parity or timing"}
    write(args.out / "suite.json", result)
    try:
        for label, config in CASES.items():
            emitters, _, _, _, _, expected = config
            directory = args.out / label
            directory.mkdir()
            asset = directory / "fixture.particles.json"
            write(asset, asset_for(label))
            write(directory / "scene.json", scene(asset, emitters, True))
            env = {"JCE_BACKEND": args.backend, "JCE_GPU_TIER": "high",
                   "JCE_SCENE": str(directory / "scene.json"), "JCE_BENCH_PARTICLE_OBSERVE": "1",
                   "JCE_GPU_PARTICLES_FORCE": "1", "JCE_BENCH_ASSET_STILL": "1",
                   "JCE_BENCH_SHOT_FRAME": "50", "JCE_BENCH_PLAYER_SHOT": str(directory / "target.png"),
                   "JCE_BENCH_PARTICLE_GPU_STATS": "1"}
            process = run_particle_process(env, directory / "engine.log", frames=200, exe=executable,
                                           samples_path=directory / "frames.csv")
            log = (directory / "engine.log").read_text(encoding="utf-8")
            files = {name: {"path": str(directory / name), "sha256": sha256_file(directory / name)}
                     for name in ("fixture.particles.json", "scene.json", "engine.log", "target.png", "frames.csv")
                     if (directory / name).is_file()}
            result["cases"][label] = {"process": process, "environment": env, "files": files,
                                      "expected_per_emitter": expected}
            write(args.out / "suite.json", result)
            particle_backend(log, args.backend)
            component_population(log, emitters, False)
            if "target.png" not in files:
                raise ValueError("missing diagnostic capture")
            result["cases"][label]["actual_gpu_alive"] = validate_counts(label, log)
            if sha256_file(executable) != identity:
                raise ValueError("executable changed")
            write(args.out / "suite.json", result)
        result["status"] = "GPU_CAPACITY_CONTROLS_PASS"
    except Exception as error:
        result.update(status="FAIL", error=str(error))
        raise
    finally:
        result["source_at_end"] = source_identity()
        result["source_changed"] = result["source"] != result["source_at_end"]
        write(args.out / "suite.json", result)


if __name__ == "__main__":
    main()
