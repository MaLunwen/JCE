#!/usr/bin/env python3
"""Requested equal emission budgets; rendered diagnostics, not R11 acceptance.

Opt-in GPU routing retains typed asynchronous actual alive-count evidence.
CPU totals use the same public typed readback. Equal counts do not prove full
CPU/GPU behavior equality. Constant ranges
remove stochastic velocity/lifetime inputs; observer-on timings are correctness diagnostics, not throughput certification.
"""
import argparse
from pathlib import Path
import re

from gbuffer_asset_suite import difference, image, sha256_file, write
from postfx_fixture import fixture
from tools.benchmark_suite import source_identity
from particle_process import particle_backend, run_particle_process
from tools.benchmark_suite import read_frames, frame_summary


def particle_asset(emitters, active=True, changed=False, mode="steady", particles=256, size=.8):
    if emitters not in (1, 8):
        raise ValueError("unknown emitter pattern")
    if mode not in ("steady", "burst"):
        raise ValueError("unknown emission mode")
    color = [.05, .05, .3, .2] if changed else [.3, .05, .05, .2]
    asset = {"maxParticles": 1024 // emitters, "emitRate": 256 / emitters if active else 0,
        "lifetimeMin": 4, "lifetimeMax": 4, "velocityMin": [0, 0, 0],
        "velocityMax": [0, 0, 0], "gravity": [0, 0, 0], "sizeStart": size,
        "sizeEnd": size, "colorStart": color, "colorEnd": color, "worldSpace": True}
    if mode == "burst":
        asset.update(maxParticles=max(1024, 1 << (particles-1).bit_length()) // emitters,
                     emitRate=0, emitBurst=particles // emitters if active else 0,
                     lifetimeMin=1000, lifetimeMax=1000)
    return asset


def scene(asset, emitters, gpu=False):
    result = fixture(empty=True)
    result["entities"][0]["components"][0].update(posX=0, posY=0, posZ=12,
        rotX=0, rotY=0, rotZ=0)
    result["entities"][0]["components"][1].update(orthographic=True, orthoSize=12)
    result["entities"][1]["components"][1].update(intensity=0, castsShadow=False)
    result["rendering"]["postfx"].update(ssao=False, ssr=False, bloom=False)
    for index in range(emitters):
        result["entities"].append({"id": index+3, "name": "Emitter%u" % index,
            "components": [{"type": "Transform", "posX": (index-(emitters-1)/2)*1.2,
                "posY": 0, "posZ": 0, "scaleX": 1, "scaleY": 1, "scaleZ": 1},
                {"type": "ParticleEmitter", "assetPath": str(asset), "gpu": gpu}]})
    return result


def validate(images):
    if set(images) != {"zero", "active", "changed", "repeat"}:
        raise ValueError("missing particle control")
    if any(value.shape != (720, 1280, 4) for value in images.values()):
        raise ValueError("unexpected particle dimensions")
    metrics = {"emission_delta": difference(images["zero"], images["active"]),
        "material_delta": difference(images["active"], images["changed"]),
        "repeat_delta": difference(images["active"], images["repeat"])}
    if metrics["emission_delta"] <= .0001 or metrics["material_delta"] <= .0001 \
            or metrics["repeat_delta"] > .0001:
        raise ValueError("particle liveness/repeat/content control failed")
    # The authored fixture keeps exposure fixed and emitters away from corners.
    # Typed alive/drawn totals alone cannot detect a skipped postfx pass.
    background = {}
    for name in ("active", "changed", "repeat"):
        delta = max(abs(int(images[name][y, x, channel]) -
                        int(images["zero"][y, x, channel]))
                    for y, x in ((0, 0), (0, -1), (-1, 0), (-1, -1))
                    for channel in range(3))
        background[name] = delta
        if delta > 2:
            raise ValueError("particle fixture background/postfx control changed")
    metrics["background_corner_max_rgb8_delta"] = background
    return metrics


def alive_records(log, emitters, cpu=False):
    expected_targets = 2 if cpu else emitters
    name_pattern = r"particles\.cpu_(?:alive|drawn)_total" if cpu else r"particles\.gpu_alive_\d+"
    counts = re.findall(r"benchmark-particle-requests: source_frame=50 count=(\d+)", log)
    rows = re.findall(r"benchmark-particle-alive: source_frame=50 completed_frame=(\d+) ready_engine_frame=(\d+) index=(\d+) name=(" + name_pattern + r") rgba=([0-9a-f]{8}) alive=(\d+)", log)
    if log.count("benchmark-particle-requests:") != 1 or \
            log.count("benchmark-particle-alive:") != expected_targets or \
            len(counts) != 1 or int(counts[0]) != expected_targets or len(rows) != expected_targets:
        raise ValueError("missing or incomplete actual particle counts")
    result = []
    for completed, ready, index, name, rgba, alive in rows:
        raw = bytes.fromhex(rgba)
        count = int.from_bytes(raw[:3], "little")
        if raw[3] != 255 or count != int(alive) or not 50 <= int(completed) < 200:
            raise ValueError("invalid typed particle count bytes/completion")
        result.append({"index": int(index), "name": name, "rgba": rgba,
            "alive": count, "completed_frame": int(completed), "ready_engine_frame": int(ready)})
    if sorted(row["index"] for row in result) != list(range(expected_targets)) or \
            len({row["name"] for row in result}) != expected_targets:
        raise ValueError("duplicate or missing emitter targets")
    if cpu:
        named = {row["name"]: row["alive"] for row in result}
        if set(named) != {"particles.cpu_alive_total", "particles.cpu_drawn_total"} or \
                named["particles.cpu_alive_total"] != named["particles.cpu_drawn_total"]:
            raise ValueError("CPU submitted population differs from simulation")
        return {"records": result, "total_alive": named["particles.cpu_alive_total"]}
    return {"records": result, "total_alive": sum(row["alive"] for row in result)}


def validate_burst_records(actual, emitters, particles, active, cpu):
    expected = particles if active else 0
    if actual["total_alive"] != expected:
        raise ValueError("burst actual population is not exact requested count")
    if not cpu and any(row["alive"] != expected // emitters for row in actual["records"]):
        raise ValueError("burst GPU per-emitter population differs from fixture")


def component_population(log, emitters, cpu):
    rows = re.findall(r"benchmark-particles: frame=50 components=(\d+) loaded=(\d+) cpu_handles=(\d+) gpu_requested=(\d+)", log)
    if log.count("benchmark-particles:") != 1 or len(rows) != 1 or int(rows[0][0]) != emitters:
        raise ValueError("particle component population unavailable or duplicated")
    marker = tuple(map(int, rows[0]))
    if marker[3] != (0 if cpu else emitters):
        raise ValueError("particle requested routing differs from fixture")
    if cpu and (marker[1] != emitters or marker[2] != emitters):
        raise ValueError("CPU particle components not ready")
    return marker


def validate_alive_controls(counts, mode="steady", particles=256):
    if mode not in ("steady", "burst"):
        raise ValueError("unknown emission mode")
    if set(counts) != {"zero", "active", "changed", "repeat"} or \
            counts["zero"] != 0 or counts["active"] <= 0 or \
            counts["active"] != counts["changed"] or counts["active"] != counts["repeat"]:
        raise ValueError("actual particle population controls failed")
    if mode == "burst" and counts["active"] != particles:
        raise ValueError("burst actual population is not exact requested count")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "vulkan", "gl"), required=True)
    parser.add_argument("--path", choices=("cpu", "gpu-experimental"), required=True)
    parser.add_argument("--emission-mode", choices=("steady", "burst"), default="steady")
    parser.add_argument("--particles", type=int, default=256)
    parser.add_argument("--size", type=float, default=.8)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.particles <= 0 or args.particles > 1048576 or args.particles % 8:
        parser.error("particles must be a positive multiple of8 <=1048576")
    if args.emission_mode != "burst" and args.particles != 256:
        parser.error("custom particle count requires burst mode")
    if not 0 < args.size <= 100:
        parser.error("size must be finite and in (0,100]")
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    binary_hash = sha256_file(args.exe)
    result = {"status": "INCOMPLETE", "source": source_identity(), "cases": {},
        "executable": str(args.exe.resolve()), "executable_sha256": binary_hash,
        "backend": args.backend, "path": args.path, "emission_mode": args.emission_mode, "particles":args.particles, "size":args.size, "requested_total_rate": 0 if args.emission_mode == "burst" else 256,
        "scope": "Typed actual alive-count controls for CPUtotal or GPU emitters; steady creation timing differs. "
                 "Cross-path equivalence, timing and complete R11 acceptance unverified"}
    write(args.out / "suite.json", result)
    try:
        for emitters in (1, 8):
            images = {}
            counts = {}
            for label in ("zero", "active", "changed", "repeat"):
                directory = args.out / (str(emitters) + "-" + label)
                directory.mkdir()
                asset = directory / "fixture.particles.json"
                write(asset, particle_asset(emitters, label != "zero", label == "changed", args.emission_mode, args.particles, args.size))
                write(directory / "scene.json", scene(asset, emitters, args.path != "cpu"))
                env = {"JCE_BACKEND": args.backend, "JCE_GPU_TIER": "high",
                    "JCE_SCENE": str(directory / "scene.json"), "JCE_BENCH_PARTICLE_OBSERVE": "1",
                    "JCE_GPU_PARTICLES_FORCE": "1" if args.path != "cpu" else "0",
                    "JCE_BENCH_ASSET_STILL": "1", "JCE_BENCH_SHOT_FRAME": "50",
                    "JCE_BENCH_PLAY_FRAME": "1", "JCE_BENCH_PARTICLE_STATS": "1",
                    "JCE_BENCH_PLAYER_SHOT": str(directory / "target.png")}
                process = run_particle_process(env, directory / "engine.log", frames=200, exe=args.exe.resolve(),
                              samples_path=directory / "frames.csv", sample_interval=.02)
                log = (directory / "engine.log").read_text(encoding="utf-8")
                particle_backend(log, args.backend)
                marker = component_population(log, emitters, args.path == "cpu")
                actual_counts = alive_records(log, emitters, args.path == "cpu")
                if args.emission_mode == "burst":
                    validate_burst_records(actual_counts, emitters, args.particles, label != "zero", args.path == "cpu")
                counts[label] = actual_counts["total_alive"]
                images[label] = image(directory / "target.png")
                if sha256_file(args.exe) != binary_hash:
                    raise ValueError("executable changed")
                result["cases"][directory.name] = {"process": process, "environment": env,
                    "component_marker": list(marker),
                    "timing_observer_on": frame_summary(read_frames(directory / "frames.csv", warmup=60, sample_count=None)), "actual_alive": actual_counts,
                    "files": {name: {"path": str(directory / name), "sha256": sha256_file(directory / name)}
                        for name in ("fixture.particles.json", "scene.json", "engine.log", "target.png", "frames.csv")}}
                write(args.out / "suite.json", result)
            result[str(emitters) + "-metrics"] = validate(images)
            validate_alive_controls(counts, args.emission_mode, args.particles)
            result[str(emitters) + "-actual-alive"] = counts
        result["status"] = "CPU_ALIVE_CONTROLS_PASS" if args.path == "cpu" else "PARTICLE_GPU_ALIVE_CONTROLS_PASS"
    except Exception as error:
        result.update(status="FAIL", error=str(error))
        raise
    finally:
        write(args.out / "suite.json", result)


if __name__ == "__main__":
    main()
