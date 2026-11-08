#!/usr/bin/env python3
"""Core-5 JCE benchmark: raw samples, guarded execution and honest gates.

Usage:
    python tools/benchmark_suite.py --scene PATH --profile standard --kind all
Results are local artifacts under build/validation/benchmark/ (ignored by Git).
All five workload
families use the shared JCE_BENCH_AUTOSPAWN path. Editor and player
are separate tracks; this tool never labels an editor result as a game Player.
"""
from __future__ import annotations

import argparse
from array import array
import csv
import hashlib
import json
import math
import os
import platform
import re
import statistics
import struct
import subprocess
import sys
import time
import uuid
from pathlib import Path

try:
    from .perf_bench import EXE, ROOT, run, read_execution_log
    from .benchmark_scene import scene_document, viewport_pose, write_scene
    from .jce_determinism import assert_has_content, read_png
except ImportError:  # direct python tools/benchmark_suite.py
    from perf_bench import EXE, ROOT, run, read_execution_log
    from benchmark_scene import scene_document, viewport_pose, write_scene
    from jce_determinism import assert_has_content, read_png

KINDS = {"drawcall": 0, "instancing": 1, "triangle": 2,
         "entity": 3, "physics": 4}
PROFILES = {
    "quick": (30, 150, 1),
    "standard": (120, 300, 3),
    "publication": (120, 600, 7),
    "soak": (120, None, 1),
}
HEADER = ["frame_index", "phase", "ms"]
CONTEXT_HEADER = HEADER + ["context_json"]
BACKEND_LATCHED_ROWS = {
    "frame_backend_latched_cpu_submit": "cpu_submit",
    "frame_backend_latched_wait_submit": "wait_submit",
    "frame_backend_latched_wait_render": "wait_render",
}
BACKEND_LATCHED_SEMANTICS = (
    "Latest backend stats from different threads; not frame-aligned or additive CPU phases. "
    "Missing/invalid source timers are unavailable; finite zero is retained but timer "
    "availability is not exposed (zero ambiguous). No GPU/presentation completion claim.")
_CONTEXT_KEYS = {"v", "window_present", "flags_begin", "flags_end", "point_size",
                 "drawable_size", "native_size_begin", "native_size_end",
                 "backend_frame_begin", "backend_frame_end"}
_CONTEXT_V2_KEYS = _CONTEXT_KEYS | {"viewport_reported", "viewports"}
_CONTEXT_V3_KEYS = _CONTEXT_V2_KEYS | {"clock"}
_CLOCK_CONTEXT_KEYS = {"domain", "frequency_hz", "begin", "end"}
_VIEWPORT_CONTEXT_KEYS = {"source_id", "flags", "cpu_outcome", "refusal_reason",
                          "requested_size", "allocated_size", "display_size",
                          "texture_index", "image_count", "queued_commands",
                          "main_submits", "recording_submits"}
GAME_SIZE = (1280, 720)


def capture_frame(warmup):
    """Visual witness captures shortly after Play starts, outside timing runs."""
    if warmup < 30:
        raise ValueError("captured runs need at least 30 warm-up frames")
    return 20


def game_capture_environment(warmup, path):
    # Existing schedule runs on Play time; autoplay enters Play at frame ten.
    seconds = (capture_frame(warmup) - 10) * 0.016666
    return {"JCE_KPI_GAME_SHOTS": "%.6f|%s" % (seconds, path),
            "JCE_KPI_GAME_CAPTURE_SIZE": "%dx%d" % GAME_SIZE}


def simulation_started(log, host):
    marker = {"editor": "autoplay: entered Play",
              "player": "benchmark-player: entered Play"}[host]
    return marker in log


def draw_activity_valid(kind, draws, control_draws, count):
    # Only unique-material draw-call fixtures promise unbatchable submissions.
    # Identical moving meshes and rigid bodies legitimately share instanced
    # batches. Their exact count and CPU work have independent gates above.
    required = max(1, int(count * 0.05)) if kind == "drawcall" else 1
    return draws - control_draws >= required


def scatter_workload_valid(log, count):
    if count < 1:
        return False
    observed = [int(value) for value in re.findall(r"\b(\d+) instances scattered\b", log)]
    # Both editor viewports may materialize the same fixture independently.
    # Require complete exact chunk groups, including the final remainder.
    cap = 65536
    expected = [cap] * (count // cap) + ([count % cap] if count % cap else [])
    return bool(observed) and len(observed) % len(expected) == 0 and all(
        observed[start:start + len(expected)] == expected
        for start in range(0, len(observed), len(expected)))


def player_counters(text):
    rows = []
    for line in text.splitlines():
        if not line.strip():
            continue
        row = json.loads(line)
        for key in ("frame", "simulation_seconds", "monotonic_ms"):
            value = row.get(key)
            if not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise CorrectnessFailure("Player counter snapshot has an invalid clock")
        engine = row.get("engine", {})
        if engine.get("entity_count_available") is not True or engine.get("entity_count", -1) < 0:
            raise CorrectnessFailure("Player counter snapshot has no actual entity count")
        for key in ("memory_current_bytes", "memory_peak_bytes"):
            value = engine.get(key)
            if not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise CorrectnessFailure("Player counter snapshot has invalid tracked memory")
        if rows and (row["frame"] <= rows[-1]["frame"] or
                     row["monotonic_ms"] < rows[-1]["monotonic_ms"] or
                     row["simulation_seconds"] < rows[-1]["simulation_seconds"]):
            raise CorrectnessFailure("Player counter snapshots are duplicated or reordered")
        rows.append(row)
    if len(rows) < 2 or rows[0].get("event") != "initialized" or rows[0]["frame"] != 0 or \
            rows[-1].get("event") != "exit" or rows[-1]["frame"] < 10 or any(
                row.get("event") != "periodic" for row in rows[1:-1]):
        raise CorrectnessFailure("Player resource sampling did not cover initialization through exit")
    return rows


def isolated_debug_context(directory, exe, *, backend=None):
    """Preserve baseline bytes and append bounded C-INI diagnostic overrides."""
    directory = Path(directory).resolve()
    sources = (ROOT / ".config" / "jce.ini",
               Path(exe).resolve().parent / ".config" / "jce.ini")
    source = next((path for path in sources if path.is_file()), None)
    original = source.read_bytes() if source is not None else b""
    if b"\0" in original:
        raise ValueError("baseline engine configuration contains NUL bytes")
    requested = None
    overrides = {"logging.level": "debug"}
    suffix = b"[logging]\nlevel=debug\n"
    if backend is not None:
        requested = {"gl": "opengl", "gles": "opengles"}.get(backend.casefold(), backend.casefold())
        if requested not in ("d3d11", "d3d12", "vulkan", "opengl", "opengles", "metal"):
            raise ValueError("isolated diagnostics require a fixed renderer backend")
        overrides["renderer.backend"] = requested
        suffix += ("[renderer]\nbackend=" + requested + "\n").encode("ascii")
    separator = b"\n" if original and not original.endswith((b"\n", b"\r")) else b""
    payload = original + separator + suffix
    cwd = directory / "process-cwd"
    cwd.mkdir(parents=True, exist_ok=False)
    config = cwd / ".config" / "jce.ini"
    config.parent.mkdir()
    with config.open("xb") as output:
        output.write(payload)
    return {"cwd": str(cwd), "engine_config_file": str(config),
            "effective_baseline": str(source.resolve()) if source is not None else "engine_defaults",
            "effective_baseline_sha256": hashlib.sha256(original).hexdigest() if source is not None else None,
            "engine_config_sha256": hashlib.sha256(payload).hexdigest(),
            "selected_logging_level": "debug", "requested_backend": requested,
            "intended_overrides": overrides,
            "config_validation_policy": "byte_exact_before_and_after_each_child",
            "config_checks": [],
            "observer_mode": "debug_instrumented_diagnostic",
            "quiet_production_throughput": False}


def validate_observer_config(observer, *, boundary="external"):
    """Reject any actual child configuration change without repairing evidence."""
    if observer.get("config_validation_policy") != "byte_exact_before_and_after_each_child":
        raise CorrectnessFailure("observer configuration validation policy is missing")
    try:
        actual = sha256_file(observer["engine_config_file"])
    except (OSError, KeyError, TypeError) as exc:
        raise CorrectnessFailure("observer configuration changed or is unavailable") from exc
    expected = observer.get("engine_config_sha256")
    observer.setdefault("config_checks", []).append(
        {"boundary": boundary, "expected_sha256": expected,
         "actual_sha256": actual, "unchanged": actual == expected})
    if actual != expected:
        raise CorrectnessFailure("observer configuration changed")
    return actual


def run_observer_child(environment, log_path, observer, *, phase, **options):
    """Keep all child launches under the same actual byte-preservation guard."""
    validate_observer_config(observer, boundary=phase + "_before")
    try:
        return run(environment, log_path, cwd=observer["cwd"], **options)
    finally:
        validate_observer_config(observer, boundary=phase + "_after")


def host_render_environment(host, directory, *, witness=False):
    directory = Path(directory).resolve()
    if host == "player":
        return {"JCE_BENCH_COUNTERS": str(directory / "engine_counters.jsonl"),
                **({"JCE_BENCH_PLAYER_SHOT": str(directory / "frame.png")} if witness else {})}
    profile = (directory / "editor-profile").resolve()
    profile.mkdir(parents=True, exist_ok=True)
    environment = (game_capture_environment(30, directory / "frame.png") if witness
                   else fixed_game_render_environment(directory))
    return {"JCE_EDITOR_PROFILE_DIR": str(profile), **environment}


def editor_profile_confirmed(log, expected):
    if not isinstance(expected, str) or not expected:
        return False
    observed = re.findall(r"(?m)\beditor_config: profile directory: ([^\r\n]*)", log)
    confirmed = []
    for value in observed:
        profile, separator, source = value.rpartition(" at ")
        if separator and re.fullmatch(r"[^\r\n]+\.(?:c|cpp|cc|cxx|h|hpp):\d+", source):
            value = profile
        confirmed.append(value)
    return bool(confirmed) and all(value == expected for value in confirmed)


def run_visual_witness(extra, directory, *, exe, timeout, expected_workload=None,
                       host="editor", observer=None):
    """A separate process proves pixels without PNG work in the timing run."""
    directory = Path(directory).resolve()
    observer = isolated_debug_context(directory, exe, backend=extra.get("JCE_BACKEND")) if observer is None else observer
    visual_extra = dict(extra)
    screenshot = directory / "frame.png"
    visual_extra.update(host_render_environment(host, directory, witness=True))
    witness = run_observer_child(visual_extra, directory / "visual.log", observer,
                                 phase="visual", frames=180, timeout=timeout, exe=exe,
                                 expected_workload=expected_workload)
    log = read_execution_log(directory / "visual.log")
    if host == "editor" and not editor_profile_confirmed(log, visual_extra["JCE_EDITOR_PROFILE_DIR"]):
        raise CorrectnessFailure("visual witness did not confirm the isolated editor profile")
    if "] ERROR -" in log or not simulation_started(log, host):
        raise CorrectnessFailure("visual witness logged an ERROR or failed to enter Play")
    if png_dimensions(screenshot) != GAME_SIZE:
        raise CorrectnessFailure("visual witness has incorrect Game View dimensions")
    return witness


def fixed_game_render_environment(directory):
    # The existing capture controller keeps its fixed render size even while
    # the sole scheduled shot is pending. This timestamp lies beyond any
    # supported finite timing run, so no readback or PNG encode occurs there.
    return {"JCE_KPI_GAME_SHOTS": "1000000000000|%s" %
            (directory / "unscheduled.png"),
            "JCE_KPI_GAME_CAPTURE_SIZE": "%dx%d" % GAME_SIZE}


class CorrectnessFailure(RuntimeError):
    """The run completed, but the requested workload did not execute correctly."""


def source_from_manifest(prior, expected_hash):
    """Accept current nested identity or the original schema-2 flat identity."""
    if prior.get("executable_sha256") != expected_hash:
        raise ValueError("B manifest must match executable hash")
    identity = prior.get("source")
    if not identity and prior.get("engine_commit"):
        identity = {"commit": prior["engine_commit"],
                    "version": prior.get("engine_version"),
                    "branch": prior.get("branch"),
                    "source_dirty": prior.get("source_dirty"),
                    "dependency_authority_sha256": prior.get("dependency_authority_sha256")}
    if not isinstance(identity, dict) or not identity.get("commit"):
        raise ValueError("B manifest must contain source identity")
    return identity


def _git(*args):
    result = subprocess.run(["git", *args], cwd=ROOT, text=True,
                            capture_output=True, check=True)
    return result.stdout.strip()


def source_identity():
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(JCE VERSION (\d+\.\d+\.\d+) LANGUAGES C\)", cmake)
    if not match:
        raise RuntimeError("JCE version missing from CMakeLists.txt")
    dirty = bool(_git("status", "--porcelain"))
    pins = ("conanfile.py", "THIRD_PARTY_LICENSES.md",
            "contracts/conan-source-policy.json")
    return {"commit": _git("rev-parse", "HEAD"), "version": match.group(1),
            "branch": _git("branch", "--show-current"), "source_dirty": dirty,
            "worktree_patch_sha256": hashlib.sha256(
                _git("diff", "--binary", "HEAD").encode("utf-8")).hexdigest(),
            "untracked_source_sha256": {
                name: sha256_file(ROOT / name)
                for name in _git("ls-files", "--others", "--exclude-standard").splitlines()
                if (ROOT / name).is_file()},
            "dependency_authority_sha256": {
                name: sha256_file(ROOT / name) for name in pins}}


def machine_manifest():
    result = {"os": platform.platform(), "machine": platform.machine(),
              "python": platform.python_version(), "cpu": platform.processor(),
              "logical_cores": os.cpu_count(), "ram_bytes": None,
              "gpu": None, "vram_bytes": None, "driver": None,
              "gpu_adapter_inventory": []}
    try:
        import psutil
        result["ram_bytes"] = psutil.virtual_memory().total
    except ImportError:
        pass
    if platform.system() == "Windows":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as cpu:
                result["cpu"] = winreg.QueryValueEx(cpu, "ProcessorNameString")[0]
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                r"SYSTEM\CurrentControlSet\Control\Video") as video:
                for index in range(winreg.QueryInfoKey(video)[0]):
                    name = winreg.EnumKey(video, index)
                    try:
                        with winreg.OpenKey(video, name + r"\0000") as adapter:
                            result["gpu_adapter_inventory"].append({
                                "name": winreg.QueryValueEx(adapter, "DriverDesc")[0],
                                "driver": winreg.QueryValueEx(adapter, "DriverVersion")[0]})
                    except OSError:
                        continue
        except OSError:
            pass
    # Inventory is not proof of which physical adapter the renderer selected.
    return result


def _frame_context_v2(packet):
    """Validate independent bounded CPU viewport facts without inferring success."""
    if not isinstance(packet, dict) or set(packet) != _CONTEXT_V2_KEYS:
        raise ValueError("frame context v2 has missing or unknown keys")
    if type(packet["v"]) is not int or packet["v"] != 2:
        raise ValueError("frame context v2 has invalid version")
    if type(packet["window_present"]) is not bool or type(packet["viewport_reported"]) is not bool:
        raise ValueError("frame context v2 has invalid window or viewport presence")
    for key in ("flags_begin", "flags_end", "backend_frame_begin", "backend_frame_end"):
        value = packet[key]
        if value is not None and (type(value) is not int or not 0 <= value <= 0xffffffff):
            raise ValueError("frame context v2 has invalid " + key)
    for key in ("point_size", "drawable_size", "native_size_begin", "native_size_end"):
        value = packet[key]
        if value is not None and (not isinstance(value, list) or len(value) != 2 or
                any(type(n) is not int or not 0 < n <= 0xffffffff for n in value)):
            raise ValueError("frame context v2 has invalid " + key)
    window_keys = ("flags_begin", "flags_end", "point_size", "drawable_size",
                   "native_size_begin", "native_size_end")
    if not packet["window_present"] and any(packet[key] is not None for key in window_keys):
        raise ValueError("frame context v2 reports state for an absent window")
    viewports = packet["viewports"]
    if not isinstance(viewports, list) or len(viewports) > 4:
        raise ValueError("frame context v2 has invalid viewports")
    if not packet["viewport_reported"] and viewports:
        raise ValueError("frame context v2 reports viewports without availability")
    sources = set()
    for viewport in viewports:
        if not isinstance(viewport, dict) or set(viewport) != _VIEWPORT_CONTEXT_KEYS:
            raise ValueError("frame context v2 viewport has missing or unknown keys")
        for key, minimum, maximum in (("source_id", 1, 0xffffffff), ("flags", 0, 0xff),
                                      ("cpu_outcome", 0, 3), ("refusal_reason", 0, 0xffffffff)):
            value = viewport[key]
            if type(value) is not int or not minimum <= value <= maximum:
                raise ValueError("frame context v2 viewport has invalid " + key)
        if viewport["source_id"] in sources:
            raise ValueError("frame context v2 has duplicate viewport source_id")
        sources.add(viewport["source_id"])
        flags = viewport["flags"]
        for key, bit in (("requested_size", 0), ("allocated_size", 1), ("display_size", 3)):
            value = viewport[key]
            if flags & (1 << bit):
                if not isinstance(value, list) or len(value) != 2 or \
                        any(type(n) is not int or not 0 < n <= 0xffffffff for n in value):
                    raise ValueError("frame context v2 viewport has invalid " + key)
            elif value is not None:
                raise ValueError("frame context v2 viewport has unavailable " + key)
        for key, bit, maximum in (("texture_index", 4, 65534), ("image_count", 5, 0xffffffff),
                                  ("queued_commands", 6, 0xffffffff), ("main_submits", 7, 0xffffffff),
                                  ("recording_submits", 7, 0xffffffff)):
            value = viewport[key]
            if flags & (1 << bit):
                if type(value) is not int or not 0 <= value <= maximum:
                    raise ValueError("frame context v2 viewport has invalid " + key)
            elif value is not None:
                raise ValueError("frame context v2 viewport has unavailable " + key)
    return packet


def _frame_context_v3(packet):
    """Retain exact Engine counter strings alongside the unchanged v2 facts."""
    if not isinstance(packet, dict) or set(packet) != _CONTEXT_V3_KEYS:
        raise ValueError("frame context v3 has missing or unknown keys")
    if type(packet["v"]) is not int or packet["v"] != 3:
        raise ValueError("frame context v3 has invalid version")
    base = {key: value for key, value in packet.items() if key != "clock"}
    base["v"] = 2
    _frame_context_v2(base)
    clock = packet["clock"]
    if not isinstance(clock, dict) or set(clock) != _CLOCK_CONTEXT_KEYS:
        raise ValueError("frame context v3 clock has missing or unknown keys")
    if clock["domain"] != "jce-performance-counter":
        raise ValueError("frame context v3 clock has invalid domain")
    values = {}
    for key in ("frequency_hz", "begin", "end"):
        value = clock[key]
        if type(value) is not str or len(value) > 20 or \
                re.fullmatch(r"0|[1-9][0-9]*", value) is None or int(value) > 0xffffffffffffffff:
            raise ValueError("frame context v3 clock has invalid " + key)
        values[key] = int(value)
    if values["frequency_hz"] == 0 or values["end"] < values["begin"]:
        raise ValueError("frame context v3 clock has invalid frequency or interval")
    return packet


def _frame_context(text):
    """Validate private context packets without guessing unknown state."""
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate frame context key: " + key)
            result[key] = value
        return result

    packet = json.loads(text, object_pairs_hook=unique_object)
    if isinstance(packet, dict) and type(packet.get("v")) is int and packet["v"] == 3:
        return _frame_context_v3(packet)
    if isinstance(packet, dict) and type(packet.get("v")) is int and packet["v"] == 2:
        return _frame_context_v2(packet)
    if not isinstance(packet, dict) or set(packet) != _CONTEXT_KEYS:
        raise ValueError("frame context has missing or unknown keys")
    if type(packet["v"]) is not int or packet["v"] != 1 or \
            type(packet["window_present"]) is not bool:
        raise ValueError("frame context has invalid version or window presence")
    for key in ("flags_begin", "flags_end", "backend_frame_begin", "backend_frame_end"):
        value = packet[key]
        if value is not None and (type(value) is not int or not 0 <= value <= 0xffffffff):
            raise ValueError("frame context has invalid " + key)
    for key in ("point_size", "drawable_size", "native_size_begin", "native_size_end"):
        value = packet[key]
        if value is not None and (not isinstance(value, list) or len(value) != 2 or
                any(type(n) is not int or not 0 < n <= 0xffffffff for n in value)):
            raise ValueError("frame context has invalid " + key)
    window_keys = ("flags_begin", "flags_end", "point_size", "drawable_size",
                   "native_size_begin", "native_size_end")
    if not packet["window_present"] and any(packet[key] is not None for key in window_keys):
        raise ValueError("frame context reports state for an absent window")
    return packet


def read_frames(path, *, warmup, sample_count, allow_zero_wall=False):
    """Stream KPI CSV; optional raw admission retains finite zero intervals.

    Steady benchmark measurements reject zero wall time by default. Raw capture
    completeness may retain valid zero timer intervals without dropping rows.
    """
    if warmup < 0 or (sample_count is not None and sample_count < 1):
        raise ValueError("invalid warm-up or sample count")
    frame_ms, cpu_ms, gpu_ms = array("d"), array("d"), array("d")
    phase_ms, selected, contexts = {}, array("Q"), []
    backend_latched_ms = {}
    current_id = previous_id = None
    entry = {}
    context = None
    context_schema = None
    clock_identity = previous_clock_end = None
    total_frames = 0

    def finish_frame():
        nonlocal total_frames
        if current_id is None:
            return
        if "frame_wall" not in entry:
            raise ValueError("frame %d has no frame_wall sample" % current_id)
        if total_frames >= warmup and (sample_count is None or
                                       len(selected) < sample_count):
            if not allow_zero_wall and entry["frame_wall"] == 0:
                raise ValueError("steady-state frame time is zero")
            old_length = len(selected)
            selected.append(current_id)
            frame_ms.append(entry["frame_wall"])
            cpu_ms.append(entry.get("frame_cpu", math.nan))
            gpu_ms.append(entry.get("frame_gpu", math.nan))
            if not backend_latched_ms and any(row in entry for row in BACKEND_LATCHED_ROWS):
                backend_latched_ms.update({name: array("d", [math.nan]) * old_length
                                            for name in BACKEND_LATCHED_ROWS.values()})
            if backend_latched_ms:
                for row, name in BACKEND_LATCHED_ROWS.items():
                    backend_latched_ms[name].append(entry.get(row, math.nan))
            if extended:
                contexts.append(context)
            for name in phase_ms:
                phase_ms[name].append(entry.get(name, math.nan))
            for name, value in entry.items():
                if not name.startswith("frame_") and name not in phase_ms:
                    phase_ms[name] = array("d", [math.nan]) * old_length
                    phase_ms[name].append(value)
        total_frames += 1

    # Count the actual validated header, including its newline, for the writer's
    # payload accounting. Extended captures must not subtract the legacy header.
    with Path(path).open("rb") as raw:
        raw_header = raw.readline(256)
    with Path(path).open(newline="", encoding="utf-8") as stream:
        reader = csv.reader(stream)
        header = next(reader, None)
        if header not in (HEADER, CONTEXT_HEADER):
            raise ValueError("raw KPI header is missing or changed")
        expected_header = ",".join(header).encode("ascii")
        if raw_header not in (expected_header + b"\n", expected_header + b"\r\n"):
            raise ValueError("raw KPI header has noncanonical physical encoding")
        extended = header == CONTEXT_HEADER
        for line in reader:
            if len(line) != len(header):
                raise ValueError("raw KPI row has wrong field count")
            frame, phase, ms = int(line[0]), line[1], float(line[2])
            if frame < 0 or not phase or not math.isfinite(ms) or ms < 0:
                raise ValueError("raw KPI row has invalid value")
            if current_id != frame:
                finish_frame()
                if previous_id is not None and frame != previous_id + 1:
                    raise ValueError("raw frame sequence has gaps or reordering")
                previous_id = current_id = frame
                entry = {}
                context = None
            if phase in entry:
                raise ValueError("duplicate KPI phase %s at frame %d" %
                                 (phase, frame))
            entry[phase] = ms
            if extended:
                if phase == "frame_wall":
                    context = _frame_context(line[3])
                    schema = {1: "window_v1", 2: "window_viewports_v2",
                              3: "window_viewports_clock_v3"}[context["v"]]
                    if context_schema is not None and schema != context_schema:
                        raise ValueError("mixed frame context schemas in raw KPI CSV")
                    context_schema = schema
                    if context["v"] == 3:
                        clock = context["clock"]
                        identity = (clock["domain"], clock["frequency_hz"])
                        begin, end = int(clock["begin"]), int(clock["end"])
                        if clock_identity is not None and identity != clock_identity:
                            raise ValueError("frame context v3 clock domain or frequency changed")
                        if previous_clock_end is not None and begin < previous_clock_end:
                            raise ValueError("frame context v3 clock intervals overlap or regress")
                        elapsed_ms = (end - begin) * 1000.0 / int(clock["frequency_hz"])
                        if not math.isclose(ms, elapsed_ms, rel_tol=1e-12, abs_tol=0.0000501):
                            raise ValueError("frame context v3 clock duration does not match frame_wall")
                        clock_identity, previous_clock_end = identity, end
                elif line[3]:
                    raise ValueError("frame context is only valid on frame_wall")
    finish_frame()
    needed = warmup + (sample_count if sample_count is not None else 1)
    if total_frames < needed:
        raise ValueError("only %d frame intervals; need %d warm-up + %s steady" %
                         (total_frames, warmup,
                          sample_count if sample_count is not None else "one"))
    samples = {"frame_indices": selected, "frame_ms": frame_ms,
            "cpu_frame_ms": cpu_ms, "gpu_frame_ms": gpu_ms,
            "phase_ms": phase_ms, "frame_context": contexts if extended else None,
            "context_schema": context_schema,
            "csv_header_bytes": len(raw_header)}
    if backend_latched_ms:
        samples.update(backend_latched_ms=backend_latched_ms,
                       backend_latched_semantics=BACKEND_LATCHED_SEMANTICS)
    return samples


def percentile(values, q):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, math.ceil(q * len(ordered)) - 1)]


def frame_summary(samples):
    values = samples["frame_ms"]
    worst1 = sorted(values, reverse=True)[:max(1, math.ceil(len(values) * 0.01))]
    worst01 = sorted(values, reverse=True)[:max(1, math.ceil(len(values) * 0.001))]
    cpu = [x for x in samples["cpu_frame_ms"] if x is not None and math.isfinite(x) and x > 0]
    gpu = [x for x in samples["gpu_frame_ms"] if x is not None and math.isfinite(x) and x > 0]
    phase_medians = {
        name: statistics.median(valid)
        for name, series in samples["phase_ms"].items()
        if (valid := [x for x in series if x is not None and math.isfinite(x)])
    }
    summary = {
        "phase_median_ms": phase_medians,
        "sample_count": len(values),
        "frame_mean_ms": statistics.fmean(values),
        "frame_median_ms": statistics.median(values),
        "frame_p95_ms": percentile(values, 0.95),
        "frame_p99_ms": percentile(values, 0.99),
        "frame_p999_ms": percentile(values, 0.999),
        "frame_max_ms": max(values),
        "one_percent_low_fps": 1000.0 / statistics.fmean(worst1),
        "point_one_percent_low_fps": 1000.0 / statistics.fmean(worst01),
        "over_16_67_ms": sum(x > 16.67 for x in values),
        "over_33_33_ms": sum(x > 33.33 for x in values),
        "over_50_ms": sum(x > 50 for x in values),
        "cpu_median_ms": statistics.median(cpu) if cpu else None,
        "gpu_median_ms": statistics.median(gpu) if gpu else None,
    }
    if samples.get("backend_latched_ms"):
        backend_medians = {}
        for name in BACKEND_LATCHED_ROWS.values():
            valid = [x for x in samples["backend_latched_ms"].get(name, ())
                     if x is not None and math.isfinite(x) and x >= 0]
            backend_medians[name] = statistics.median(valid) if valid else None
        summary.update(backend_latched_median_ms=backend_medians,
                       backend_latched_semantics=BACKEND_LATCHED_SEMANTICS)
    return summary


def soak_windows(samples, seconds=30.0):
    """Non-overlapping wall-time windows retain drift hidden by one median."""
    windows = []
    start = 0
    elapsed = window_start = 0.0
    values = samples["frame_ms"]
    for index, value in enumerate(values):
        elapsed += value / 1000.0
        if elapsed - window_start >= seconds or index == len(values) - 1:
            end = index + 1
            sliced = {key: samples[key][start:end] for key in
                      ("frame_ms", "cpu_frame_ms", "gpu_frame_ms", "frame_indices")}
            sliced["phase_ms"] = {key: series[start:end]
                                  for key, series in samples["phase_ms"].items()}
            sliced["backend_latched_ms"] = {key: series[start:end]
                                            for key, series in samples.get("backend_latched_ms", {}).items()}
            windows.append({"start_s": window_start, "end_s": elapsed,
                            "frame": frame_summary(sliced)})
            start, window_start = end, elapsed
    return windows


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def png_dimensions(path):
    with Path(path).open("rb") as stream:
        header = stream.read(24)
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or \
            header[12:16] != b"IHDR":
        raise ValueError("capture is missing or not a PNG: %s" % path)
    return struct.unpack(">II", header[16:24])


def capture_info(path):
    width, height = png_dimensions(path)
    if width < 64 or height < 64:
        raise ValueError("capture dimensions are implausible: %dx%d" %
                         (width, height))
    return {"file": str(path), "width": width, "height": height,
            "sha256": sha256_file(path)}


def visual_delta_fraction(control_image, workload_image):
    """Conservative sampled liveness check in the central Game View region."""
    width, height, channels, control = control_image
    other_width, other_height, other_channels, workload = workload_image
    if (width != other_width or height != other_height or
            channels != other_channels):
        raise ValueError("control and workload captures have different formats")
    changed = checked = 0
    for y in range(int(height * 0.05), int(height * 0.95), 16):
        for x in range(int(width * 0.05), int(width * 0.95), 16):
            offset = (y * width + x) * channels
            checked += 1
            if max(abs(control[offset + channel] - workload[offset + channel])
                   for channel in range(3)) > 30:
                changed += 1
    if not checked:
        raise ValueError("capture is too small for visual liveness sampling")
    return changed / checked


def visual_delta_files(control_path, workload_path):
    """Use an optional image decoder for fast post-run sampling."""
    try:
        from PIL import Image
    except ImportError:
        candidate = read_png(workload_path)
        width, height = candidate[:2]
        assert_has_content(candidate, workload_path,
                           y0=int(height * 0.05), y1=int(height * 0.95),
                           x0=int(width * 0.05), x1=int(width * 0.95))
        return visual_delta_fraction(read_png(control_path), candidate)
    with Image.open(control_path) as original, \
            Image.open(workload_path) as candidate:
        if original.size != candidate.size:
            raise ValueError("control and workload captures differ in size")
        width, height = original.size
        candidate_rgb = candidate.convert("RGB")
        assert_has_content((width, height, 3, candidate_rgb.tobytes()),
                           workload_path, y0=int(height * 0.05),
                           y1=int(height * 0.95), x0=int(width * 0.05),
                           x1=int(width * 0.95))
        box = (int(width * 0.05), int(height * 0.05),
               int(width * 0.95), int(height * 0.95))
        sample_size = (max(1, (box[2] - box[0]) // 16),
                       max(1, (box[3] - box[1]) // 16))
        control = original.convert("RGB").crop(box).resize(
            sample_size, Image.Resampling.NEAREST)
        workload = candidate_rgb.crop(box).resize(
            sample_size, Image.Resampling.NEAREST)
        checked = sample_size[0] * sample_size[1]
        before = control.tobytes()
        after = workload.tobytes()
        changed = sum(max(abs(before[i + channel] - after[i + channel])
                          for channel in range(3)) > 30
                      for i in range(0, len(before), 3))
        return changed / checked


def _write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=True) + "\n",
                    encoding="utf-8", newline="\n")


def run_control(*, exe, scene, kind, count, warmup, sample_count,
                backend, out, timeout, capture, spread, directory=None, host="editor"):
    control_dir = Path(directory or out / "controls" / ("%s-%d-%s" %
                                      (kind, count, uuid.uuid4().hex[:8]))).resolve()
    control_dir.mkdir(parents=True, exist_ok=False)
    observer = isolated_debug_context(control_dir, exe, backend=backend)
    raw = control_dir / "samples.csv"
    screenshot = control_dir / "frame.png"
    extra = {"JCE_SCENE": str(Path(scene).resolve()), "JCE_MULTI_INSTANCE": "1",
             "JCE_BACKEND": backend, "JCE_DBG_FOCUS_GAME": "1",
             "JCE_BENCH_CAM": viewport_pose(kind, count, spread)}
    extra["JCE_DBG_AUTOPLAY"] = "10"
    if kind == "physics":
        extra["JCE_PHYSICS_MAX_BODIES"] = str(max(4096, count + 64))
    if host == "editor":
        extra.update(host_render_environment(host, control_dir))
    intent = {"host": host, "control": True,
              "kind": kind, "count": count, "environment": extra, "observer": observer}
    _write_json(control_dir / "intent.json", intent)
    if capture:
        run_visual_witness(extra, control_dir, exe=exe, timeout=timeout, host=host, observer=observer)
        visual_log = read_execution_log(control_dir / "visual.log")
        if "benchmark-spawn:" in visual_log:
            raise ValueError("visual negative control accidentally spawned a benchmark")
        extra.update(host_render_environment(host, control_dir))
    _write_json(control_dir / "intent.json", intent)
    metrics = run_observer_child(extra, control_dir / "engine.log", observer,
                                 phase="timing", frames=warmup + sample_count + 1,
                                 timeout=timeout, exe=exe, samples_path=raw)
    log = read_execution_log(control_dir / "engine.log", metrics.get("process"))
    if host == "editor" and not editor_profile_confirmed(log, extra["JCE_EDITOR_PROFILE_DIR"]):
        raise ValueError("negative control did not confirm the isolated editor profile")
    if "benchmark-spawn:" in log:
        raise ValueError("negative control accidentally spawned a benchmark")
    if "] ERROR -" in log or not simulation_started(log, host):
        raise ValueError("negative control logged an ERROR or failed to enter Play")
    if capture and png_dimensions(screenshot) != GAME_SIZE:
        raise ValueError("negative control has incorrect Game View dimensions")
    samples = read_frames(raw, warmup=warmup, sample_count=sample_count)
    control = {
        "status": "PASS", "kind": kind, "count": count, "observer": observer,
        "scene": str(Path(scene).resolve()), "backend_actual": metrics["backend_actual"],
        "frame": frame_summary(samples), "draws_representative": metrics["draws"],
        "directory": str(control_dir),
        "capture": capture_info(screenshot) if capture else None,
    }
    _write_json(control_dir / "summary.json", control)
    return control


def run_one(*, exe, scene, kind, count, profile, warmup, sample_count,
            backend, out, index, identity, hardware, timeout, spread,
            executable_sha256, control, capture, soak_seconds=None,
            variant="A", directory=None, build_variant="release", host="editor"):
    run_id = "%s-%s-%d-%s" % (
        time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()),
        kind, index, uuid.uuid4().hex[:8])
    target = Path(directory or out / run_id).resolve()
    run_id = target.name
    target.mkdir(parents=True, exist_ok=False)
    observer = isolated_debug_context(target, exe, backend=backend)
    raw = target / "samples.csv"
    screenshot = target / "frame.png"
    extra = {"JCE_SCENE": str(Path(scene).resolve()), "JCE_BENCH_AUTOSPAWN": "%d,%d" %
             (KINDS[kind], count), "JCE_BENCH_ISOLATE": "1",
             "JCE_MULTI_INSTANCE": "1", "JCE_BACKEND": backend,
             "JCE_DBG_FOCUS_GAME": "1",
             "JCE_BENCH_CAM": viewport_pose(kind, count, spread)}
    if spread > 0:
        extra["JCE_BENCH_SPREAD"] = str(spread)
    extra["JCE_DBG_AUTOPLAY"] = "10"
    if kind == "physics":
        extra["JCE_PHYSICS_MAX_BODIES"] = str(max(4096, count + 64))
    if host == "editor":
        extra.update(host_render_environment(host, target))
    intent = {"host": host, "source": identity,
              "profile": profile, "variant": variant, "kind": kind,
              "count": count, "environment": extra,
              "executable_sha256": executable_sha256, "observer": observer}
    _write_json(target / "intent.json", intent)
    if capture:
        run_visual_witness(extra, target, exe=exe, timeout=timeout,
                           expected_workload=(KINDS[kind], count), host=host, observer=observer)
        extra.update(host_render_environment(host, target))
    _write_json(target / "intent.json", intent)
    # One additional engine frame makes the last interval and 120-frame perf
    # report available even though the KPI snapshot rotates at frame start.
    frames = 2000000000 if soak_seconds is not None else \
             warmup + sample_count + 1
    metrics = run_observer_child(extra, target / "engine.log", observer,
                                 phase="timing", frames=frames, timeout=timeout,
                                 exe=exe, samples_path=raw,
                                 expected_workload=(KINDS[kind], count),
                                 max_seconds=soak_seconds)
    samples = read_frames(raw, warmup=warmup, sample_count=sample_count)
    summary = frame_summary(samples)
    engine_log = read_execution_log(target / "engine.log", metrics.get("process"))
    if host == "editor" and not editor_profile_confirmed(engine_log, extra["JCE_EDITOR_PROFILE_DIR"]):
        raise CorrectnessFailure("measurement did not confirm the isolated editor profile")
    matched_keys = ("effective_baseline_sha256", "engine_config_sha256",
                    "selected_logging_level", "observer_mode", "requested_backend",
                    "intended_overrides", "config_validation_policy")
    if any(control.get("observer", {}).get(key) != observer[key] for key in matched_keys):
        raise CorrectnessFailure("measurement configuration differs from negative control")
    if "] ERROR -" in engine_log:
        raise CorrectnessFailure("engine logged an ERROR during the benchmark")
    if soak_seconds is not None and "JCE_MAX_SECONDS reached" not in engine_log:
        raise CorrectnessFailure("soak ended without reaching its timed limit")
    if not simulation_started(engine_log, host):
        raise CorrectnessFailure("the simulation did not enter Play mode")
    resource_rows = player_counters((target / "engine_counters.jsonl").read_text(encoding="utf-8")) \
                    if host == "player" else None
    if resource_rows is not None:
        _write_json(target / "engine_counters.json", resource_rows)
    required_phase = {"drawcall": "draw_submit", "instancing": "sr_foliage",
                      "triangle": "sr_foliage", "entity": "ecs_move",
                      "physics": "physics"}[kind]
    if summary["phase_median_ms"].get(required_phase, 0) <= 0:
        raise CorrectnessFailure("%s workload did not exercise %s" %
                                 (kind, required_phase))
    if kind in ("instancing", "triangle"):
        if not scatter_workload_valid(engine_log, count):
            raise CorrectnessFailure(
                "scatter workload mismatch: requested %d; incomplete or incorrect chunk groups" % count)
    if metrics["draws"] <= 0:
        raise CorrectnessFailure("renderer submitted no draws")
    baseline_phase = control["frame"]["phase_median_ms"].get(required_phase, 0)
    observed_phase = summary["phase_median_ms"][required_phase]
    if observed_phase <= baseline_phase * 1.25 + 0.01:
        raise CorrectnessFailure(
            "%s phase did not rise above negative control (%.4f vs %.4f ms)" %
            (required_phase, observed_phase, baseline_phase))
    if profile != "soak" and kind in ("drawcall", "entity", "physics") and not (
            draw_activity_valid(kind, metrics["draws"], control["draws_representative"], count)):
        raise CorrectnessFailure("draw count did not rise above negative control")
    actual = metrics["backend_actual"]
    if capture and png_dimensions(screenshot) != GAME_SIZE:
        raise CorrectnessFailure("incorrect Game View capture dimensions")
    visual = capture_info(screenshot) if capture else None
    visual_changed = None
    if capture:
        visual_changed = visual_delta_files(
            control["capture"]["file"], screenshot)
        if visual_changed < 0.02:
            raise CorrectnessFailure(
                "Game View did not visibly change from the negative control "
                "(%.3f%% sampled pixels)" % (visual_changed * 100.0))
    _write_json(target / "process_samples.json", metrics["process_samples"])
    manifest = {
        "schema_version": 2, "run_id": run_id, "variant": variant,
        "timing_capture_policy": "separate_visual_witness",
        "observer_mode": "debug_instrumented_diagnostic",
        "quiet_production_throughput": False,
        "engine": "JCE",
        "host": host, "engine_commit": identity.get("commit"),
        "engine_version": identity.get("version"), "branch": identity.get("branch"),
        "source_dirty": identity.get("source_dirty"), "source": identity,
        "build_variant": build_variant, "profile": profile,
        "dependency_authority_sha256": identity.get("dependency_authority_sha256"),
        "render_pipeline_log": [line for line in engine_log.splitlines()
                                if "render_pipeline: applied:" in line],
        "scenario": "B%d-%s" % (KINDS[kind], kind),
        "workload": {"kind": kind, "count": count, "scene": str(scene),
                     "scene_sha256": sha256_file(scene), "spread": spread,
                     "warmup_frames": warmup,
                     "capture_play_seconds_requested": 0.16666 if capture else None,
                     "capture_host": "separate_%s_process" % host if capture else None,
                     "game_render_size": list(GAME_SIZE) if capture else None,
                     "sample_frames": len(samples["frame_ms"]),
                     "requested_soak_seconds": soak_seconds,
                     "physics_max_bodies": extra.get("JCE_PHYSICS_MAX_BODIES"),
                     "editor_camera_pose": extra["JCE_BENCH_CAM"]},
        "backend_requested": backend, "backend_actual": actual, "observer": observer,
        "negative_control": str(control["directory"]),
        "hardware": hardware, "executable": str(exe),
        "executable_bytes": exe.stat().st_size,
        "executable_sha256": executable_sha256,
        "cross_engine_comparable": False,
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "raw_file": "samples.csv", "process_file": "process_samples.json",
        "engine_counters_file": "engine_counters.json" if resource_rows is not None else None,
        "log_file": "engine.log",
        "visual_log_file": "visual.log" if capture else None,
        "correctness": {"clean_exit": True, "fresh_workload_marker": True,
                        "actual_backend_verified": True,
                        "raw_frames_contiguous": True,
                        "negative_control_verified": True,
                        "visual_control": {"status": "LIVENESS_PASS",
                                           "changed_fraction": visual_changed,
                                           "workload": visual,
                                           "negative_control": control["capture"],
                                           "full_visual_review": "UNVERIFIED"}
                        if capture else None},
    }
    _write_json(target / "manifest.json", manifest)
    if capture and ((target / "unscheduled.png").exists() or
                    "readback capture saved:" in engine_log):
        raise CorrectnessFailure("an unexpected capture occurred in the timing run")
    process_rows = metrics["process_samples"]
    memory_trend = None
    if soak_seconds is not None and len(process_rows) >= 10:
        tail = max(1, len(process_rows) // 5)
        first = process_rows[:tail]
        last = process_rows[-tail:]
        first_rss = statistics.median(row["rss_bytes"] for row in first)
        last_rss = statistics.median(row["rss_bytes"] for row in last)
        interval_s = (statistics.median(row["elapsed_s"] for row in last) -
                      statistics.median(row["elapsed_s"] for row in first))
        memory_trend = {"rss_first_fifth_median_bytes": first_rss,
                        "rss_last_fifth_median_bytes": last_rss,
                        "rss_delta_bytes": last_rss - first_rss,
                        "rss_growth_bytes_per_minute":
                            (last_rss - first_rss) * 60.0 / interval_s
                            if interval_s > 0 else None}
    _write_json(target / "summary.json", {
        "status": "PASS", "frame": summary,
        "soak": {"requested_seconds": soak_seconds,
                 "observed_process_seconds": metrics["process"]["elapsed_s"],
                 "memory_trend": memory_trend,
                 "wall_time_windows": soak_windows(samples)}
                if soak_seconds is not None else None,
        "process": metrics["process"],
        "engine_counters": {"status": "AVAILABLE", "samples": resource_rows}
                           if resource_rows is not None else
                           {"status": "UNAVAILABLE", "reason": "this host does not export SDK counter snapshots"},
        "counters": {
            "draws_representative": metrics["draws"],
            "gpu_memory_mib_representative": metrics["gpu_mem"],
            "rss_mib_representative": metrics["rss"],
            "commit_mib_representative": metrics["commit"],
            "triangles": None, "visible": None, "culled": None},
        "phase_ms_representative": metrics["phases"],
        "limitations": ["Visual correctness not verified by this run.",
                        "DEBUG-instrumented diagnostic; not quiet production throughput.",
                        "Editor and Player tracks must not be combined.",
                        "Draws and memory from perf log are representative windows."],
    })
    return {"run_id": run_id, "kind": kind, "variant": variant,
            "run_index": index, "directory": str(target), "status": "PASS",
            "frame": summary, "backend_actual": actual, "observer": observer}


def aggregate_runs(results, kinds, repeats, variants=("A",)):
    """Summarize independent runs; never turn missing runs into a number."""
    aggregate = {}
    for kind in kinds:
        for variant in variants:
            key = kind if len(variants) == 1 else "%s:%s" % (kind, variant)
            matching = [row for row in results
                        if row.get("kind") == kind and
                        row.get("variant", "A") == variant and
                        row.get("status") == "PASS"]
            if len(matching) != repeats:
                aggregate[key] = {
                    "status": "UNAVAILABLE",
                    "reason": "fewer than %d valid repeats" % repeats}
                continue
            medians = [row["frame"]["frame_median_ms"] for row in matching]
            p99s = [row["frame"]["frame_p99_ms"] for row in matching]
            center = statistics.median(medians)
            mad = statistics.median(abs(value - center) for value in medians)
            relative_mad = mad / center if center > 0 else None
            aggregate[key] = {
                "status": "PASS", "repeats": repeats,
                "run_ids": [row["run_id"] for row in matching],
                "median_of_frame_medians_ms": center,
                "median_of_frame_p99_ms": statistics.median(p99s),
                "relative_mad_of_medians": relative_mad,
                "noise_warning": (relative_mad is not None and
                                  relative_mad > 0.05),
            }
    return aggregate


def compare_variants(results, kinds, repeats):
    """Paired A/B ratios use matching run indices and reject missing pairs."""
    comparison = {}
    for kind in kinds:
        pairs = []
        for index in range(repeats):
            a = [row for row in results if row.get("kind") == kind and
                 row.get("variant") == "A" and row.get("run_index") == index and
                 row.get("status") == "PASS"]
            b = [row for row in results if row.get("kind") == kind and
                 row.get("variant") == "B" and row.get("run_index") == index and
                 row.get("status") == "PASS"]
            if len(a) != 1 or len(b) != 1:
                comparison[kind] = {"status": "UNAVAILABLE",
                                    "reason": "missing valid A/B pair"}
                break
            pairs.append(b[0]["frame"]["frame_median_ms"] /
                         a[0]["frame"]["frame_median_ms"])
        else:
            comparison[kind] = {"status": "PASS", "paired_b_over_a": pairs,
                                "median_b_over_a": statistics.median(pairs)}
    return comparison


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", type=Path,
                        help="existing scene; default is a generated canonical JCE fixture")
    parser.add_argument("--exe", type=Path, default=EXE)
    parser.add_argument("--host", choices=("editor", "player"), default="editor",
                        help="measurement track; Player requires its own --exe")
    parser.add_argument("--build-variant", default="release",
                        choices=("release", "debug", "dist", "asan"),
                        help="declared executable build variant; match the build recipe")
    parser.add_argument("--profile", choices=tuple(PROFILES), default="standard")
    parser.add_argument("--kind", choices=("all", *KINDS), default="all")
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--spread", type=float, default=0.0,
                        help="ground-plane extent for drawcall/entity/physics")
    parser.add_argument("--backend", default="d3d11")
    parser.add_argument("--compare-backend",
                        help="interleave A/B runs against a second backend")
    parser.add_argument("--compare-exe", type=Path,
                        help="interleave A/B builds on the same backend (B executable)")
    parser.add_argument("--compare-manifest", type=Path,
                        help="prior run manifest proving B executable hash and source")
    parser.add_argument("--capture", action="store_true",
                        help="compatibility flag; every profile captures the fixed Game View")
    parser.add_argument("--out", type=Path,
                        default=ROOT / "build" / "validation" / "benchmark")
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--soak-seconds", type=int, default=600,
                        help="wall-clock duration for the soak profile")
    args = parser.parse_args(argv)
    if args.backend.casefold() == "auto" or (args.compare_backend and args.compare_backend.casefold() == "auto"):
        parser.error("isolated diagnostics require a fixed renderer backend; auto is unsupported")
    if args.host == "player" and args.exe == EXE:
        parser.error("--host player requires an explicit Player executable")
    if args.count < 1 or args.count > 2000000:
        parser.error("--count must be in 1..2000000")
    if not math.isfinite(args.spread) or args.spread < 0:
        parser.error("--spread must be a finite nonnegative number")
    scene = args.scene.resolve() if args.scene else None
    exe = args.exe.resolve()
    if not exe.is_file() or (scene is not None and not scene.is_file()):
        parser.error("scene and selected executable must exist")
    warmup, sample_count, repeats = PROFILES[args.profile]
    if args.profile == "soak" and (args.soak_seconds < 1 or
                                   args.timeout <= args.soak_seconds + 30):
        parser.error("soak requires positive seconds and timeout > duration + 30")
    identity = source_identity()
    hardware = machine_manifest()
    executable_sha256 = sha256_file(exe)
    if args.compare_exe and args.compare_backend:
        parser.error("choose executable A/B or backend A/B, not both")
    if args.compare_manifest and not args.compare_exe:
        parser.error("--compare-manifest requires --compare-exe")
    executables = {"A": exe}
    hashes = {"A": executable_sha256}
    identities = {"A": identity}
    if args.compare_exe:
        candidate = args.compare_exe.resolve()
        if not candidate.is_file():
            parser.error("--compare-exe must exist")
        digest = sha256_file(candidate)
        if digest == executable_sha256:
            parser.error("executable A/B requires different binary hashes")
        executables["B"] = candidate
        hashes["B"] = digest
        identities["B"] = {"status": "UNAVAILABLE",
                           "reason": "B source identity was not supplied"}
        if args.compare_manifest:
            prior = json.loads(args.compare_manifest.read_text(encoding="utf-8"))
            try:
                identities["B"] = source_from_manifest(prior, digest)
            except ValueError as exc:
                parser.error(str(exc))
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    kinds = list(KINDS) if args.kind == "all" else [args.kind]
    if args.compare_backend == args.backend:
        parser.error("--compare-backend must differ from --backend")
    variants = [("A", args.backend)]
    if args.compare_backend:
        variants.append(("B", args.compare_backend))
        executables["B"] = exe
        hashes["B"] = executable_sha256
        identities["B"] = identity
    elif args.compare_exe:
        variants.append(("B", args.backend))
    results = []
    failed = False
    for kind in kinds:
        fixture_key = hashlib.sha256(json.dumps(
            scene_document(kind, args.count, args.spread),
            sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()[:12]
        workload_scene = scene or write_scene(
            out / "fixtures" / ("%s-%d-%s.scene.json" %
                                (kind, args.count, fixture_key)),
            kind, args.count, args.spread)
        controls = {}
        for variant, requested_backend in variants:
            control_dir = out / "controls" / ("%s-%d-%s" %
                                              (kind, args.count, uuid.uuid4().hex[:8]))
            try:
                controls[variant] = run_control(
                    exe=executables[variant], scene=workload_scene, kind=kind, count=args.count,
                    warmup=30 if args.profile == "soak" else warmup,
                    sample_count=150 if args.profile == "soak" else sample_count,
                    backend=requested_backend, out=out, timeout=args.timeout,
                    capture=True, spread=args.spread, directory=control_dir, host=args.host)
            except (OSError, ValueError, RuntimeError) as exc:
                failed = True
                results.append({
                    "kind": kind, "variant": variant,
                    "directory": str(control_dir),
                    "status": "INVALID MEASUREMENT",
                    "reason": "negative control: %s" % exc})
                print("[bench] INVALID MEASUREMENT %s/%s control: %s" %
                      (kind, variant, exc), flush=True)
        for index in range(repeats):
            order = variants if index % 2 == 0 else list(reversed(variants))
            for variant, requested_backend in order:
                if variant not in controls:
                    continue
                print("[bench] %s/%s %d run %d/%d" %
                      (kind, variant, args.count, index + 1, repeats),
                      flush=True)
                run_dir = out / ("%s-%s-%d-%s" % (
                    time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()),
                    kind, index, uuid.uuid4().hex[:8]))
                try:
                    result = run_one(
                        exe=executables[variant], scene=workload_scene, kind=kind,
                        count=args.count, profile=args.profile,
                        warmup=warmup, sample_count=sample_count,
                        backend=requested_backend, out=out, index=index,
                        identity=identities[variant], hardware=hardware,
                        timeout=args.timeout, spread=args.spread,
                        executable_sha256=hashes[variant],
                        control=controls[variant],
                        capture=True, directory=run_dir,
                        soak_seconds=args.soak_seconds
                        if args.profile == "soak" else None,
                        variant=variant, build_variant=args.build_variant, host=args.host)
                except CorrectnessFailure as exc:
                    failed = True
                    result = {"kind": kind, "variant": variant,
                              "directory": str(run_dir),
                              "run_index": index, "status": "FAIL",
                              "reason": str(exc)}
                except (OSError, ValueError, RuntimeError) as exc:
                    failed = True
                    result = {"kind": kind, "variant": variant,
                              "directory": str(run_dir),
                              "run_index": index,
                              "status": "INVALID MEASUREMENT",
                              "reason": str(exc)}
                results.append(result)
                if result["status"] == "PASS":
                    print("[bench] PASS %s median=%.3f ms p99=%.3f ms" %
                          (result["run_id"],
                           result["frame"]["frame_median_ms"],
                           result["frame"]["frame_p99_ms"]), flush=True)
                else:
                    print("[bench] %s %s/%s: %s" %
                          (result["status"], kind, variant,
                           result["reason"]), flush=True)
    suite_id = "%s-%s" % (time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()),
                            uuid.uuid4().hex[:8])
    aggregate = {
        "schema_version": 2, "suite_id": suite_id,
        "timing_capture_policy": "separate_visual_witness",
        "observer_mode": "debug_instrumented_diagnostic",
        "quiet_production_throughput": False,
        "engine": "JCE", "host": args.host,
        "profile": args.profile, "source": identity,
        "variants": [{"label": label, "backend_requested": backend,
                      "executable": str(executables[label]),
                      "executable_sha256": hashes[label],
                      "source": identities[label]}
                     for label, backend in variants],
        "results": results,
        "aggregate": aggregate_runs(
            results, kinds, repeats, tuple(label for label, _ in variants)),
        "comparison": compare_variants(results, kinds, repeats)
                      if len(variants) == 2 else None,
        "status": ("FAIL" if any(r["status"] == "FAIL" for r in results)
                   else "INVALID MEASUREMENT" if failed else "PASS"),
    }
    suite_path = out / ("suite-%s.json" % suite_id)
    _write_json(suite_path, aggregate)
    print("[bench] suite %s: %s" % (aggregate["status"], suite_path),
          flush=True)
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
