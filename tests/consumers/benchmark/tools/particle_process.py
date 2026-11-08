"""Guarded particle consumer execution, independent of stock host perf summaries."""
import os
from pathlib import Path
import re

from tools.benchmark_suite import read_frames
from tools.jce_determinism import DETERMINISM, MARKER, actual_backend, assert_backend, backend_family
from tools.perf_bench import read_execution_log
from tools.process_sampler import run_process

ROOT = Path(__file__).resolve().parents[4]


def particle_backend(log, requested):
    rows = re.findall(r"benchmark-particle-backend: reported=([^\r\n]+)", log)
    if len(rows) != 1:
        raise ValueError("missing or duplicated actual particle backend")
    name = rows[0].split(" at ", 1)[0].strip()
    if not re.fullmatch(r"(?:Direct3D\s*(?:11|12)|Vulkan|OpenGL(?: ES)?(?: [0-9]+(?:\.[0-9]+){1,2})?)", name):
        raise ValueError("malformed actual particle backend")
    actual = assert_backend(MARKER + name, requested)
    if actual_backend(log) is not None:
        engine = assert_backend(log, requested)
        if backend_family("".join(engine.split()).lower()) != backend_family("".join(actual.split()).lower()):
            raise ValueError("contradictory actual particle and engine backends")
    return actual


def validate_particle_execution(process, log_path, samples_path, frames, backend):
    if type(process.get("returncode")) is not int or process["returncode"] != 0 or \
            process.get("timed_out") is not False:
        raise ValueError("particle process failed or completion unavailable")
    if not Path(log_path).is_file() or not Path(samples_path).is_file():
        raise ValueError("particle log or raw frame CSV unavailable")
    log = read_execution_log(log_path, process)
    if any(marker in log for marker in ("] ERROR -", "single-instance lock already held",
                                       "CPU particle upload unavailable", "CPU instance upload unavailable")):
        raise ValueError("particle runtime reported an error, lock or unavailable upload")
    actual = particle_backend(log, backend)
    records = read_frames(samples_path, warmup=0, sample_count=None)
    if list(records["frame_indices"]) != list(range(frames)):
        raise ValueError("particle raw CSV does not cover the complete declared run")
    return actual


def run_particle_process(env_over, log_path, frames, *, exe, samples_path,
                         timeout=1800, sample_interval=.02):
    if type(frames) is not int or not 61 <= frames <= 10000:
        raise ValueError("bounded particle frame count required")
    log_path, samples_path = Path(log_path).resolve(), Path(samples_path).resolve()
    process_path = log_path.parent / (log_path.stem + "_process")
    if any(path.exists() for path in (log_path, samples_path, process_path)):
        raise ValueError("particle process requires fresh output paths")
    env = {key: value for key, value in os.environ.items() if not key.startswith("JCE_")}
    env.update(DETERMINISM)
    env.update({"JCE_PERF_LOG": "1", "JCE_LOG_FILE": str(log_path),
                "JCE_MAX_FRAMES": str(frames), "JCE_KPI_FRAME_LOG": str(samples_path),
                "JCE_KPI_FRAME_COUNT": str(frames)})
    env.update(env_over)
    process = run_process([str(Path(exe).resolve())], env=env, cwd=str(ROOT),
                          timeout=timeout, output_dir=process_path,
                          marker_path=samples_path, interval=sample_interval)
    actual = validate_particle_execution(process, log_path, samples_path, frames,
                                         env.get("JCE_BACKEND", "auto"))
    return {"backend_actual": actual, "raw_frame_count": frames,
            "process": {key: value for key, value in process.items()
                        if key not in ("stdout", "stderr", "samples")},
            "process_samples": process["samples"], "stdout": process["stdout"],
            "stderr": process["stderr"], "samples_path": str(samples_path),
            "scope": "Finite observer-on particle controls; stock aggregate performance unavailable"}
