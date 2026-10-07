#!/usr/bin/env python3
"""Guarded performance runs for the editor.

WHY A HARNESS AND NOT A COMMAND LINE
------------------------------------
Two failure modes produced wrong numbers repeatedly, and both are silent:

  1. The editor holds a single-instance lock. A second process finds the lock,
     activates the existing window and exits 0 WITHOUT RENDERING A FRAME. The
     caller then parses the log file from a previous run -- possibly of a
     different build -- and reports it as a fresh measurement. That is how a
     draw count appeared to "vary" between 145, 147 and 150 across runs of one
     binary, which sent an entire optimisation down the wrong path.

  2. A run that dies early leaves the previous log in place, with the same
     effect.

So this deletes the log first, refuses to start while another editor is alive,
and raises when the log is missing, or contains the single-instance message, or
has no perf line. A measurement that did not happen must look like an error,
never like a number.

WHAT IT DOES NOT DO
-------------------
It does not make runs comparable across invocations. The editor persists its
viewport pose in per-user session state, so two runs minutes apart can be
looking at different parts of the scene: the 200k dense bench has been measured
at 3.10 and at 5.28 ms with identical code for exactly that reason, while
200k-spread, street, graveyard and es stay within a few percent. For a
before/after, INTERLEAVE the two builds (A, B, A, B, ...) and compare medians;
never compare a run today against a number from an hour ago.

For image comparisons see tools/visual_diff.py, which carries the determinism
recipe and refuses to report when its own noise floor is too high.

USAGE
-----
    from tools.perf_bench import run
    r = run({"JCE_SCENE": "...", "JCE_BENCH_AUTOSPAWN": "3,200000"},
            Path("out.log"), frames=500)
    r["frame"], r["draws"], r["phases"]["sr_loop"]

    python tools/perf_bench.py --scene street_demo/.../street.scene.json \\
        --spawn 3,200000 --runs 3
"""
from collections import Counter
import argparse
import math
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from tools.build.jce import HOST, TARGETS, host_arch, preset_binary_dir


def default_editor_exe():
    target = next((row for row in TARGETS.values()
                   if row["host"] == HOST and row["arch"] == host_arch()), None)
    if target is None:
        return ROOT / "build" / "desktop" / (HOST + "-" + host_arch()) / "release" / "jce_editor"
    name = "jce_editor.exe" if HOST == "windows" else "jce_editor"
    return preset_binary_dir(target, "release") / "release" / name


EXE = default_editor_exe()

# PERF_RE now lives in jce_determinism (imported below) so the acceptance
# protocol and this file cannot drift apart on what a perf line looks like.
PHASE_RE = re.compile(r"perf-phases: avg-ms/frame over \d+: (.*?) at jce_engine")


import sys as _sys
from pathlib import Path as _Path
_sys.path.insert(0, str(_Path(__file__).resolve().parent))
from jce_determinism import (DETERMINISM, assert_backend,   # noqa: E402
                             PERF_RE, parse_perf)
from process_sampler import run_process  # noqa: E402


def _editor_alive():
    try:
        import psutil
    except ImportError:
        if os.name == "nt":
            out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq jce_editor.exe", "/NH"],
                                 capture_output=True, text=True, check=True).stdout
            return "jce_editor.exe" in out.lower()
        out = subprocess.run(["ps", "-A", "-o", "comm="],
                             capture_output=True, text=True, check=True).stdout
        return any(Path(row.strip()).name == "jce_editor" for row in out.splitlines())
    for proc in psutil.process_iter(["name"]):
        if (proc.info.get("name") or "").lower() in ("jce_editor", "jce_editor.exe"):
            return True
    return False


def wait_idle(timeout=180):
    """Refuse to start while another editor holds the single-instance lock."""
    deadline = time.time() + timeout
    while _editor_alive():
        if time.time() > deadline:
            raise RuntimeError("an editor is still running after %ds; refusing "
                               "to start (its lock would make this run a no-op "
                               "and we would read the OLD log)" % timeout)
        time.sleep(0.3)


def read_execution_log(log_path, process=None):
    """Merge this execution's captures, never old rotated engine logs.

    Preserve the maximum per-stream multiplicity of each normalized line:
    mirror captures do not double events, real repeats in one stream survive.
    Freshness belongs to run_process/its guarded caller, not filesystem mtime.
    """
    path = Path(log_path)
    captured = process or {}
    streams = []
    for name in ("stdout", "stderr"):
        if name in captured:
            streams.append(captured[name])
        else:
            source = Path(captured.get(name + "_path",
                path.parent / (path.stem + "_process") / (name + ".log")))
            streams.append(source.read_text(encoding="utf-8", errors="replace") if source.exists() else "")
    streams.append(path.read_text(encoding="utf-8", errors="replace"))
    normalized = [re.sub(r"\x1b\[[0-9;]*m", "", text).splitlines() for text in streams]
    normalized.sort(key=lambda lines: sum(map(len, lines)), reverse=True)
    merged = list(normalized[0])
    counts = Counter(merged)
    for lines in normalized[1:]:
        seen = Counter()
        for line in lines:
            seen[line] += 1
            if seen[line] > counts[line]:
                merged.append(line)
        counts |= seen
    text = "\n".join(merged)
    # The ordinary backend validator checks the request; also reject contradictory
    # actual markers instead of silently accepting whichever stream appears first.
    from jce_determinism import actual_backend
    reported = {re.sub(r"\s+", "", name).casefold()
                for line in merged if (name := actual_backend(line)) is not None}
    if len(reported) > 1:
        raise RuntimeError("conflicting actual renderer markers in fresh execution captures")
    return text


def has_exact_workload(log_text, kind, count):
    observed = re.findall(
        r"benchmark-spawn: kind=(\d+) requested=(\d+) spawned=(\d+)\b",
        log_text)
    return observed == [(str(kind), str(count), str(count))]


def run(env_over, log_path, frames=400, timeout=1800, *,
        exe=EXE, samples_path=None, expected_workload=None,
        max_seconds=None, live_markers=None, sample_interval=0.1):
    """One guarded run. Invalid or stale output raises instead of yielding data.

    samples_path enables the engine's full per-frame KPI stream. The caller
    owns warm-up selection and derived statistics; the original CSV stays raw.
    """
    log_path = Path(log_path)
    exe = Path(exe).resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    if log_path.exists():
        log_path.unlink()
    if samples_path is not None:
        samples_path = Path(samples_path)
        samples_path.parent.mkdir(parents=True, exist_ok=True)
        if samples_path.exists():
            samples_path.unlink()
    if exe.name.lower() in ("jce_editor", "jce_editor.exe") and not env_over.get("JCE_MULTI_INSTANCE"):
        wait_idle()

    env = {key: value for key, value in os.environ.items()
           if not key.startswith("JCE_")}
    env.update(DETERMINISM)
    env.update({
        "JCE_PERF_LOG": "1",
        "JCE_LOG_FILE": str(log_path),
        "JCE_MAX_FRAMES": str(frames),
    })
    if samples_path is not None:
        env["JCE_KPI_FRAME_LOG"] = str(samples_path)
        env["JCE_KPI_FRAME_COUNT"] = str(frames)
    if max_seconds is not None:
        if not math.isfinite(max_seconds) or max_seconds <= 0 or max_seconds > 86400:
            raise ValueError("max_seconds must be in (0, 86400]")
        env["JCE_MAX_SECONDS"] = str(max_seconds)
    env.update(env_over)
    result = run_process([str(exe)], env=env, cwd=str(ROOT), timeout=timeout,
                         output_dir=log_path.parent / (log_path.stem + "_process"),
                         marker_path=samples_path, live_markers=live_markers,
                         interval=sample_interval)
    if result["timed_out"]:
        raise RuntimeError("the run timed out after %.1f seconds" % result["elapsed_s"])
    if result["returncode"] != 0:
        raise RuntimeError(
            "the run exited %d -- a measurement from a process that did not "
            "finish is not a measurement. Last stderr: %s"
            % (result["returncode"], result["stderr"].strip()[-400:] or "(empty)"))
    if not log_path.exists():
        raise RuntimeError("no log at %s -- the run produced nothing" % log_path)
    log_text = read_execution_log(log_path, result)
    # Standalone and editor builds can install JCE_LOG_FILE at different
    # points in boot. Require the actual renderer marker in either stream.
    actual = assert_backend(log_text,
                            env.get("JCE_BACKEND", "auto"))
    print("[perf] engine reported backend: %s" % actual)
    if "] ERROR -" in log_text:
        raise RuntimeError("engine logged an ERROR during the fresh execution")
    if "single-instance lock already held" in log_text:
        raise RuntimeError("the run hit the single-instance lock and exited "
                           "without rendering; this log is NOT a measurement")
    if expected_workload is not None:
        kind, count = expected_workload
        marker = "benchmark-spawn: kind=%d requested=%d spawned=%d" % (
            kind, count, count)
        if not has_exact_workload(log_text, kind, count):
            raise RuntimeError("expected workload marker missing: %s" % marker)

    rows = PERF_RE.findall(log_text)
    if not rows:
        raise RuntimeError("no perf line in %s -- the run did not reach steady "
                           "state" % log_path)
    last = rows[-1]
    out = {
        "frame": float(last[0]), "fps": int(last[1]),
        "cpu": float(last[2]), "gpu": float(last[3]),
        "draws": int(last[4]), "gpu_mem": int(last[5]),
        "tex": int(last[6]), "rt": int(last[7]),
        "rss": int(last[8]), "commit": int(last[9]),
        "phases": {}, "backend_actual": actual,
        "process": {k: v for k, v in result.items()
                    if k not in ("stdout", "stderr", "samples")},
        "process_samples": result["samples"],
    }
    ph = PHASE_RE.findall(log_text)
    if ph:
        out["phases"] = {k: float(v)
                         for k, v in re.findall(r"([a-z_0-9]+)=([0-9.]+)", ph[-1])}
    if samples_path is not None:
        if not samples_path.exists() or samples_path.stat().st_size == 0:
            raise RuntimeError("no raw samples at %s" % samples_path)
        out["samples_path"] = str(samples_path)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scene", required=True)
    ap.add_argument("--spawn", help='benchmark autospawn, e.g. "3,200000"')
    ap.add_argument("--spread", help="JCE_BENCH_SPREAD world units")
    ap.add_argument("--env", action="append", default=[], metavar="KEY=VAL")
    ap.add_argument("--frames", type=int, default=400)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--phase", action="append", default=[],
                    help="also report this perf-phase (repeatable)")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    out = Path(args.out) if args.out else ROOT / ".jce/perf_bench"
    env = {"JCE_SCENE": args.scene}
    if args.spawn:
        env["JCE_BENCH_AUTOSPAWN"] = args.spawn
        env["JCE_BENCH_ISOLATE"] = "1"
    if args.spread:
        env["JCE_BENCH_SPREAD"] = args.spread
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v

    frames, draws, phases = [], [], {p: [] for p in args.phase}
    for i in range(args.runs):
        r = run(env, out / ("run_%d.log" % i), frames=args.frames)
        frames.append(r["frame"])
        draws.append(r["draws"])
        for p in args.phase:
            phases[p].append(r["phases"].get(p, 0.0))
        print("  run %d: frame=%.2f ms  draws=%d" % (i, r["frame"], r["draws"]))

    print("frame median %.2f ms  (%s)" %
          (statistics.median(frames), " ".join("%.2f" % f for f in frames)))
    print("draws %s%s" % (draws, "" if len(set(draws)) == 1 else
                          "   <-- draw count VARIED: the runs are not "
                          "comparable, the camera moved"))
    for p in args.phase:
        print("%-12s median %.3f ms  (%s)" %
              (p, statistics.median(phases[p]),
               " ".join("%.3f" % x for x in phases[p])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
