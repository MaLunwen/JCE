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


_CHILD_EXIT_GRACE_SECONDS = 5.0


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


def _optional_query_error(exc):
    if isinstance(exc, (OSError, NotImplementedError)):
        return True
    try:
        import psutil
    except ImportError:
        return False
    return isinstance(exc, psutil.Error)


def _creation_time(proc):
    if proc is None or getattr(proc, "create_time", None) is None:
        return None, "UNAVAILABLE"
    try:
        value = proc.create_time()
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
            return None, "INVALID"
        return value, "psutil.create_time"
    except Exception as exc:
        if _optional_query_error(exc):
            return None, type(exc).__name__
        raise


def _thread_sample(proc, started, limit):
    # This caps retained output, not the OS query. Cumulative counters are not
    # complete thread lifetimes: exited threads and reused TIDs remain unknown.
    result = {"thread_cpu_available": False, "thread_cpu_reason": "UNSUPPORTED",
              "threads_observed_count": None, "threads_truncated": None,
              "thread_cpu": []}
    result["thread_cpu_query_start_elapsed_s"] = round(time.perf_counter() - started, 6)
    getter = getattr(proc, "threads", None)
    if getter is not None:
        try:
            rows = []
            for thread in getter():
                values = (thread.user_time, thread.system_time)
                if (isinstance(thread.id, bool) or not isinstance(thread.id, int) or thread.id <= 0 or
                        any(isinstance(v, bool) or not isinstance(v, (int, float)) or
                            not math.isfinite(v) or v < 0 for v in values)):
                    result["thread_cpu_reason"] = "INVALID"
                    break
                rows.append({"tid": thread.id, "user_time_s": thread.user_time,
                             "system_time_s": thread.system_time})
            else:
                rows.sort(key=lambda r: (-(r["user_time_s"] + r["system_time_s"]), r["tid"]))
                result.update(thread_cpu_available=True, thread_cpu_reason="AVAILABLE",
                              threads_observed_count=len(rows), threads_truncated=len(rows) > limit,
                              thread_cpu=rows[:limit])
        except Exception as exc:
            if not _optional_query_error(exc):
                raise
            result["thread_cpu_reason"] = type(exc).__name__
    result["thread_cpu_query_end_elapsed_s"] = round(time.perf_counter() - started, 6)
    return result


def _sample(proc, started, *, sample_threads=False, thread_limit=256):
    query_start_ns = time.perf_counter_ns()
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
        query_end_ns = time.perf_counter_ns()
        result = {
            # Bracket the aggregate OS queries; none is an atomic frame sample.
            # Decimal strings preserve exact counters in JSON/JavaScript readers.
            "query_start_perf_counter_ns": str(query_start_ns),
            "query_end_perf_counter_ns": str(query_end_ns),
            "elapsed_s": round(query_end_ns / 1000000000 - started, 6),
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
    # Thread-query programming errors are outside the aggregate exit-race
    # boundary; they must propagate and trigger run_process child cleanup.
    if sample_threads:
        result.update(_thread_sample(proc, started, thread_limit))
    return result


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


def validate_thread_options(sample_threads, thread_limit):
    if type(sample_threads) is not bool or type(thread_limit) is not int or not 1 <= thread_limit <= 1024:
        raise ValueError("sample_threads must be boolean and thread_limit must be 1..1024")


def validate_gpu_options(sample_gpu, gpu_limit, gpu_peer_pids):
    if type(sample_gpu) is not bool or type(gpu_limit) is not int or not 1 <= gpu_limit <= 1024:
        raise ValueError("sample_gpu must be boolean and gpu_limit must be 1..1024")
    if (not isinstance(gpu_peer_pids, (list, tuple)) or len(gpu_peer_pids) > 16 or
        any(type(pid) is not int or not 0 < pid <= 0xffffffff for pid in gpu_peer_pids) or
        len(set(gpu_peer_pids)) != len(gpu_peer_pids)):
        raise ValueError("gpu_peer_pids must be <=16 unique explicit positive DWORD PIDs")
    if gpu_peer_pids and not sample_gpu:
        raise ValueError("gpu_peer_pids requires sample_gpu")


def _gpu_provider():
    # Default-off never imports the Windows provider or loads a DLL.
    if __package__:
        from .windows_gpu_observation import WindowsGpuObservation
    else:
        from windows_gpu_observation import WindowsGpuObservation
    return WindowsGpuObservation(clock=time.perf_counter)


class _GpuSampler:
    def __init__(self, pid, proc, birth, peers):
        if pid in peers: raise ValueError("GPU target cannot also be an explicit peer")
        self.bindings = [(pid, proc, birth, "target")]
        for peer in peers:
            reader = _process_reader(peer)
            peer_birth, _ = _creation_time(reader)
            self.bindings.append((peer, reader, peer_birth, "explicit_peer"))
        self.provider = _gpu_provider()

    def subjects(self):
        values = []
        for pid, reader, birth, purpose in self.bindings:
            # psutil caches create_time on an instance; use a fresh PID reader
            # before/after each GPU query instead of trusting that cached birth.
            current_reader = _process_reader(pid) if reader is not None else None
            current, reason = _creation_time(current_reader)
            if current_reader is not None and type(current_reader.pid) is not int:
                raise TypeError("GPU process reader must expose an actual integer PID")
            same = (birth is not None and current == birth and current_reader is not None and
                    current_reader.pid == pid)
            values.append(dict(pid=pid, creation_time_unix_s=birth, purpose=purpose,
                identity_available=same, identity_reason="AVAILABLE" if same else
                    "IDENTITY_CHANGED" if current is not None and birth is not None else
                    "IDENTITY_UNAVAILABLE" if birth is None else reason,
                creation_time_source="psutil.create_time" if birth is not None else "UNAVAILABLE"))
        return values

    def sample(self, started, limit):
        observed_start = time.perf_counter()
        before = self.subjects()
        result = self.provider.sample(before, started, limit)
        after = self.subjects()
        result["subjects_after_query"] = after
        result["observation_query_elapsed_s"] = time.perf_counter() - observed_start
        result["identity_scope"] = "initial psutil birth vs fresh PID-reader birth before/after query; not an atomic process-lifetime snapshot"
        allowed = {row["pid"] for row in after if row["identity_available"]}
        changed = False
        for row in result["rows"]:
            if row["pid"] not in allowed:
                row.update(available=False, utilization_percent=None, reason="IDENTITY_CHANGED")
                row.pop("invalid_counter_detail", None)
                changed = True
        for witness in result.get("unparsed_instance_witnesses", []):
            if witness["pid"] not in allowed:
                witness.update(identity_available=False, identity_reason="IDENTITY_CHANGED")
        if changed:
            result["available"] = any(row["available"] for row in result["rows"])
            result["reason"] = "IDENTITY_CHANGED"
        return result

    def close(self):
        return self.provider.close()


def run_process(command, *, env, cwd, timeout, output_dir,
                marker_path=None, interval=0.1, live_markers=None,
                sample_threads=False, thread_limit=256,
                sample_gpu=False, gpu_limit=256, gpu_peer_pids=()):
    """Run one process and retain attributable OS samples and exact child IO."""
    if timeout <= 0 or not math.isfinite(timeout) or interval <= 0 or not math.isfinite(interval):
        raise ValueError("timeout and interval must be positive")
    validate_thread_options(sample_threads, thread_limit)
    validate_gpu_options(sample_gpu, gpu_limit, gpu_peer_pids)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = output_dir / "stdout.log"
    stderr_path = output_dir / "stderr.log"
    clock_info = time.get_clock_info("perf_counter")
    started_ns = time.perf_counter_ns()
    started = started_ns / 1000000000
    clock = {
        "api": "time.perf_counter", "ns_api": "time.perf_counter_ns",
        "implementation": clock_info.implementation,
        "monotonic": clock_info.monotonic, "adjustable": clock_info.adjustable,
        "resolution_s": clock_info.resolution,
        "origin_perf_counter_ns": str(started_ns),
        "origin_scope": "before child Popen; not Unix time",
        "integer_encoding": "nanosecond counters are decimal strings",
        "alignment_scope": "native clock identity only; cross-process alignment is not certified",
    }
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
    gpu_sampler, gpu_close_status, gpu_samples = None, None, []
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        child = subprocess.Popen(command, env=env, cwd=cwd,
                                 stdout=stdout, stderr=stderr)
        try:
            proc = _process_reader(child.pid)
            creation_time, creation_source = _creation_time(proc)
            if sample_gpu:
                gpu_sampler = _GpuSampler(child.pid, proc, creation_time, gpu_peer_pids)
            while child.poll() is None:
                if proc is not None:
                    sample = _sample(proc, started, sample_threads=sample_threads, thread_limit=thread_limit)
                    if sample is not None:
                        samples.append(sample)
                if gpu_sampler is not None:
                    gpu_samples.append(gpu_sampler.sample(started, gpu_limit))
                if (first_frame_s is None and _has_frame_marker(marker_path)
                        and child.poll() is None):
                    first_frame_s = time.perf_counter() - started
                for name, (path, signature) in live_markers.items():
                    if observed_markers[name] is None and _has_live_marker(path, signature) \
                            and child.poll() is None:
                        observed_markers[name] = time.perf_counter() - started
                remaining = timeout - (time.perf_counter() - started)
                if remaining <= 0:
                    timed_out = True
                    break
                time.sleep(min(interval, remaining))
        finally:
            # Only this launched child is owned. If termination does not
            # complete, fail the run instead of waiting without a deadline.
            try:
                if timed_out or child.poll() is None:
                    child.kill()
                    child.wait(timeout=_CHILD_EXIT_GRACE_SECONDS)
            finally:
                if gpu_sampler is not None:
                    gpu_close_status = gpu_sampler.close()
        if proc is not None:
            sample = _sample(proc, started, sample_threads=sample_threads, thread_limit=thread_limit)
            if sample is not None:
                samples.append(sample)
        # A file that becomes readable only after exit proves completed work,
        # not startup latency. Never substitute shutdown time for first frame.
    elapsed = time.perf_counter() - started
    peak_rss = max((s["rss_bytes"] for s in samples), default=None)
    peak_commit = max((s["commit_bytes"] for s in samples
                       if s["commit_bytes"] is not None), default=None)
    cpu_s = samples[-1]["cpu_time_s"] if samples else None
    result = {
        "clock": clock,
        "process_identity": {"pid": child.pid, "creation_time_unix_s": creation_time,
                             "creation_time_source": creation_source},
        "thread_sampling_requested": sample_threads,
        "thread_output_limit": thread_limit if sample_threads else None,
        "thread_cpu_scope": "OS cumulative CPU by observed TID; not complete thread lifetime or process CPU accounting"
                            if sample_threads else None,
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
        "cpu_time_scope": "last observed cumulative process CPU; exited-between-polls work may be unobserved",
        "samples_observed": len(samples),
        "sampling_first_elapsed_s": samples[0]["elapsed_s"] if samples else None,
        "sampling_last_elapsed_s": samples[-1]["elapsed_s"] if samples else None,
        "cpu_equivalent": cpu_s / elapsed if cpu_s is not None and elapsed else None,
        "samples": samples,
        "sampling_available": proc is not None,
        "stdout_path": str(stdout_path),
        "stderr_path": str(stderr_path),
        "stdout": stdout_path.read_text(encoding="utf-8", errors="replace"),
        "stderr": stderr_path.read_text(encoding="utf-8", errors="replace"),
    }
    if sample_gpu:
        gpu_path = output_dir / "gpu-samples.json"
        gpu_path.write_text(json.dumps(gpu_samples, indent=2, allow_nan=False) + "\n",
                            encoding="utf-8", newline="\n")
        result.update(gpu_sampling_requested=True, gpu_output_limit=gpu_limit,
                      gpu_peer_pids=list(gpu_peer_pids), gpu_samples_observed=len(gpu_samples),
                      gpu_samples_path=str(gpu_path), gpu_close_status=gpu_close_status,
                      gpu_scope="explicit PID/birth subjects; PDH per-engine rates, no sum/FPS/present or GPU ownership")
    # Preserve telemetry even when a higher correctness gate rejects the run.
    (output_dir / "samples.json").write_text(
        json.dumps(samples, indent=2) + "\n", encoding="utf-8", newline="\n")
    (output_dir / "result.json").write_text(
        json.dumps({key: value for key, value in result.items()
                    if key not in ("samples", "stdout", "stderr")}, indent=2)
        + "\n", encoding="utf-8", newline="\n")
    return result
