"""Process residency sampling; RSS/commit are not allocator or GPU ownership."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics

from process_sampler import run_process, validate_thread_options, validate_gpu_options


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1048576), b""):
            value.update(block)
    return value.hexdigest()


def validate_window(warmup, window, timeout=None):
    if not math.isfinite(warmup) or warmup < 0 or not math.isfinite(window) or window <= 0:
        raise ValueError("finite nonnegative warmup and positive window required")
    if timeout is not None and (not math.isfinite(timeout) or timeout <= warmup + window):
        raise ValueError("finite timeout must exceed the observation window")


def summarize(samples, warmup, window):
    validate_window(warmup, window)
    selected = []
    previous = -1.0
    for sample in samples:
        time = sample.get("elapsed_s")
        if isinstance(time, bool) or not isinstance(time, (float, int)) or not math.isfinite(time) or time < previous:
            raise ValueError("ordered finite sample times required")
        previous = time
        for key in ("rss_bytes", "commit_bytes", "thread_count", "handle_count"):
            value = sample.get(key)
            if value is not None and (isinstance(value, bool) or not isinstance(value, int) or value < 0):
                raise ValueError("invalid process sample value: " + key)
        if warmup <= time <= warmup + window:
            selected.append(sample)
    if len(selected) < 2 or selected[-1]["elapsed_s"] - selected[0]["elapsed_s"] < window * 0.95:
        raise ValueError("requested observation window not covered")
    if any(b["elapsed_s"] - a["elapsed_s"] > max(1.0, window * 0.05) for a, b in zip(selected, selected[1:])):
        raise ValueError("observation window has sampling gaps")
    result = {"sample_count": len(selected), "begin_s": selected[0]["elapsed_s"],
              "end_s": selected[-1]["elapsed_s"], "allocator_ownership": "UNAVAILABLE"}
    for key in ("rss_bytes", "commit_bytes", "thread_count", "handle_count"):
        values = [s.get(key) for s in selected]
        if any(v is None for v in values):
            result[key] = {"availability": "UNAVAILABLE"}
        else:
            result[key] = {"availability": "OBSERVED", "min": min(values), "max": max(values),
                           "median": statistics.median(values), "first": values[0], "last": values[-1]}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("exe", "cwd", "env_file", "out"):
        parser.add_argument("--" + name.replace("_", "-"), type=Path, required=True)
    parser.add_argument("--warmup", type=float, default=15)
    parser.add_argument("--window", type=float, default=45)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--sample-threads", action="store_true",
                        help="retain optional raw cumulative CPU per observed TID, without lifetime attribution")
    parser.add_argument("--thread-limit", type=int, default=256,
                        help="maximum retained thread rows per poll (1..1024)")
    parser.add_argument("--sample-gpu", action="store_true",
                        help="optional Windows PDH per-engine rates; no FPS or total-GPU aggregation")
    parser.add_argument("--gpu-limit", type=int, default=256,
                        help="maximum retained GPU rows per poll (1..1024), not an OS query cap")
    parser.add_argument("--gpu-peer-pid", type=int, action="append", default=[],
                        help="explicit comparison PID (<=16); requires --sample-gpu and observed birth identity")
    args = parser.parse_args()
    try:
        validate_window(args.warmup, args.window, args.timeout)
        validate_thread_options(args.sample_threads, args.thread_limit)
        validate_gpu_options(args.sample_gpu, args.gpu_limit, args.gpu_peer_pid)
    except ValueError as error:
        parser.error(str(error))
    environment_before = digest(args.env_file)
    validator_before = digest(__file__)
    overrides = json.loads(args.env_file.read_text(encoding="utf-8"))
    if not isinstance(overrides, dict) or not all(isinstance(k, str) and isinstance(v, str) for k, v in overrides.items()):
        parser.error("environment file must be a string-to-string object")
    exe = args.exe.resolve()
    before = digest(exe)
    env = os.environ.copy()
    # Explicit null-free names and values; callers own workload/hidden-window policy.
    if any(not k or "=" in k or "\0" in k or "\0" in v for k, v in overrides.items()):
        parser.error("invalid environment override")
    env.update(overrides)
    args.out.mkdir(parents=True, exist_ok=False)
    process = run_process([str(exe)], env=env, cwd=str(args.cwd.resolve()),
                          timeout=args.timeout, output_dir=args.out / "process", interval=0.1,
                          sample_threads=args.sample_threads, thread_limit=args.thread_limit,
                          sample_gpu=args.sample_gpu, gpu_limit=args.gpu_limit, gpu_peer_pids=args.gpu_peer_pid)
    if (process["timed_out"] or process["returncode"] != 0 or digest(exe) != before
            or digest(args.env_file) != environment_before or digest(__file__) != validator_before):
        raise ValueError("process did not complete normally with a stable artifact")
    summary = summarize(process["samples"], args.warmup, args.window)
    result = {"status": "PROCESS_RESIDENCY_OBSERVED", "artifact_sha256": before,
              "environment_sha256": environment_before, "validator_sha256": validator_before,
              "warmup_s": args.warmup,
              "window_s": args.window, "summary": summary,
              "process_identity": process["process_identity"],
              "gpu_sampling_requested": args.sample_gpu,
              "gpu_samples": str(args.out / "process/gpu-samples.json") if args.sample_gpu else None,
              "gpu_peer_pids": args.gpu_peer_pid,
              "thread_sampling_requested": args.sample_threads,
              "thread_cpu_samples": str(args.out / "process/samples.json") if args.sample_threads else None,
              "scope": "process working set and private commit; no heap/GPU attribution or device certification"}
    path = args.out / "residency.json"
    path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(path)


if __name__ == "__main__":
    main()
