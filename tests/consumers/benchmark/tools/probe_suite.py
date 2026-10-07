#!/usr/bin/env python3
"""CPU diagnostic matrix for the SDK-only benchmark project.

Host/scenario policy belongs here; process sampling and source identities use
the repository's common tools. No editor is substituted for a Player.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
from pathlib import Path
import re
import statistics
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from tools.benchmark_suite import machine_manifest, sha256_file, source_identity
from tools.process_sampler import run_process

PROBES = ("jobs", "transform", "hierarchy", "scene-load", "raycast",
          "pathfinding", "behavior-tree", "physics-contacts", "physics-disabled",
          "animation", "animation-blend", "world-stream", "ui-layout")
ROW = re.compile(
    r"benchmark-probe: name=(\S+) round=(\d+) count=(\d+) completed=(\d+) "
    r"concurrency=(\d+) depth=(\d+) ticks=(\d+) frequency=(\d+) checksum=(\d+) correct=(\d+)")
FIELDS = ("probe", "round", "count", "completed", "concurrency", "depth",
          "ticks", "frequency", "checksum", "correct")
POINT = re.compile(r"benchmark-stream-point: round=(\d+) chunk=(\d+) ticks=(\d+) frequency=(\d+)")


def parse_waypoints(log, *, count, rounds):
    rows = [dict(zip(("round", "chunk", "ticks", "frequency"), map(int, match.groups())))
            for match in POINT.finditer(log)]
    if [(row["round"], row["chunk"]) for row in rows] != [
            (r, c) for r in range(rounds) for c in range(count)]:
        raise ValueError("stream route has missing, duplicate or reordered waypoints")
    if len({row["frequency"] for row in rows}) != 1:
        raise ValueError("waypoint clock frequency changed")
    for row in rows:
        if row["ticks"] <= 0 or row["frequency"] <= 0:
            raise ValueError("waypoint has no positive monotonic interval")
        row["ms"] = row["ticks"] * 1000 / row["frequency"]
    return rows


def parse_samples(log, *, probe, count, rounds, concurrency, depth):
    if "] ERROR -" in log:
        raise ValueError("process logged an ERROR")
    if log.count("benchmark-probe: completed name=" + probe) != 1:
        raise ValueError("missing or duplicate completion marker")
    samples = []
    for match in ROW.finditer(log):
        values = (match[1], *(int(match[i]) for i in range(2, 11)))
        row = dict(zip(FIELDS, values))
        if (row["probe"], row["count"], row["completed"], row["concurrency"],
                row["depth"], row["correct"]) != (probe, count, count, concurrency, depth, 1):
            raise ValueError("requested workload was not executed correctly")
        if row["ticks"] <= 0 or row["frequency"] <= 0:
            raise ValueError("missing monotonic elapsed clock")
        row["ms"] = row["ticks"] * 1000 / row["frequency"]
        if not math.isfinite(row["ms"]):
            raise ValueError("non-finite elapsed time")
        samples.append(row)
    if [row["round"] for row in samples] != list(range(rounds)):
        raise ValueError("missing, duplicate or reordered sample rounds")
    if len({row["frequency"] for row in samples}) != 1:
        raise ValueError("clock frequency changed inside process")
    return samples


def primary_matrix():
    # Explicit size and shape; no undocumented per-machine reduction in count.
    participants = sorted({1, 2, 4, 8, min(128, os.cpu_count() or 1)})
    return [*(dict(probe="jobs", count=1000000, concurrency=n, depth=1) for n in participants),
            *(dict(probe="transform", count=n, concurrency=1, depth=1)
              for n in (10000, 100000, 200000)),
            *(dict(probe="hierarchy", count=n, concurrency=1, depth=d)
              for n in (10000, 100000) for d in (1, 32)),
            dict(probe="raycast", count=10000, concurrency=1, depth=1),
            *(dict(probe="pathfinding", count=n, concurrency=1, depth=1)
              for n in (1000, 10000)),
            dict(probe="behavior-tree", count=1000, concurrency=1, depth=1),
            dict(probe="physics-contacts", count=2000, concurrency=1, depth=1),
            dict(probe="physics-disabled", count=2000, concurrency=1, depth=1),
            *(dict(probe=p, count=n, concurrency=1, depth=1)
              for p in ("animation", "animation-blend") for n in (10, 100, 500, 1000)),
            *(dict(probe="world-stream", count=16, concurrency=c, depth=32)
              for c in (1, 4)),
            *(dict(probe="ui-layout", count=n, concurrency=1, depth=1)
              for n in (1000, 10000)),
            *(dict(probe="scene-load", count=n, concurrency=1, depth=1)
              for n in (10000, 100000))]


def measure(config, *, exe, directory, rounds, timeout):
    directory.mkdir(parents=True, exist_ok=False)
    scratch = directory / "fixtures"
    scratch.mkdir()
    log_path = directory / "engine.log"
    env = {key: value for key, value in os.environ.items() if not key.startswith("JCE_")}
    env.update({"JCE_HEADLESS": "1", "JCE_WINDOW_HIDDEN": "1", "JCE_MAX_FRAMES": "2",
                "JCE_LOG_FILE": str(log_path), "JCE_BENCH_PROBE": config["probe"],
                "JCE_BENCH_COUNT": str(config["count"]), "JCE_BENCH_ROUNDS": str(rounds),
                "JCE_BENCH_CONCURRENCY": str(config["concurrency"]),
                "JCE_BENCH_DEPTH": str(config["depth"])})
    env["JCE_BENCH_SCRATCH"] = str(scratch)
    process = run_process([str(exe)], env=env, cwd=str(ROOT), timeout=timeout,
                          output_dir=directory / "process")
    result = {**config, "directory": str(directory), "process": process,
              "status": "FAIL", "samples": []}
    try:
        if process["timed_out"] or process["returncode"] != 0:
            raise ValueError("process timed out or exited unsuccessfully")
        # Engine initialization precedes installation of the optional file sink
        # in some hosts. Read attributable child stdout too, never an old log.
        file_log = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
        log = file_log if "benchmark-probe: completed name=" in file_log else process["stdout"]
        if "] ERROR -" in file_log + process["stdout"] + process["stderr"]:
            raise ValueError("process logged an ERROR")
        samples = parse_samples(log, rounds=rounds, **config)
        with (directory / "samples.csv").open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=(*FIELDS, "ms"), lineterminator="\n")
            writer.writeheader()
            writer.writerows(samples)
        values = sorted(row["ms"] for row in samples)
        result.update(status="PASS", samples=samples,
                      median_ms=statistics.median(values),
                      p99_ms=values[math.ceil(len(values) * 0.99) - 1],
                      units_per_second=config["count"] * 1000 / statistics.median(values))
        if config["probe"] in ("physics-contacts", "physics-disabled"):
            result["timing_scope"] = "one 1/60s physics step; wake preparation and body/contact validation excluded"
            result["units_per_second_scope"] = "population body-steps per wall-clock second"
        if config["probe"] in ("animation", "animation-blend"):
            result["timing_scope"] = "64-joint CPU sampling and skin-matrix evaluation; pose checks excluded; no GPU upload or draw"
            result["units_per_second_scope"] = "character evaluations per wall-clock second"
        if config["probe"] == "world-stream":
            result["timing_scope"] = "wall-clock route to chunk residency, including 1ms async polling and entity checks; no rendering/upload; warm OS cache"
            result["units_per_second_scope"] = "loaded route waypoints per wall-clock second"
            result["configuration_scope"] = "count=chunks; depth=entities/chunk; concurrency=max pending; 1 selects cooperative mode"
            result["fixture_sha256"] = {path.name: sha256_file(path) for path in sorted(scratch.iterdir()) if path.is_file()}
            points = parse_waypoints(log, count=config["count"], rounds=rounds)
            with (directory / "waypoints.csv").open("w", encoding="utf-8", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=("round", "chunk", "ticks", "frequency", "ms"), lineterminator="\n")
                writer.writeheader()
                writer.writerows(points)
            latency = sorted(row["ms"] for row in points)
            result["waypoint_latency"] = {"sample_count": len(points), "median_ms": statistics.median(latency),
                                          "p99_ms": latency[math.ceil(len(latency)*0.99)-1], "max_ms": max(latency),
                                          "raw_sha256": sha256_file(directory / "waypoints.csv")}
        if config["probe"] == "ui-layout":
            result["timing_scope"] = "headless canvas layout and raycast rect recording; rect validation excluded; no text shaping or GPU draw"
            result["units_per_second_scope"] = "laid-out element visits per wall-clock second"
    except ValueError as exc:
        result.update(status="FAIL", error=str(exc))
        for key in ("median_ms", "p99_ms", "units_per_second"):
            result.pop(key, None)
    (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--probe", choices=(*PROBES, "all"), default="all")
    parser.add_argument("--matrix", action="store_true", help="fixed primary scale/worker/shape matrix")
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--rounds", type=int, default=10)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--concurrency", type=int, default=1,
                        help="scheduler participants, including the waiting creator thread")
    parser.add_argument("--depth", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--out", type=Path, default=ROOT / "reports" / "benchmark-probes")
    args = parser.parse_args(argv)
    if not args.exe.is_file():
        parser.error("--exe must name an existing SDK-built Player")
    if not (1 <= args.count <= 2000000 and 1 <= args.rounds <= 100 and
            1 <= args.repeats <= 20 and 1 <= args.concurrency <= 128 and
            1 <= args.depth <= 64 and math.isfinite(args.timeout) and args.timeout > 0):
        parser.error("counts, rounds, repeats, concurrency, depth and timeout must be bounded positive values")
    configs = primary_matrix() if args.matrix else [
        dict(probe=p, count=args.count, concurrency=args.concurrency, depth=args.depth)
        for p in (PROBES if args.probe == "all" else (args.probe,))]
    if args.matrix and args.probe != "all":
        configs = [config for config in configs if config["probe"] == args.probe]
    suite_id = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + "-" + uuid.uuid4().hex[:8]
    directory = args.out.resolve() / suite_id
    directory.mkdir(parents=True, exist_ok=False)
    exe = args.exe.resolve()
    manifest = {"schema": 1, "suite_id": suite_id, "engine": "JCE", "host": "player", "headless": True,
                "source": source_identity(), "executable": str(exe),
                "executable_sha256": sha256_file(exe), "machine": machine_manifest(),
                "rounds": args.rounds, "repeats": args.repeats, "cases": configs,
                "timing_scope": "SDK workload plus in-loop correctness checks; initialization excluded",
                "unavailable": ["worker utilization", "scheduler internal latency", "hardware cache counters",
                                "editor hierarchy UI", "streaming camera route"]}
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    results = []
    for config in configs:
        for repeat in range(args.repeats):
            name = "%s-%d-c%d-d%d-r%d" % (config["probe"], config["count"],
                                         config["concurrency"], config["depth"], repeat)
            result = measure(config, exe=exe, directory=directory / name,
                             rounds=args.rounds, timeout=args.timeout)
            results.append(result)
            print("[probe] %s %s median=%s ms" % (result["status"], name, result.get("median_ms")), flush=True)
    status = "PASS" if all(result["status"] == "PASS" for result in results) else "FAIL"
    (directory / "suite.json").write_text(json.dumps({"status": status, "manifest": manifest,
                                                      "results": results}, indent=2) + "\n", encoding="utf-8")
    print("[probe] suite %s: %s" % (status, directory / "suite.json"), flush=True)
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
