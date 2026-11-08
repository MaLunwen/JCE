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
has no perf line by default. Explicit periodic-log-off controls instead require
fresh complete CPU frame CSV and matching writer completion. Unavailable log
metrics stay None; raw CPU intervals do not imply GPU execution.

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
import json
import math
import os
import re
import secrets
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

# Periodic telemetry is parsed by jce_determinism, shared with acceptance.
PHASE_RE = re.compile(r"perf-phases: avg-ms/frame over \d+: (.*?) at jce_engine")
WRITER_RE = re.compile(
    r"frame KPI capture complete: accepted=(\d+) written=(\d+) "
    r"accepted_bytes=(\d+) written_bytes=(\d+) peak_bytes=(\d+) peak_records=(\d+)\b")


import sys as _sys
from pathlib import Path as _Path
_sys.path.insert(0, str(_Path(__file__).resolve().parent))
from jce_determinism import (DETERMINISM, assert_backend,   # noqa: E402
                             parse_perf)
from process_sampler import run_process, validate_gpu_options  # noqa: E402


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


def _validate_raw_cpu_capture(samples_path, log_text, environment, expected_frame_count):
    """Reuse the streaming CSV authority once and match actual writer accounting."""
    if samples_path is None or not samples_path.exists() or samples_path.stat().st_size == 0:
        raise RuntimeError("no raw samples at %s" % samples_path)
    # Lazy import avoids the suite's normal runner import creating a cycle.
    from tools.benchmark_suite import read_frames
    samples = read_frames(samples_path, warmup=0, sample_count=None, allow_zero_wall=True)
    selector = environment.get("JCE_FRAME_LOG_CONTEXT", "")
    if selector not in ("", "0", "1", "2", "3"):
        raise ValueError("JCE_FRAME_LOG_CONTEXT must be 0, 1, 2 or 3")
    expected_schema = {"1": "window_v1", "2": "window_viewports_v2",
                       "3": "window_viewports_clock_v3"}.get(selector)
    if samples["context_schema"] != expected_schema:
        raise RuntimeError("raw frame context schema does not match the requested capture")
    indices = samples["frame_indices"]
    count = len(indices)
    if not count or indices[0] != 0:
        raise RuntimeError("raw frame capture must start at frame zero and be nonempty")

    seconds = float(environment.get("JCE_MAX_SECONDS", "0"))
    if not math.isfinite(seconds) or seconds < 0 or seconds > 86400:
        raise ValueError("effective JCE_MAX_SECONDS must be in [0, 86400]")
    if expected_frame_count is not None:
        if isinstance(expected_frame_count, bool) or not isinstance(expected_frame_count, int) or expected_frame_count < 1:
            raise ValueError("expected_frame_count must be a positive integer")
        expected = expected_frame_count
    elif seconds > 0:
        expected = None
    else:
        expected = int(environment["JCE_MAX_FRAMES"])
        if expected < 1:
            raise ValueError("effective JCE_MAX_FRAMES must be positive for a fixed-frame raw capture")
    if seconds > 0 and "JCE_MAX_SECONDS reached" not in log_text:
        raise RuntimeError("timed raw capture did not reach JCE_MAX_SECONDS")
    if expected is not None and count != expected:
        raise RuntimeError("raw frame count %d does not match expected frame count %d" % (count, expected))

    rows = WRITER_RE.findall(log_text)
    if len(rows) != 1 or "frame KPI capture incomplete:" in log_text:
        raise RuntimeError("raw frame capture needs exactly one complete KPI writer summary")
    writer = dict(zip(("accepted", "written", "accepted_bytes", "written_bytes", "peak_bytes", "peak_records"),
                      map(int, rows[0])))
    if writer["accepted"] != count or writer["written"] != count:
        raise RuntimeError("KPI writer records do not match the complete raw frame count")
    # The native writer excludes its actual schema header from payload bytes.
    # The shared parser validates that header and every row before admission.
    payload_bytes = samples_path.stat().st_size - samples["csv_header_bytes"]
    if writer["accepted_bytes"] != payload_bytes or writer["written_bytes"] != payload_bytes:
        raise RuntimeError("KPI writer payload bytes do not match the fresh raw CSV")
    return {"raw_frame_count": count, "kpi_writer": writer,
            "expected_frame_count": expected, "max_seconds_requested": seconds or None,
            "frame_budget_requested": int(environment["JCE_MAX_FRAMES"]),
            "raw_cpu_capture_complete": True,
            "frame_context_schema": samples["context_schema"],
            "capture_scope": "declared_writer_window",
            "timed_horizon_checked": False}


def run(env_over, log_path, frames=400, timeout=1800, *,
        exe=EXE, samples_path=None, expected_workload=None,
        max_seconds=None, live_markers=None, sample_interval=0.1, cwd=None,
        require_perf_log=True, expected_frame_count=None, require_terminal_ack=False,
        terminal_trace_max_history=None, sample_gpu=False, gpu_limit=256,
        gpu_peer_pids=()):
    """One guarded run. Invalid or stale output raises instead of yielding data.

    samples_path enables the engine's full per-frame KPI stream. The caller
    owns warm-up selection and derived statistics; the original CSV stays raw.
    cwd selects an isolated launch directory; omitted callers retain ROOT.
    require_perf_log=False admits only explicit JCE_PERF_LOG=0 with a complete
    raw CPU CSV and matching writer summary. Periodic metrics are None when
    unavailable, phases remain empty, and perf_log_available records coverage.
    Fixed-frame controls require the effective JCE_MAX_FRAMES count; an explicit
    expected_frame_count declares an intentionally shorter capture window.
    Timed controls require the actual limit marker instead of a frame sentinel.
    Writer completion covers the declared capture window; callers validate full
    timed-horizon coverage separately from the application's limit marker.
    Disabling PERF_LOG changes combined periodic telemetry, not only GPU timing.
    require_terminal_ack explicitly requests one token-bound host-tail record in
    owned stderr. Requested trace additionally needs a fresh continuous stream
    matching the final cleanup scope/session. This does not certify GPU or disk
    durability; legacy/default runs do not request or consume the protocol.
    A small exclusive prelaunch request receipt retains the expected nonce
    even when the child fails or a later log/capture check rejects the run.
    terminal_trace_max_history optionally bounds the complete trace validator's
    retained history (integer 1..1000000, default 200000). An explicit override
    requires terminal acknowledgement and a requested continuous trace; it is
    validated before prior outputs are touched or a native process is launched.
    sample_gpu opts into the existing process sampler's OS engine-rate observer;
    gpu_limit and explicit gpu_peer_pids use its shared validation before output
    mutation. Raw process receipt paths/availability are retained unchanged, and
    counters do not become GPU frame time, FPS or presentation evidence.
    """
    validate_gpu_options(sample_gpu, gpu_limit, gpu_peer_pids)
    if not isinstance(require_perf_log, bool):
        raise ValueError("require_perf_log must be boolean")
    if type(require_terminal_ack) is not bool:
        raise ValueError("require_terminal_ack must be boolean")
    if samples_path is not None and env_over.get("JCE_FRAME_LOG_CONTEXT", "") not in ("", "0", "1", "2", "3"):
        raise ValueError("JCE_FRAME_LOG_CONTEXT must be 0, 1, 2 or 3")
    if not require_perf_log:
        # Validate the explicit opt-out before touching previous evidence or
        # starting a native child. The ordinary strict-run path is unchanged.
        if max_seconds is not None and (not math.isfinite(max_seconds) or
                max_seconds <= 0 or max_seconds > 86400):
            raise ValueError("max_seconds must be in (0, 86400]")
        if env_over.get("JCE_PERF_LOG") != "0" or samples_path is None:
            raise ValueError("raw CPU admission requires explicit JCE_PERF_LOG=0 and samples_path")
        if expected_frame_count is not None and (isinstance(expected_frame_count, bool) or
                not isinstance(expected_frame_count, int) or expected_frame_count < 1):
            raise ValueError("expected_frame_count must be a positive integer")
        configured_frames = int(env_over.get("JCE_MAX_FRAMES", frames))
        configured_seconds = float(env_over.get("JCE_MAX_SECONDS", max_seconds or 0))
        if configured_frames < 1 or not math.isfinite(configured_seconds) or not 0 <= configured_seconds <= 86400:
            raise ValueError("invalid effective frame budget or timed duration for raw CPU admission")
    log_path = Path(log_path).resolve()
    exe = Path(exe).resolve()
    launch_cwd = ROOT if cwd is None else Path(cwd).resolve()
    # The reader's explicit log target must match the child's effective target.
    # Refuse before unlinking evidence or launching expensive native work.
    if "JCE_LOG_FILE" in env_over:
        requested_log = env_over["JCE_LOG_FILE"]
        if not isinstance(requested_log, str) or not requested_log:
            raise ValueError("JCE_LOG_FILE must match the explicit log_path")
        requested_log = Path(requested_log)
        if not requested_log.is_absolute():
            requested_log = launch_cwd / requested_log
        if requested_log.resolve() != log_path:
            raise ValueError("JCE_LOG_FILE conflicts with the explicit log_path")
    process_output = log_path.parent / (log_path.stem + "_process")
    terminal_token, terminal_trace = None, None
    if require_terminal_ack:
        from tools.terminal_ack_observation import requested_trace_path
        terminal_trace = requested_trace_path(env_over, launch_cwd)
        if terminal_trace is not None and (terminal_trace == log_path or
                (samples_path is not None and terminal_trace == Path(samples_path).resolve()) or
                terminal_trace == process_output or process_output in terminal_trace.parents):
            raise ValueError("terminal ack trace path overlaps another run output")
        terminal_token = secrets.token_hex(16)
    if require_terminal_ack or terminal_trace_max_history is not None:
        from tools.terminal_ack_observation import terminal_trace_history_budget
        terminal_trace_history_budget(terminal_trace_max_history,
            trace_requested=require_terminal_ack and terminal_trace is not None)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    if log_path.exists():
        log_path.unlink()
    if samples_path is not None:
        samples_path = Path(samples_path).resolve()
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
    terminal_request = None
    if require_terminal_ack:
        env["JCE_TERMINAL_ACK_TOKEN"] = terminal_token
        process_output.mkdir(parents=True, exist_ok=True)
        terminal_request = process_output / f"terminal-ack-request-{terminal_token}.json"
        # This is a request witness, not a launch or cleanup verdict.  Do not
        # derive the expected nonce from the child's own stderr after failure.
        with terminal_request.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump({
                "schema": 1,
                "scope": "prelaunch terminal acknowledgement request",
                "nonce": terminal_token,
                "exe_path": str(exe),
                "launch_cwd": str(launch_cwd),
                "expected_stderr_path": str(process_output / "stderr.log"),
                "trace_path": str(terminal_trace) if terminal_trace is not None else None,
            }, stream, sort_keys=True)
            stream.write("\n")
    result = run_process([str(exe)], env=env, cwd=str(launch_cwd), timeout=timeout,
                         output_dir=process_output,
                         marker_path=samples_path, live_markers=live_markers,
                         interval=sample_interval, sample_gpu=sample_gpu,
                         gpu_limit=gpu_limit, gpu_peer_pids=gpu_peer_pids)
    if result["timed_out"]:
        raise RuntimeError("the run timed out after %.1f seconds" % result["elapsed_s"])
    if result["returncode"] != 0:
        raise RuntimeError(
            "the run exited %d -- a measurement from a process that did not "
            "finish is not a measurement. Last stderr: %s"
            % (result["returncode"], result["stderr"].strip()[-400:] or "(empty)"))
    terminal_observation = None
    if require_terminal_ack:
        from tools.terminal_ack_observation import observe_terminal_ack
        terminal_observation = observe_terminal_ack(result, terminal_token,
            expected_stderr_path=process_output / "stderr.log", trace_path=terminal_trace,
            terminal_trace_max_history=terminal_trace_max_history)
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

    last = parse_perf(log_text)
    if last is None and require_perf_log:
        raise RuntimeError("no perf line in %s -- the run did not reach steady "
                           "state" % log_path)
    out = {
        "frame": last["frame_ms"] if last else None, "fps": last["fps"] if last else None,
        "cpu": last["cpu_ms"] if last else None, "gpu": last["gpu_ms"] if last else None,
        "draws": last["draws"] if last else None, "gpu_mem": last["gpu_mem_mb"] if last else None,
        "tex": last["tex_mb"] if last else None, "rt": last["rt_mb"] if last else None,
        "rss": last["rss_mb"] if last else None, "commit": last["commit_mb"] if last else None,
        "memory_observation": {key: last[key] for key in (
            "memory_unit", "memory_log_unit", "memory_raw", "memory_availability",
            "memory_scope")}
            if last else None,
        "perf_log_available": last is not None, "perf_log_required": require_perf_log,
        "phases": {}, "backend_actual": actual,
        "process": {k: v for k, v in result.items()
                    if k not in ("stdout", "stderr", "samples")},
        "process_samples": result["samples"],
    }
    ph = PHASE_RE.findall(log_text)
    if ph and last is not None:
        out["phases"] = {k: float(v)
                         for k, v in re.findall(r"([a-z_0-9]+)=([0-9.]+)", ph[-1])}
    if samples_path is not None:
        if not samples_path.exists() or samples_path.stat().st_size == 0:
            raise RuntimeError("no raw samples at %s" % samples_path)
        out["samples_path"] = str(samples_path)
    if not require_perf_log:
        out.update(_validate_raw_cpu_capture(samples_path, log_text, env, expected_frame_count))
    if terminal_observation is not None:
        out["terminal_ack"] = terminal_observation
        out["terminal_ack_request_path"] = str(terminal_request)
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
    ap.add_argument("--require-terminal-ack", action="store_true",
                    help="require token-bound host-tail acknowledgement and correlate any requested trace")
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
        r = run(env, out / ("run_%d.log" % i), frames=args.frames,
                require_terminal_ack=args.require_terminal_ack)
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
