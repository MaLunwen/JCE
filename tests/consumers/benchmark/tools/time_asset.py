#!/usr/bin/env python3
"""Whole Player frame timings conditioned on an imported-character witness.

Import, initialization and 120 warm-up intervals are excluded. Frame timings
include update/render/post/composition and KPI export; GPU samples are delayed
backend statistics, not individually aligned GPU/CPU frame pairs.
"""
import argparse
import datetime
import json
import math
from pathlib import Path
import statistics
import sys
import uuid

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from tools.benchmark_suite import (frame_summary, host_render_environment, player_counters,
                                   read_frames, sha256_file, source_identity)
from tools.perf_bench import run
from check_asset_pixels import (cells_pass, pixels_pass, validate_markers,
                                validate_population, validate_still_controls)


def witness_input(path, exe):
    witness = json.loads(path.read_text(encoding="utf-8"))
    if witness.get("status") != "LIVENESS_PASS" or not pixels_pass(witness["differences"]):
        raise ValueError("a successful imported-character pixel witness is required")
    if witness["executable_sha256"] != sha256_file(exe):
        raise ValueError("witness and timing executable differ")
    subject = witness["results"]["moving50"]
    count = subject["live_samples"][0][1]
    if count > 1 and any(not cells_pass(witness.get("per_cell_changed_pixels", {}).get(key, []), count)
                         for key in ("coverage", "motion")):
        raise ValueError("every character needs visible coverage and motion")
    for sample in witness["results"].values():
        if sha256_file(sample["image"]) != sample["image_sha256"]:
            raise ValueError("a witness capture changed")
        if sha256_file(Path(sample["environment"]["JCE_SCENE"])) != sample["scene_sha256"]:
            raise ValueError("a witness scene changed")
    env = dict(subject["environment"])
    if sha256_file(Path(env["JCE_BENCH_ASSET_PAK"])) != witness["pak_sha256"]:
        raise ValueError("the imported resource PAK changed")
    chroma = witness["differences"]["chromatic_coverage"]
    minimum = witness["minimum_chromatic_coverage"]
    if not all(isinstance(value, (int, float)) and math.isfinite(value)
               and 0 <= value <= 1 for value in (chroma, minimum)) or chroma < minimum:
        raise ValueError("the colored material witness failed")
    return witness, env, count


def timing_environment(original, directory):
    # The witness process owns captures; none may leak into measured frames.
    env = {key: value for key, value in original.items() if key not in (
        "JCE_BENCH_PLAYER_SHOT", "JCE_KPI_GAME_SHOTS", "JCE_LOG_FILE",
        "JCE_MAX_FRAMES", "JCE_MAX_SECONDS", "JCE_KPI_FRAME_LOG", "JCE_KPI_FRAME_COUNT")}
    env.update(host_render_environment("player", directory))
    return env


def trial_schedule(repeats, interval, comparison=None):
    if comparison == interval:
        raise ValueError("durability A/B intervals must differ")
    trials = []
    for index in range(repeats):
        modes = [interval] if comparison is None else [interval, comparison]
        if index % 2:
            modes.reverse()
        trials.extend((index, mode) for mode in modes)
    return trials


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--witness", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--samples", type=int, default=300)
    parser.add_argument("--durable-flush-interval", type=int, default=0,
                        help="diagnostic storage-sync interval; 0 flushes only at shutdown")
    parser.add_argument("--compare-durable-flush-interval", type=int,
                        help="interleave a second durability interval on the same executable")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not args.exe.is_file() or not args.witness.is_file() or not 1 <= args.repeats <= 20 \
            or not 300 <= args.samples <= 100000 or not 0 <= args.durable_flush_interval <= 100000:
        parser.error("existing inputs, repeats 1..20 and at least 300 samples required")
    if args.compare_durable_flush_interval is not None and (
            not 0 <= args.compare_durable_flush_interval <= 100000 or
            args.compare_durable_flush_interval == args.durable_flush_interval):
        parser.error("comparison interval must be distinct and within 0..100000")
    witness, original_env, count = witness_input(args.witness, args.exe)
    root = args.out.resolve() / uuid.uuid4().hex[:12]
    root.mkdir(parents=True, exist_ok=False)
    identity = source_identity()
    rows = []
    for index, interval in trial_schedule(args.repeats, args.durable_flush_interval,
                                          args.compare_durable_flush_interval):
        target = root / ("run-%02u-flush-%u" % (index, interval))
        target.mkdir()
        env = timing_environment(original_env, target)
        env["JCE_KPI_DURABLE_FLUSH_INTERVAL"] = str(interval)
        raw, log = target / "frames.csv", target / "engine.log"
        intent = {"environment": env, "source": identity, "executable_sha256": sha256_file(args.exe),
                  "witness": str(args.witness.resolve()), "witness_sha256": sha256_file(args.witness),
                  "warmup_intervals": 120, "measured_intervals": args.samples}
        (target / "intent.json").write_text(json.dumps(intent, indent=2) + "\n",
                                             encoding="utf-8", newline="\n")
        metrics = run(env, log, frames=120 + args.samples + 1, timeout=900,
                      exe=args.exe.resolve(), samples_path=raw)
        text = log.read_text(encoding="utf-8")
        if "] ERROR -" in text or "benchmark-player: entered Play" not in text:
            raise ValueError("the Player did not enter a clean simulation")
        validate_still_controls(text)
        topology = witness["results"]["moving50"]["live_samples"][0]
        validate_markers(text, count, topology[3], topology[5], topology[6], True)
        validate_population(text, count, tuple(witness["receiver_layers"]), True)
        resources = player_counters((target / "engine_counters.jsonl").read_text(encoding="utf-8"))
        if resources[-1]["engine"]["entity_count"] != count + 2:
            raise ValueError("the live entity population changed")
        samples = read_frames(raw, warmup=120, sample_count=args.samples)
        row = {"status": "PASS", "index": index, "directory": str(target),
               "durable_flush_interval": interval,
               "frame": frame_summary(samples), "process": metrics,
               "resources": resources, "raw_sha256": sha256_file(raw), "log_sha256": sha256_file(log)}
        (target / "summary.json").write_text(json.dumps(row, indent=2) + "\n",
                                               encoding="utf-8", newline="\n")
        rows.append(row)
        print(json.dumps({"run": index, "count": count, "durable_flush_interval": interval,
                          "median_ms": row["frame"]["frame_median_ms"],
                          "p99_ms": row["frame"]["frame_p99_ms"],
                          "max_ms": row["frame"]["frame_max_ms"]}), flush=True)
    result = {"status": "PASS", "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "source": identity, "count": count, "runs": rows,
              "witness": str(args.witness.resolve()), "witness_sha256": sha256_file(args.witness),
              "scope": __doc__, "median_of_frame_medians_ms": statistics.median(
                  row["frame"]["frame_median_ms"] for row in rows
                  if row["durable_flush_interval"] == args.durable_flush_interval),
              "median_frame_by_flush_interval_ms": {str(mode): statistics.median(
                  row["frame"]["frame_median_ms"] for row in rows
                  if row["durable_flush_interval"] == mode)
                  for mode in sorted({row["durable_flush_interval"] for row in rows})}}
    path = root / "suite.json"
    path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"suite": str(path), "status": "PASS",
                      "median_ms": result["median_of_frame_medians_ms"]}), flush=True)


if __name__ == "__main__":
    main()
