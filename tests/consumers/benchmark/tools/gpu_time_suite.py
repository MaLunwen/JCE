"""Whole SDK Player GPU ablations, requiring same-binary static pixel proof."""
import argparse
import json
from pathlib import Path
import statistics
import uuid

from gpu_model_suite import (front_cells, full_pixel_difference, player_counters,
                             read_png, sha256_file, source_identity, validate_gpu, validate_model, validate_policy)
from tools.benchmark_suite import frame_summary, host_render_environment, read_frames
from tools.perf_bench import run
from gpu_counter_evidence import read_survivors

MODES = ("cpu", "fixed", "gpu", "hiz")


def timing_environment(capture_environment, directory):
    env = dict(capture_environment)
    for key in ("JCE_BENCH_SHOT_FRAME", "JCE_BENCH_TEXT_CAPTURE",
                "JCE_BENCH_PLAYER_SHOT", "JCE_BENCH_GPU_SURVIVORS"):
        env.pop(key, None)
    env.update(host_render_environment("player", directory))
    return env


def verify_witness(path, exe):
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("status") != "STATIC_WITNESS_PASS" or \
            document.get("executable_sha256") != sha256_file(exe):
        raise ValueError("same-binary passing static witness required")
    root = path.parent
    for name, key in (("scene.json", "scene_sha256"), ("fixture.pak", "pak_sha256")):
        if sha256_file(root / name) != document[key]:
            raise ValueError("static witness scene or asset PAK changed")
    count, vertices = document["count"], document["vertices"]
    images = {}
    for label in ("empty", "cpu", "fixed", "gpu", "hiz", "repeat"):
        capture = document["captures"][label]
        image = Path(capture["image"])
        if sha256_file(image) != capture["image_sha256"]:
            raise ValueError("static witness capture was edited")
        images[label] = read_png(image)
        if label != "empty":
            mode = "hiz" if label == "repeat" else label
            log = (image.parent / "visual.log").read_text(encoding="utf-8")
            validate_model(log, count, vertices)
            validate_policy(log, mode)
            rows = player_counters((image.parent / "engine_counters.jsonl").read_text(encoding="utf-8"))
            if rows != capture["resources"]:
                raise ValueError("resource evidence differs from its original log")
            validate_gpu(rows, count, mode)
        recorded = document.get("gpu_survivors", {}).get(label)
        if recorded:
            actual = read_survivors(Path(recorded["path"]), count, (count + 2) // 3)
            if actual != recorded:
                raise ValueError("GPU survivor evidence changed since the static witness")
    if any(value <= 0 for value in front_cells(images["empty"], images["cpu"], count)):
        raise ValueError("not every known front object has coverage")
    for label in ("fixed", "gpu", "hiz", "repeat"):
        if full_pixel_difference(images["cpu"], images[label]) >= 0.0001 or \
                any(front_cells(images["cpu"], images[label], count)):
            raise ValueError("a GPU ablation has missing or changed visible geometry")
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--witness", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--samples", type=int, default=600)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not args.exe.is_file() or not args.witness.is_file() or \
            not 1 <= args.repeats <= 20 or not 300 <= args.samples <= 100000:
        parser.error("existing executable/witness and valid repeats/samples required")
    witness = verify_witness(args.witness.resolve(), args.exe.resolve())
    root = args.out.resolve() / uuid.uuid4().hex[:12]
    root.mkdir(parents=True, exist_ok=False)
    source, exe_hash, witness_hash = source_identity(), sha256_file(args.exe), sha256_file(args.witness)
    runs = []
    for index in range(args.repeats):
        # Rotate then reverse alternate repetitions; retain actual run order.
        order = MODES[index % 4:] + MODES[:index % 4]
        if index % 2:
            order = tuple(reversed(order))
        for mode in order:
            if sha256_file(args.exe) != exe_hash or sha256_file(args.witness) != witness_hash:
                raise ValueError("binary or static witness changed during the timing matrix")
            target = root / ("run-%02u-%s" % (index, mode))
            target.mkdir()
            env = timing_environment(witness["captures"][mode]["environment"], target)
            intent = {"source": source, "executable_sha256": exe_hash, "environment": env,
                      "witness_sha256": witness_hash, "mode": mode, "warmup_intervals": 120,
                      "measured_intervals": args.samples, "index": index, "order": order}
            (target / "intent.json").write_text(json.dumps(intent, indent=2) + "\n",
                                                encoding="utf-8", newline="\n")
            process = run(env, target / "engine.log", frames=121 + args.samples, timeout=1800,
                          exe=args.exe.resolve(), samples_path=target / "frames.csv",
                          expected_workload=(3, witness["count"]))
            validate_model((target / "engine.log").read_text(encoding="utf-8"),
                           witness["count"], witness["vertices"])
            validate_policy((target / "engine.log").read_text(encoding="utf-8"), mode)
            resources = player_counters((target / "engine_counters.jsonl").read_text(encoding="utf-8"))
            validate_gpu(resources, witness["count"], mode)
            summary = frame_summary(read_frames(target / "frames.csv", warmup=120,
                                               sample_count=args.samples))
            runs.append({"status": "PASS", "index": index, "mode": mode, "directory": str(target),
                         "frame": summary, "process": process, "resources": resources,
                         "raw_sha256": sha256_file(target / "frames.csv")})
            print(json.dumps({"index": index, "mode": mode, "median_ms": summary["frame_median_ms"]}), flush=True)
    result = {"status": "PASS", "source": source, "executable_sha256": exe_hash,
              "witness": str(args.witness.resolve()), "witness_sha256": witness_hash,
              "scene_sha256": witness["scene_sha256"], "count": witness["count"],
              "comparison_name": "GPU model submission", "runs": runs,
              "scope": "whole Player, known static front/offscreen/behind groups; no PNG readback in timing",
              "limitations": ["backend GPU times are delayed, not paired CPU/GPU samples",
                              "prior actual survivor evidence is revalidated; readback excluded from timing" if
                              witness.get("gpu_survivors") else
                              "input records are not independently read GPU survivors"],
              "median_frame_ms": {mode: statistics.median(row["frame"]["frame_median_ms"]
                  for row in runs if row["mode"] == mode) for mode in MODES}}
    output = root / "suite.json"
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(output), "median_frame_ms": result["median_frame_ms"]}))


if __name__ == "__main__":
    main()
