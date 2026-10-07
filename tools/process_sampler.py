#!/usr/bin/env python3
"""Engine-neutral process sampler for reproducible local benchmarks.

The child owns its stdout/stderr files. Optional psutil sampling is explicitly
reported as unavailable when psutil is absent; no zero is substituted for a
missing measurement.
"""
from __future__ import annotations

import math
import json
import subprocess
import time
from pathlib import Path


def _process_reader(pid):
    try:
        import psutil
    except ImportError:
        return None
    try:
        return psutil.Process(pid)
    except (OSError, psutil.Error):
        # A short-lived child can exit before the initial Process lookup,
        # not only between later poll() and memory_info() calls. Retain its
        # real exit/log evidence with unavailable sampling rather than crash.
        return None


def _sample(proc, started):
    try:
        memory = proc.memory_info()
        cpu = proc.cpu_times()
        try:
            io = proc.io_counters()
            read_bytes = io.read_bytes
            write_bytes = io.write_bytes
        except (AttributeError, OSError):
            read_bytes = write_bytes = None
        resources = {"thread_count": None, "handle_count": None,
                     "file_descriptor_count": None}
        for key, name in (("thread_count", "num_threads"),
                          ("handle_count", "num_handles"),
                          ("file_descriptor_count", "num_fds")):
            getter = getattr(proc, name, None)
            if getter is not None:
                try:
                    resources[key] = getter()
                except (OSError, NotImplementedError):
                    pass
        return {
            "elapsed_s": round(time.monotonic() - started, 6),
            "rss_bytes": memory.rss,
            "commit_bytes": getattr(memory, "private", None),
            "cpu_time_s": round(cpu.user + cpu.system, 6),
            "read_bytes": read_bytes,
            "write_bytes": write_bytes,
            **resources,
        }
    except Exception as exc:
        # The child can exit between poll() and memory_info(). psutil raises
        # its own Error hierarchy (not OSError) for that normal race.
        try:
            import psutil
        except ImportError:
            psutil = None
        if isinstance(exc, (OSError, ValueError)) or (
                psutil is not None and isinstance(exc, psutil.Error)):
            return None
        raise


def _has_frame_marker(path):
    if path is None or not path.exists():
        return False
    try:
        with path.open("rb") as stream:
            return b",frame_wall," in stream.read(131072)
    except OSError:
        return False


def _has_live_marker(path, signature):
    try:
        with Path(path).open("rb") as stream:
            if signature in stream.read(131072):
                return True
            size = stream.seek(0, 2)
            stream.seek(max(0, size - len(signature) - 64))
            return signature in stream.read()
    except OSError:
        return False


def run_process(command, *, env, cwd, timeout, output_dir,
                marker_path=None, interval=0.1, live_markers=None):
    """Run one process and retain attributable OS samples and exact child IO."""
    if timeout <= 0 or not math.isfinite(timeout) or interval <= 0 or not math.isfinite(interval):
        raise ValueError("timeout and interval must be positive")
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = output_dir / "stdout.log"
    stderr_path = output_dir / "stderr.log"
    started = time.monotonic()
    samples = []
    first_frame_s = None
    live_markers = live_markers or {}
    if any(not name or not isinstance(signature, bytes) or not signature
           for name, (path, signature) in live_markers.items()):
        raise ValueError("live markers require named paths and nonempty byte signatures")
    if any(Path(path).exists() for path, signature in live_markers.values()):
        raise ValueError("live marker files must be fresh, not evidence from an earlier process")
    observed_markers = {name: None for name in live_markers}
    timed_out = False
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        child = subprocess.Popen(command, env=env, cwd=cwd,
                                 stdout=stdout, stderr=stderr)
        proc = _process_reader(child.pid)
        while child.poll() is None:
            if proc is not None:
                sample = _sample(proc, started)
                if sample is not None:
                    samples.append(sample)
            if (first_frame_s is None and _has_frame_marker(marker_path)
                    and child.poll() is None):
                first_frame_s = time.monotonic() - started
            for name, (path, signature) in live_markers.items():
                if observed_markers[name] is None and _has_live_marker(path, signature) \
                        and child.poll() is None:
                    observed_markers[name] = time.monotonic() - started
            if time.monotonic() - started > timeout:
                timed_out = True
                child.kill()
                break
            time.sleep(interval)
        child.wait()
        if proc is not None:
            sample = _sample(proc, started)
            if sample is not None:
                samples.append(sample)
        # A file that becomes readable only after exit proves completed work,
        # not startup latency. Never substitute shutdown time for first frame.
    elapsed = time.monotonic() - started
    peak_rss = max((s["rss_bytes"] for s in samples), default=None)
    peak_commit = max((s["commit_bytes"] for s in samples
                       if s["commit_bytes"] is not None), default=None)
    cpu_s = samples[-1]["cpu_time_s"] if samples else None
    result = {
        "returncode": child.returncode,
        "timed_out": timed_out,
        "elapsed_s": elapsed,
        "first_frame_s": first_frame_s,
        "first_frame_probe": "live_marker" if first_frame_s is not None
                             else "UNAVAILABLE",
        "live_markers_s": observed_markers,
        "marker_sampling_interval_s": interval,
        "peak_rss_bytes": peak_rss,
        "peak_commit_bytes": peak_commit,
        "cpu_time_s": cpu_s,
        "cpu_equivalent": cpu_s / elapsed if cpu_s is not None and elapsed else None,
        "samples": samples,
        "sampling_available": proc is not None,
        "stdout_path": str(stdout_path),
        "stderr_path": str(stderr_path),
        "stdout": stdout_path.read_text(encoding="utf-8", errors="replace"),
        "stderr": stderr_path.read_text(encoding="utf-8", errors="replace"),
    }
    # Preserve telemetry even when a higher correctness gate rejects the run.
    (output_dir / "samples.json").write_text(
        json.dumps(samples, indent=2) + "\n", encoding="utf-8", newline="\n")
    (output_dir / "result.json").write_text(
        json.dumps({key: value for key, value in result.items()
                    if key not in ("samples", "stdout", "stderr")}, indent=2)
        + "\n", encoding="utf-8", newline="\n")
    return result
