#!/usr/bin/env python3
"""Capture an ordinary visible Editor session using existing JCE timing owners.

The user controls Play and Stop. Eight native Game View PNGs use natural size
and visibility by default. CPU intervals do not certify GPU presentation.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import sys
from types import SimpleNamespace
import zipfile


# Public jce_window.h observed flags; these do not prove unobscured pixels.
HIDDEN, MINIMIZED, INPUT_FOCUS = 0x8, 0x40, 0x200
RESET_PREFIXES = ("JCE_KPI_", "JCE_BENCH_", "JCE_TRACE", "JCE_TEST_",
                  "JCE_DBG_AUTO", "JCE_CAPTURE_")
RESET_KEYS = {
    "JCE_FRAME_DT_FIXED", "JCE_STREAM_SYNC", "JCE_TAA_JITTER_PHASE",
    "JCE_INPUT_REPLAY", "JCE_INPUT_RECORD", "JCE_DBG_FOCUS_GAME", "JCE_WINDOW_HIDDEN",
    "JCE_WINDOW_NOT_FOCUSABLE", "JCE_HEADLESS", "JCE_IMPOSTOR_BAKE",
    "JCE_MAX_FRAMES", "JCE_MULTI_INSTANCE", "JCE_RESOURCE_TRACE",
    "JCE_RESOURCE_TRACE_MS", "JCE_BGFX_ALLOC_TRACE",
    "JCE_PERF_SCRIPT_DETAIL", "JCE_PERF_SCRIPT_PATH", "JCE_PERF_ECULL_DETAIL",
    "JCE_SHOT_FRAME", "JCE_SHOT_PATH", "JCE_WINCAP_FRAME", "JCE_WINCAP_PATH",
}
LIMITS = (
    "Instrumented visible Editor session, not quiet production throughput.",
    "runtime_tick>0 observes a CPU runtime tick; it is not an independent Play-state observer.",
    "CPU phases are nested and overlapping; never sum the ranking.",
    "Backend timers are latest-latched observations, not aligned to the current CPU frame.",
    "frame_cpu and frame_gpu are backend statistics snapshots, not current-frame native CPU phases.",
    "A zero backend timer does not establish timer support or absence of work.",
    "Window focus and Game CPU completion do not establish GPU completion or presentation.",
    "No scene identity is inferred from a matching filename; retain the supplied file hash and logs.",
    "Phase averages cover observed samples only; sparse phases are reported separately from recurring work.",
    "process.first_frame_s observes the CSV marker, which may appear only at flush; it is not startup or presentation latency.",
    "Scheduled Game PNGs are render-target captures, not desktop or whole-Editor screenshots.",
    "Screenshot activity can perturb timing; raw and recorded-capture-window-excluded analyses are separate.",
)
PLAY_TRANSITIONS = ("ed_play_capture", "ed_play_create", "ed_play_teardown", "ed_play_restore")
ECULL_DETAIL_PHASES = ("ecull_world", "ecull_bounds", "ecull_classify",
                       "ecull_static_store", "ecull_shadow_list", "ecull_focus_store")


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + "\n",
                          encoding="utf-8", newline="\n")


def authorities(root):
    """Use the explicit checkout, including when this file was unpacked elsewhere."""
    root = Path(root).resolve()
    for name in ("process_sampler.py", "perf_bench.py", "benchmark_suite.py"):
        if not (root / "tools" / name).is_file():
            raise ValueError("missing maintained diagnostic dependency: " + name)
    sys.path[:0] = [str(root), str(root / "tools")]
    modules = [importlib.import_module("tools." + name) for name in
               ("process_sampler", "perf_bench", "benchmark_suite", "jce_determinism",
                "editor_play_screenshots")]
    if any(Path(module.__file__).resolve().parent != root / "tools" for module in modules):
        raise ValueError("diagnostic dependencies resolved from a different checkout")
    sampler, perf, suite, determinism, screenshots = modules
    return SimpleNamespace(run_process=sampler.run_process, read_log=perf.read_execution_log,
                           read_frames=suite.read_frames, writer_re=perf.WRITER_RE,
                           editor_alive=perf._editor_alive,
                           actual_backend=determinism.actual_backend,
                           screenshots=screenshots,
                           dependency_hashes={str(Path(m.__file__).name): sha256(m.__file__)
                                              for m in modules})


def detail_options(script_path=None, ecull_detail=False):
    """Validate explicit launch selectors before any output or child process."""
    if type(ecull_detail) is not bool:
        raise ValueError("ecull_detail must be a bool")
    if script_path is not None:
        if (not isinstance(script_path, str) or not script_path or
                len(script_path.encode("utf-8")) > 255 or
                any(ord(ch) < 32 or ord(ch) == 127 for ch in script_path)):
            raise ValueError("script_path needs 1..255 UTF-8 bytes without controls")
    requested = {}
    if script_path is not None:
        requested.update(JCE_PERF_SCRIPT_DETAIL="1", JCE_PERF_SCRIPT_PATH=script_path)
    if ecull_detail:
        requested["JCE_PERF_ECULL_DETAIL"] = "1"
    metadata = None
    if requested:
        metadata = {
            "mode": "detailed_cpu_attribution",
            "script_path": script_path, "ecull_detail": ecull_detail,
            "script_scope": "Exact authored-path on_update subtotal across runtimes; excludes fixed_update and coroutines.",
            "zero_scope": "A zero subtotal does not prove that a matching callback executed.",
            "timing_scope": "Additional instrumentation; keep separate from ordinary capture timing.",
        }
    return requested, metadata


def observe_details(samples, log, request):
    """Qualify requested CPU partitions; capture integrity remains separate."""
    phases = samples["phase_ms"] if samples is not None else {}
    result = {"mode": "detailed_cpu_attribution",
              "scope": "Raw CPU phase rows, including zero; no GPU or callback-count claim."}
    path = request["script_path"]
    if path is not None:
        expected = ("script detail v1: selector=exact scope=on_update path_bytes=%d path=%s at "
                    % (len(path.encode("utf-8")), path))
        receipts = sum(expected in line for line in log.splitlines())
        names = ("script", "script_focus", "script_other")
        observed = [name for name in names if name in phases and
                    any(math.isfinite(v) for v in phases[name])]
        checked = partial = mismatches = 0
        maximum = None
        witnesses = []
        if samples is not None:
            for i, frame in enumerate(samples["frame_indices"]):
                values = [phases[name][i] if name in phases else math.nan for name in names]
                finite = [math.isfinite(value) for value in values]
                if all(finite):
                    checked += 1
                    total, focus, other = values
                    error = abs(total - focus - other)
                    maximum = error if maximum is None else max(maximum, error)
                    # Three independent %.4f CSV roundings plus a relative term.
                    if error > .000151 + 1e-12 * max(abs(total), abs(focus + other)):
                        mismatches += 1
                        if len(witnesses) < 16:
                            witnesses.append(int(frame))
                elif any(finite):
                    partial += 1
        status = "OBSERVED" if checked and receipts == 1 and not partial and not mismatches else (
            "INCOMPLETE" if receipts or any(name in observed for name in names[1:]) else "UNAVAILABLE")
        result["script"] = {
            "status": status, "requested_path": path, "native_selector_receipts": receipts,
            "observed_phase_names": observed, "missing_phase_names": [n for n in names if n not in observed],
            "same_frame_checked": checked, "partial_partition_frames": partial,
            "sum_mismatch_frames": mismatches, "first_mismatch_frame_indices": witnesses,
            "max_abs_sum_error_ms": maximum,
            "sum_tolerance": "0.000151 ms + 1e-12 * max(abs(total), abs(focus+other))",
            "zero_scope": "Zero is an empty or zero-duration subtotal, not proof a matching callback ran.",
        }
    if request["ecull_detail"]:
        observed = {name: sum(math.isfinite(value) for value in phases.get(name, ()))
                    for name in ECULL_DETAIL_PHASES}
        observed = {name: count for name, count in observed.items() if count}
        missing = [name for name in ECULL_DETAIL_PHASES if name not in observed]
        result["ecull"] = {
            "status": "OBSERVED" if not missing else "INCOMPLETE" if observed else "UNAVAILABLE",
            "observed_phase_sample_counts": observed, "missing_phase_names": missing,
            "scope": "Existing serial miss-path subphases only; never sum these as total ecull.",
            "missing_scope": "Missing rows can mean the path did not execute, an unsupported executable, or unavailable observation.",
        }
    return result


def child_environment(parent, scene, out, seconds, screenshot_environment=None,
                      *, script_path=None, ecull_detail=False):
    details, _ = detail_options(script_path, ecull_detail)
    env = dict(parent)
    removed = sorted(key for key in env if key in RESET_KEYS or
                     key.startswith(RESET_PREFIXES))
    for key in removed:
        del env[key]
    requested = {
        "JCE_PERF_LOG": "1", "JCE_TRACE": "0", "JCE_SCENE": str(scene),
        "JCE_LOG_FILE": str(out / "engine.log"), "JCE_MAX_SECONDS": str(seconds),
        "JCE_KPI_FRAME_LOG": str(out / "frames.csv"),
        "JCE_KPI_FRAME_LOG_VIEWPORT": "natural", "JCE_FRAME_LOG_CONTEXT": "3",
        "JCE_KPI_BACKEND_TIMINGS": "1",
    }
    requested.update(screenshot_environment or {})
    requested.update(details)
    replaced = sorted(key for key, value in requested.items()
                      if key in parent and parent[key] != value)
    env.update(requested)
    # No inherited values are serialized: they can contain local secrets.
    changes = {"removed_keys": removed, "replaced_keys": replaced,
               "requested_keys": sorted(requested),
               "preserved_jce_keys": sorted(key for key in parent
                                            if key.startswith("JCE_") and key not in removed
                                            and key not in requested),
               "policy": "Preserve rendering, VSync, FPS caps, backend and Editor profile."}
    return env, changes


def timing(values, *, zero_ambiguous=False):
    values = list(values)
    finite = [float(value) for value in values if math.isfinite(value)]
    zeros = sum(value == 0 for value in finite)
    measured = [value for value in finite if not zero_ambiguous or value > 0]
    ordered = sorted(measured)
    return {"samples": len(values), "finite_samples": len(finite),
            "unavailable_samples": len(values) - len(finite), "zero_samples": zeros,
            "statistics_samples": len(measured),
            "statistics_scope": "positive reports only; zero availability unknown"
                                if zero_ambiguous else "finite observed samples",
            "mean_ms": statistics.fmean(measured) if measured else None,
            "p50_ms": statistics.median(measured) if measured else None,
            "p95_ms": ordered[max(0, math.ceil(.95 * len(ordered)) - 1)] if ordered else None}


def eligibility(context):
    if not context["window_present"]:
        return "window_unavailable", None
    for key in ("flags_begin", "flags_end"):
        flags = context[key]
        if flags is None:
            return "window_flags_unavailable", None
        if flags & (HIDDEN | MINIMIZED) or not flags & INPUT_FOCUS:
            return "background_hidden_or_minimized", None
    if not context["viewport_reported"]:
        return "game_observation_unavailable", None
    game = next((v for v in context["viewports"] if v["source_id"] == 1), None)
    if not game or game["cpu_outcome"] != 3 or not game["image_count"]:
        return "game_cpu_content_not_observed", None
    if not game["allocated_size"] or not game["display_size"]:
        return "game_extent_unavailable", None
    return None, (tuple(game["allocated_size"]), tuple(game["display_size"]))


def analyze_frames(samples, *, settle_seconds=5.0, screenshot_frames=()):
    phases, contexts = samples["phase_ms"], samples["frame_context"]
    ticks = phases.get("runtime_tick", [math.nan] * len(contexts))
    excluded, spans = Counter(), []
    key = None
    start = None
    selected = []
    screenshot_frames = frozenset(screenshot_frames)

    def finish():
        if not selected:
            return
        ranked = [{"phase": name, **timing(values[i] for i in selected)}
                  for name, values in phases.items()
                  if any(math.isfinite(values[i]) and values[i] > 0 for i in selected)]
        for row in ranked:
            row["observed_frame_fraction"] = row["finite_samples"] / len(selected)
        ranked.sort(key=lambda row: row["mean_ms"] or 0, reverse=True)
        recurring = [row for row in ranked if row["observed_frame_fraction"] >= .95]
        sparse = [row for row in ranked if row["observed_frame_fraction"] < .95]
        spans.append({
            "runtime_tick_observation": key[0], "allocated_size": key[1][0],
            "display_size": key[1][1],
            "first_frame": int(samples["frame_indices"][selected[0]]),
            "last_frame": int(samples["frame_indices"][selected[-1]]),
            "steady_frames": len(selected), "settle_seconds_excluded": settle_seconds,
            "wall": timing(samples["frame_ms"][i] for i in selected),
            "backend_cpu_snapshot": timing((samples["cpu_frame_ms"][i] for i in selected),
                                            zero_ambiguous=True),
            "backend_gpu_snapshot": timing((samples["gpu_frame_ms"][i] for i in selected),
                                            zero_ambiguous=True),
            "backend_latched": {name: timing((values[i] for i in selected), zero_ambiguous=True)
                                for name, values in samples.get("backend_latched_ms", {}).items()},
            "recurring_min_observed_fraction": .95,
            "top_cpu_phases_not_additive": recurring[:12],
            "sparse_cpu_phases_not_additive": sparse,
        })

    for i, context in enumerate(contexts):
        reason, extent = eligibility(context)
        if not reason and any(name in phases and math.isfinite(phases[name][i])
                              and phases[name][i] > 0 for name in PLAY_TRANSITIONS):
            reason = "play_lifecycle_transition"
        if not reason and "evp_scene_render" in phases:
            value = phases["evp_scene_render"][i]
            if math.isfinite(value) and value > 0:
                reason = "scene_view_also_rendered_during_game_observation"
        tick = ticks[i]
        observed = "positive" if math.isfinite(tick) and tick > 0 else "not_observed"
        current = (observed, extent)
        if reason:
            finish(); key, start, selected = None, None, []
            excluded[reason] += 1
            continue
        clock = context["clock"]
        frequency, begin = int(clock["frequency_hz"]), int(clock["begin"])
        if current != key:
            finish(); key, start, selected = current, begin, []
        if (begin - start) / frequency < settle_seconds:
            excluded["settling_after_state_extent_or_visibility_change"] += 1
        elif i in screenshot_frames:
            # Preserve natural eligibility and its settling clock. Split only
            # selected runs; PNG activity is not a new Play/extent transition.
            finish()
            selected = []
            excluded["recorded_screenshot_activity"] += 1
        elif samples["frame_ms"][i] <= 0:
            excluded["zero_wall_interval"] += 1
        else:
            selected.append(i)
    finish()
    modes_by_extent = {}
    for span in spans:
        extent = (tuple(span["allocated_size"]), tuple(span["display_size"]))
        modes_by_extent.setdefault(extent, set()).add(span["runtime_tick_observation"])
    paired = [{"allocated_size": extent[0], "display_size": extent[1]}
              for extent, modes in modes_by_extent.items() if len(modes) == 2]
    return {"steady_spans": spans, "excluded_frame_counts": dict(excluded),
            "comparison_status": "OBSERVED_BOTH_TICK_MODES" if paired
                                 else "INSUFFICIENT_COMPARABLE_TICK_MODES",
            "extents_with_both_tick_modes": paired,
            "comparison_note": "These are within-session observations, not a controlled causal before/after.",
            "raw_frames": len(contexts), "game_source_id": 1}


def validate_capture(api, out, process):
    if process["timed_out"]:
        raise RuntimeError("owned child exceeded watchdog and was terminated; capture is incomplete")
    if process["returncode"] != 0:
        raise RuntimeError("owned child returned nonzero: " + str(process["returncode"]))
    log = api.read_log(out / "engine.log", process)
    if "single-instance lock already held" in log:
        raise RuntimeError("another Editor owned the single-instance lock; no fresh session")
    samples = api.read_frames(out / "frames.csv", warmup=0, sample_count=None, allow_zero_wall=True)
    if samples["context_schema"] != "window_viewports_clock_v3":
        raise RuntimeError("capture lacks requested context-v3 window/viewport/clock observations")
    indices = samples["frame_indices"]
    if not len(indices) or indices[0] != 0:
        raise RuntimeError("capture must begin at native frame zero")
    rows = api.writer_re.findall(log)
    if len(rows) != 1 or "frame KPI capture incomplete:" in log:
        raise RuntimeError("capture needs exactly one complete native KPI writer summary")
    writer = dict(zip(("accepted", "written", "accepted_bytes", "written_bytes", "peak_bytes",
                       "peak_records"), map(int, rows[0])))
    if writer["accepted"] != len(indices) or writer["written"] != len(indices):
        raise RuntimeError("native writer records do not match the complete CSV frame count")
    payload_bytes = (out / "frames.csv").stat().st_size - samples["csv_header_bytes"]
    if writer["accepted_bytes"] != payload_bytes or writer["written_bytes"] != payload_bytes:
        raise RuntimeError("native writer payload bytes do not match the complete CSV")
    if "jce_perf_phase: table full" in log:
        raise RuntimeError("native CPU phase table overflowed; phase coverage is incomplete")
    backend = next((api.actual_backend(line) for line in log.splitlines()
                    if api.actual_backend(line) is not None), None)
    return samples, {"native_writer": writer, "csv_payload_bytes": payload_bytes,
                     "csv_sha256": sha256(out / "frames.csv"), "actual_backend": backend,
                     "native_frame_clock": {key: samples["frame_context"][0]["clock"][key]
                                            for key in ("domain", "frequency_hz")},
                     "exit_mode": "native_seconds_limit" if "JCE_MAX_SECONDS reached" in log
                                  else "clean_child_exit_with_complete_writer"}


def capture(args, api=None):
    script_path = getattr(args, "script_path", None)
    ecull_detail = getattr(args, "ecull_detail", False)
    _, detail_request = detail_options(script_path, ecull_detail)
    root, exe, scene, out = (Path(getattr(args, name)).resolve()
                            for name in ("jce_root", "exe", "scene", "out"))
    if not math.isfinite(args.seconds) or not 30 <= args.seconds <= 600:
        raise ValueError("seconds must be finite and between 30 and 600")
    if not root.is_dir() or not exe.is_file() or not scene.is_file():
        raise ValueError("explicit checkout, executable and scene must exist")
    archive = out.with_name(out.name + ".zip")
    if out.exists() or archive.exists():
        raise FileExistsError("output directory and companion ZIP must both be new")
    api = api or authorities(root)
    if api.editor_alive():
        raise RuntimeError("close the existing Editor before starting this capture")
    identities = {name: {"path": str(path), "sha256": sha256(path), "bytes": path.stat().st_size}
                  for name, path in (("exe", exe), ("scene", scene))}
    shots = [] if getattr(args, "no_screenshots", False) else api.screenshots.plan(out)
    env, changes = child_environment(os.environ, scene, out, args.seconds,
                                     api.screenshots.environment(shots),
                                     script_path=script_path, ecull_detail=ecull_detail)
    out.mkdir(parents=True, exist_ok=False)
    if shots:
        (out / "screenshots").mkdir()
    report = {"schema": 1, "status": "FAILED", "limits": LIMITS, "inputs": identities,
              "requested_seconds": args.seconds, "cwd": str(root),
              "tool_sha256": sha256(__file__),
              "host": {"platform": platform.platform(), "machine": platform.machine(),
                       "processor_reported": platform.processor(), "python": platform.python_version(),
                       "logical_cores": os.cpu_count()},
              "environment_changes": changes, "dependency_hashes": api.dependency_hashes,
              "screenshot_requests": [{k: v for k, v in shot.items() if k != "native_path"}
                                      for shot in shots]}
    if detail_request is not None:
        report["detail_request"] = detail_request
    write_json(out / "request.json", report)
    print("Select Game View. Observe stopped for 15 seconds, Play for at least 65 seconds, then Stop for 15 seconds.", flush=True)
    if shots:
        print("During Play, keep the first 30 seconds still, then move through trees, interiors or caves. Eight Game-only PNGs use Play-clock seconds 6,12,24,36,42,48,54,60; pauses and stalls delay them relative to wall time.", flush=True)
    print("Keep the window size and rendering settings unchanged; close the Editor when done. Automatic exit after %.0f seconds." % args.seconds, flush=True)
    if detail_request is not None:
        print("Detailed CPU attribution is requested. Keep this capture separate from ordinary timing; requested script matching is exact and is not a proven bottleneck.", flush=True)
    samples, observed_log = None, ""
    try:
        process = api.run_process([str(exe)], env=env, cwd=root, timeout=args.seconds + 90,
                                  output_dir=out / "process", marker_path=out / "frames.csv",
                                  interval=.25)
        report["process"] = {key: process[key] for key in
                             ("process_identity", "clock", "returncode", "timed_out", "elapsed_s",
                              "first_frame_s", "sampling_available", "peak_rss_bytes", "cpu_time_s",
                              "cpu_time_scope")}
        observed_log = api.read_log(out / "engine.log", process)
        samples, complete = validate_capture(api, out, process)
        if any(sha256(item["path"]) != item["sha256"] for item in identities.values()):
            raise RuntimeError("executable or input scene changed during capture")
        report.update(status="CAPTURE_COMPLETE", integrity=complete,
                      analysis=analyze_frames(samples))
    except (Exception, KeyboardInterrupt) as exc:
        report["failure"] = type(exc).__name__ + ": " + str(exc)
    if detail_request is not None:
        report["detail_observation"] = observe_details(samples, observed_log, detail_request)
    screenshots = api.screenshots.collect(out, shots, observed_log,
                                          samples["frame_context"] if samples else None)
    write_json(out / "screenshots.json", screenshots)
    report["screenshots"] = {"manifest": "screenshots.json", "status": screenshots["status"],
                              "complete_images": screenshots["complete_images"],
                              "requested_images": screenshots["requested_images"],
                              "archived_images": screenshots["archived_images"],
                              "unverified_images": screenshots["unverified_images"]}
    if samples is not None:
        exclusion = screenshots["performance_exclusion"]
        if exclusion["available"]:
            report["analysis_excluding_screenshots"] = analyze_frames(
                samples, screenshot_frames=exclusion["frame_indices"])
            report["analysis_excluding_screenshots"]["scope"] = exclusion["policy"]
        else:
            report["analysis_excluding_screenshots"] = {
                "status": "UNAVAILABLE", "reason": "Screenshot native-clock receipts are absent or inconsistent; raw analysis is retained."}
    write_json(out / "report.json", report)
    # Only this exclusive session's named artifacts; never recurse into a repo.
    names = ["request.json", "report.json", "engine.log", "frames.csv",
             "process/stdout.log", "process/stderr.log", "process/result.json", "process/samples.json",
             "screenshots.json", *screenshots["archive_members"]]
    with zipfile.ZipFile(archive, "x", zipfile.ZIP_DEFLATED) as bundle:
        for name in names:
            path = out / name
            if path.is_file() and not path.is_symlink():
                bundle.write(path, name)
    print("%s: %s" % (report["status"], archive), flush=True)
    print("Game PNGs: %s (%d/%d completed); see screenshots.json." %
          (screenshots["status"], screenshots["complete_images"], screenshots["requested_images"]), flush=True)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jce-root", required=True, type=Path)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--scene", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--seconds", type=float, default=120)
    parser.add_argument("--no-screenshots", action="store_true",
                        help="Explicit timing-only control; omit native Game View PNG requests")
    parser.add_argument("--script-path",
                        help="Opt-in detailed capture: exact authored script path for same-frame on_update focus/other costs")
    parser.add_argument("--ecull-detail", action="store_true",
                        help="Opt-in detailed capture: enable existing entity-cull CPU subphases")
    args = parser.parse_args(argv)
    try:
        return 0 if capture(args)["status"] == "CAPTURE_COMPLETE" else 1
    except Exception as exc:
        print("Diagnostic refused: %s: %s" % (type(exc).__name__, exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
