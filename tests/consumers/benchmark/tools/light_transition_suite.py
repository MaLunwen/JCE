#!/usr/bin/env python3
"""Public SDK warm draw-cache lighting transitions with retained controls."""
import argparse
import re
from pathlib import Path

from gbuffer_asset_suite import difference, image, sha256_file, write
from light_suite import COUNT, responses, scene
from tools.benchmark_suite import source_identity
from tools.perf_bench import run

FRAMES = (29, 31, 71, 121)
MODES = ("off-on", "on-off", "off-on-off")


def expected(mode, frame):
    if mode not in MODES:
        raise ValueError("unknown transition")
    state = int(mode == "on-off") if frame < 30 else int(mode != "on-off")
    return 0 if mode == "off-on-off" and frame >= 70 else state


def validate_log(log, mode, cache):
    if "] ERROR -" in log or "HIT MISMATCH" in log or "VERIFY MISMATCH" in log:
        raise ValueError("consumer error or stale draw command")
    if mode:
        markers = re.findall(r"benchmark-fp-transition: frame=(\d+) mode=(\S+) "
            r"expected=(\d+) actual=(\d+) changed=(\d+)", log)
        seen = {}
        for frame, logged_mode, target, actual, changed in markers:
            frame = int(frame)
            if frame in seen or logged_mode != mode:
                raise ValueError("duplicate or wrong transition marker")
            target_state = expected(mode, frame)
            edge = frame in ((1, 30, 70) if mode == "off-on-off" else (1, 30))
            if (int(target), int(actual), int(changed)) != (target_state, target_state, int(edge)):
                raise ValueError("transition authority marker differs")
            seen[frame] = target_state
        if not set(range(1, 122)).issubset(seen):
            raise ValueError("missing pre/post transition frame evidence")
    hits = [int(value) for value in re.findall(r"drawcmd-cache: (\d+) hits /", log)]
    if cache and (len(hits) < 2 or not any(hits)):
        raise ValueError("cache did not demonstrate warm hits")
    if not cache and hits:
        raise ValueError("disabled cache still reported activity")
    return {"cache_reports": len(hits), "positive_hit_reports": sum(v > 0 for v in hits)}


def validate_baseline(pixels, zero, state):
    values = responses(pixels, zero)
    lit = [i for i, value in enumerate(values) if value >= 8]
    required = COUNT if state else 16
    if len(lit) != required or any(abs(value) > 2 for i, value in enumerate(values) if i not in lit):
        raise ValueError("steady lighting lacks expected %u-patch coverage" % required)
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "vulkan", "gl"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    identity = source_identity()
    binary = sha256_file(args.exe)
    result = {"status": "INCOMPLETE", "source": identity, "executable": str(args.exe.resolve()),
        "executable_sha256": binary, "backend": args.backend, "cases": {},
        "scope": "Static point-light patches; same-scene public cvar transitions, not performance certification"}
    write(args.out / "suite.json", result)

    def capture(label, state, cache, shot=121, mode=None, zero=False):
        directory = args.out / label
        directory.mkdir(parents=True)
        write(directory / "scene.json", scene("point", () if zero else range(COUNT)))
        env = {"JCE_BACKEND": args.backend, "JCE_FORWARDPLUS": str(state),
            "JCE_SCENE": str(directory / "scene.json"), "JCE_GPU_TIER": "high",
            "JCE_PRIM_INSTANCE": "0", "JCE_TEX_INSTANCE": "0", "JCE_PARALLEL_GATHER": "0",
            "JCE_DRAWCMD_CACHE": str(cache), "JCE_DRAWCMD_VERIFY": "1",
            "JCE_BENCH_ASSET_STILL": "1", "JCE_BENCH_SHOT_FRAME": str(shot),
            "JCE_BENCH_PLAYER_SHOT": str(directory / "target.png"),
            "JCE_BENCH_FP_TRANSITION": mode or ""}
        record = {"environment": env, "status": "INCOMPLETE"}
        result["cases"][label] = record
        write(args.out / "suite.json", result)
        try:
            record["process"] = run(env, directory / "engine.log", frames=200, exe=args.exe)
            record["log_validation"] = validate_log(
                (directory / "engine.log").read_text(encoding="utf-8"), mode, cache)
            if sha256_file(args.exe) != binary:
                raise ValueError("consumer executable changed during witness")
            pixels = image(directory / "target.png")
            record["status"] = "CAPTURED"
            return pixels
        except Exception as error:
            record.update(status="FAIL", error=str(error))
            raise
        finally:
            record["files"] = {name: {"path": str(directory / name),
                "sha256": sha256_file(directory / name)} for name in
                ("scene.json", "engine.log", "target.png") if (directory / name).exists()}
            write(args.out / "suite.json", result)

    try:
        references = {}
        for cache in (0, 1):
            for state in (0, 1):
                zero = capture("cache%u-zero%u" % (cache, state), state, cache, zero=True)
                pixels = capture("cache%u-steady%u" % (cache, state), state, cache)
                result.setdefault("coverage", {})["cache%u-steady%u" % (cache, state)] = validate_baseline(pixels, zero, state)
                references[cache, state] = pixels
        for state in (0, 1):
            if difference(references[0, state], references[1, state]) > .0001:
                raise ValueError("cache changes steady baseline")
        for cache in (0, 1):
            for mode in MODES:
                for frame in FRAMES:
                    label = "cache%u-%s-frame%u" % (cache, mode, frame)
                    pixels = capture(label, int(mode == "on-off"), cache, frame, mode)
                    delta = difference(pixels, references[cache, expected(mode, frame)])
                    result["cases"][label]["reference_difference"] = delta
                    if delta > .0001:
                        raise ValueError("transition differs from steady target: " + label)
        result["status"] = "WARM_LIGHT_TRANSITIONS_PASS"
    except Exception as error:
        result.update(status="FAIL", error=str(error))
        raise
    finally:
        result["source_at_end"] = source_identity()
        result["source_changed"] = result["source_at_end"] != identity
        write(args.out / "suite.json", result)


if __name__ == "__main__":
    main()
