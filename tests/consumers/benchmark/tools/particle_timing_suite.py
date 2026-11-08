#!/usr/bin/env python3
"""Bounded burst frame timing after independent correctness and diagnostic cutoff."""
import argparse
import json
import re
from pathlib import Path
from particle_suite import particle_asset, scene, alive_records, validate, validate_alive_controls, component_population, validate_burst_records
from gbuffer_asset_suite import image, sha256_file, write
from tools.benchmark_suite import source_identity, read_frames, frame_summary
from tools.jce_determinism import assert_backend, MARKER
from particle_process import particle_backend, run_particle_process, validate_particle_execution
from tools.perf_bench import read_execution_log



def validate_process(metrics, backend):
    """The guarded consumer runner preserves strict nested OS completion."""
    process = metrics.get("process")
    if not isinstance(process, dict) or type(process.get("returncode")) is not int \
            or process["returncode"] != 0 or process.get("timed_out") is not False:
        raise ValueError("correctness process failed or completion unavailable")
    actual = metrics.get("backend_actual")
    if not isinstance(actual, str) or not actual or "\n" in actual or "\r" in actual:
        raise ValueError("actual process backend unavailable")
    try:
        assert_backend(MARKER + actual, backend)
    except RuntimeError as error:
        raise ValueError("actual process backend mismatch") from error


def timing_records(log, emitters, cpu, count):
    if "] ERROR -" in log or "CPU particle upload unavailable" in log:
        raise ValueError("runtime error")
    actual = alive_records(log, emitters, cpu)
    validate_burst_records(actual, emitters, count, True, cpu)
    disabled = re.findall(r"benchmark-particle-stats-disabled: frame=(\d+) requested=(\d+) completed=(\d+)", log)
    if len(disabled) != 1:
        raise ValueError("diagnostic cutoff absent or duplicate")
    frame, requested, completed = map(int, disabled[0])
    expected = 2 if cpu else emitters
    if not 64 <= frame < 120 or requested != expected or completed != expected:
        raise ValueError("late/incomplete diagnostic cutoff")
    if any(row["completed_frame"] > frame for row in actual["records"]):
        raise ValueError("readback after cutoff")
    return {"actual": actual, "disabled_frame": frame}


def correctness(path, exe_hash, backend, route, count, size, emitters):
    data = json.loads(path.read_text(encoding="utf-8"))
    expected_status = "CPU_ALIVE_CONTROLS_PASS" if route == "cpu" else "PARTICLE_GPU_ALIVE_CONTROLS_PASS"
    if (data.get("status") != expected_status or data.get("executable_sha256") != exe_hash
        or sha256_file(Path(data["executable"])) != exe_hash or data.get("backend") != backend
        or data.get("path") != route or data.get("particles") != count
        or data.get("size") != size or data.get("emission_mode") != "burst"):
        raise ValueError("correctness prerequisite identity mismatch")
    pixels, counts = {}, {}
    for label in ("zero", "active", "changed", "repeat"):
        case = data["cases"][str(emitters)+"-"+label]
        for record in case["files"].values():
            if sha256_file(Path(record["path"])) != record["sha256"]:
                raise ValueError("correctness input changed")
        asset_path = Path(case["files"]["fixture.particles.json"]["path"])
        if json.loads(asset_path.read_text(encoding="utf-8")) != particle_asset(
                emitters, label != "zero", label == "changed", "burst", count, size):
            raise ValueError("correctness particle fixture mismatch")
        if json.loads(Path(case["files"]["scene.json"]["path"]).read_text(encoding="utf-8")) != scene(
                asset_path, emitters, route != "cpu"):
            raise ValueError("correctness scene mismatch")
        validate_process(case["process"], backend)
        captured = dict(case["process"]["process"])
        for stream in ("stdout", "stderr"):
            if stream in case["process"]:
                captured[stream] = case["process"][stream]
        log_path = Path(case["files"]["engine.log"]["path"])
        validate_particle_execution(captured, log_path,
            Path(case["files"]["frames.csv"]["path"]), 200, backend)
        log = read_execution_log(log_path, captured)
        component_population(log, emitters, route == "cpu")
        actual = alive_records(log, emitters, route == "cpu")
        validate_burst_records(actual, emitters, count, label != "zero", route == "cpu")
        counts[label] = actual["total_alive"]
        pixels[label] = image(Path(case["files"]["target.png"]["path"]))
    validate(pixels)
    validate_alive_controls(counts, "burst", count)
    return {"path": str(path.resolve()), "sha256": sha256_file(path)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "vulkan", "gl"), required=True)
    parser.add_argument("--path", choices=("cpu", "gpu-experimental"), required=True)
    parser.add_argument("--count", type=int, required=True)
    parser.add_argument("--emitters", type=int, choices=(1, 8), default=1)
    parser.add_argument("--size", type=float, default=.8)
    parser.add_argument("--repeats", type=int, choices=(3, 7), default=3)
    parser.add_argument("--correctness", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not 0 < args.count <= 1048576 or args.count % args.emitters or not 0 < args.size <= 100:
        parser.error("bounded divisible count and finite size required")
    args.exe = args.exe.resolve()
    binary_hash = sha256_file(args.exe)
    prerequisite = correctness(args.correctness, binary_hash, args.backend, args.path,
                               args.count, args.size, args.emitters)
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    result = {"status": "INCOMPLETE", "source": source_identity(),
        "executable": str(args.exe), "executable_sha256": binary_hash,
        "backend": args.backend, "path": args.path, "count": args.count,
        "emitters": args.emitters, "correctness": prerequisite, "runs": [],
        "scope": "120 warm-up, 600 measured frames; diagnostic cutoff before warm-up ends. "
                 "Burst lifetime1000s; not sustained 1800s endurance or cross-path equivalence."}
    try:
        for repeat in range(args.repeats):
            directory = args.out / ("repeat-%02d" % repeat)
            directory.mkdir()
            asset = directory / "fixture.particles.json"
            write(asset, particle_asset(args.emitters, mode="burst", particles=args.count, size=args.size))
            write(directory / "scene.json", scene(asset, args.emitters, args.path != "cpu"))
            env = {"JCE_BACKEND": args.backend, "JCE_GPU_TIER": "high",
                "JCE_SCENE": str(directory / "scene.json"), "JCE_BENCH_PARTICLE_OBSERVE": "1",
                "JCE_GPU_PARTICLES_FORCE": "0" if args.path == "cpu" else "1",
                "JCE_BENCH_ASSET_STILL": "1", "JCE_BENCH_PLAY_FRAME": "1",
                "JCE_BENCH_PARTICLE_STATS": "1", "JCE_BENCH_PARTICLE_STATS_UNTIL_FRAME": "64"}
            process = run_particle_process(env, directory / "engine.log", frames=721, exe=args.exe,
                          samples_path=directory / "frames.csv", sample_interval=.02)
            validate_process(process, args.backend)
            log = (directory / "engine.log").read_text(encoding="utf-8")
            particle_backend(log, args.backend)
            component_population(log, args.emitters, args.path == "cpu")
            witness = timing_records(log, args.emitters, args.path == "cpu", args.count)
            if sha256_file(args.exe) != binary_hash:
                raise ValueError("executable changed")
            result["runs"].append({"environment": env, "process": process, "witness": witness,
                "summary": frame_summary(read_frames(directory / "frames.csv", warmup=120, sample_count=600)),
                "files": {name: {"path": str(directory / name), "sha256": sha256_file(directory / name)}
                    for name in ("fixture.particles.json", "scene.json", "engine.log", "frames.csv")}})
            write(args.out / "suite.json", result)
        result["status"] = "BOUNDED_PARTICLE_TIMING_PASS"
    except Exception as error:
        result.update(status="FAIL", error=str(error))
        raise
    finally:
        write(args.out / "suite.json", result)


if __name__ == "__main__":
    main()
