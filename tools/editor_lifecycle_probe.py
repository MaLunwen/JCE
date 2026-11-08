"""Hidden editor Play/Stop scene restoration witness, independent of perf INFO logs."""
import argparse
import copy
import json
import math
from pathlib import Path
import re
import secrets
import uuid

from benchmark_suite import read_frames
from editor_portability_probe import probe_environment, sha256, validate_image, validate_process
from jce_determinism import actual_backend, read_png
from perf_bench import read_execution_log
from process_sampler import run_process

PHASES = ("ed_play_capture", "ed_play_create", "ed_play_teardown", "ed_play_restore")
RENDERER_SELECTORS = {
    "backend": (("d3d11", "d3d12", "vulkan", "opengl"), "JCE_BACKEND"),
    "gpu_adapter": (("auto", "intel", "nvidia", "amd"), "JCE_GPU_ADAPTER"),
    "gpu_tier": (("low", "medium", "high", "ultra"), "JCE_GPU_TIER"),
}


def _validate_renderer_selectors(backend=None, gpu_adapter=None, gpu_tier=None):
    """Normalize only supported explicit startup requests before any file access."""
    requested = {}
    for name, value in (("backend", backend), ("gpu_adapter", gpu_adapter), ("gpu_tier", gpu_tier)):
        if value is None:
            continue
        choices, _ = RENDERER_SELECTORS[name]
        if not isinstance(value, str) or value.strip().lower() not in choices:
            raise ValueError("--%s must be one of: %s" % (name.replace("_", "-"), ", ".join(choices)))
        requested[name] = value.strip().lower()
    return requested


def _validate_visible_not_focusable(value):
    if type(value) is not bool:
        raise ValueError("visible_not_focusable must be boolean")


def _validate_vsync(value):
    if value is not None and (type(value) is not str or value not in ("on", "off")):
        raise ValueError("--vsync must be on or off")


def _validate_schedule(frames=240, play_at=20, stop_at=160, observe_at=(10, 80, 220), shot_at=100):
    """Admit one bounded plan before path access, output creation or child launch."""
    if not isinstance(observe_at, (tuple, list)) or not 3 <= len(observe_at) <= 16:
        raise ValueError("observe_at requires 3..16 integer frames")
    if any(type(value) is not int for value in (frames, play_at, stop_at, shot_at, *observe_at)):
        raise ValueError("schedule frames must be integers, not booleans")
    if not 240 <= frames <= 10000:
        raise ValueError("frames must be in 240..10000")
    if stop_at <= 0:
        raise ValueError("stop_at must be a positive PLAYING-tick count")
    if any(left >= right for left, right in zip(observe_at, observe_at[1:])):
        raise ValueError("observe_at frames must be strictly increasing")
    # Requested Play can defer viewport admission. This is the earliest Stop
    # tick; actual positive lifecycle phase observations remain authoritative.
    stop_editor_tick = play_at + stop_at
    before, *during, after = observe_at
    if not (0 < before < play_at < during[0] <= during[-1] < stop_editor_tick < after < frames):
        raise ValueError("schedule requires before < Play < every during observation < earliest Stop < after < frames")
    if not play_at < shot_at < stop_editor_tick:
        raise ValueError("shot_at must be strictly between play_at and stop_editor_tick")
    # Observation precedes Stop within a tick, so AFTER must be later even
    # under immediate Play admission; the observed phases check deferred cases.
    return dict(frames=frames, play_at=play_at, stop_at=stop_at,
                observe_at=tuple(observe_at), shot_at=shot_at)


def canonical_snapshot(value):
    """Rename scene handles bijectively; preserve all authored payload and hierarchy."""
    result = copy.deepcopy(value)
    scene = result.get("scene") if isinstance(result, dict) else None
    entities = scene.get("entities") if isinstance(scene, dict) else None
    if not isinstance(entities, list) or not entities:
        raise ValueError("nonempty canonical scene.entities required")
    handles = {}
    for index, entity in enumerate(entities):
        if not isinstance(entity, dict) or not isinstance(entity.get("components"), list):
            raise ValueError("invalid entity payload")
        handle = entity.get("id")
        if isinstance(handle, bool) or not isinstance(handle, int) or handle <= 0 or handle in handles:
            raise ValueError("invalid or duplicate scene handle")
        handles[handle] = index + 1
    for entity in entities:
        parent = entity.get("parentId")
        if isinstance(parent, bool) or not isinstance(parent, int) or parent < 0:
            raise ValueError("invalid parent handle")
        if parent and parent not in handles:
            raise ValueError("unresolved parent handle")
        entity["id"] = handles[entity["id"]]
        entity["parentId"] = handles[parent] if parent else 0
        for component in entity["components"]:
            if component.get("type") == "WaterNearshore":
                props = component.get("properties")
                terrain = props.get("terrain") if isinstance(props, dict) else None
                if type(terrain) is not int or terrain < 0 or (terrain and terrain not in handles):
                    raise ValueError("invalid or unresolved nearshore terrain handle")
                props["terrain"] = handles[terrain] if terrain else 0
            elif component.get("type") == "Script":
                params = component.get("params", [])
                if not isinstance(params, list):
                    raise ValueError("invalid script parameter array")
                for param in params:
                    if not isinstance(param, dict):
                        raise ValueError("invalid script parameter payload")
                    kind = param.get("kind")
                    if type(kind) is int and kind == 3:  # JCE_SCRIPT_PARAM_ENTITY
                        target = param.get("entity")
                        if type(target) is not int or target < 0 or (target and target not in handles):
                            raise ValueError("invalid or unresolved script parameter entity handle")
                        param["entity"] = handles[target] if target else 0
    return result


def validate_scenes(before, during, after, require_mutation=False):
    values = [canonical_snapshot(value) for value in (before, during, after)]
    if values[0] != values[2]:
        raise ValueError("authored scene was not restored exactly under handle renaming")
    changed = values[0] != values[1]
    if require_mutation and not changed:
        raise ValueError("required runtime scene mutation was not observed")
    return {"restored": True, "during_mutation_observed": changed,
            "normalization": "bijective entity and known reference renaming by retained entity order; all other fields retained",
            "script_execution": "UNAVAILABLE: scene mutation alone does not identify its producer"}


def validate_phases(samples, *, frames=240, play_at=20, stop_at=160, observe_at=(10, 80, 220), shot_at=100):
    schedule = _validate_schedule(frames, play_at, stop_at, observe_at, shot_at)
    indices = list(samples["frame_indices"])
    if indices != list(range(schedule["frames"])):
        raise ValueError("exactly %d contiguous frames required" % schedule["frames"])
    observed = {}
    for name in PHASES:
        series = samples["phase_ms"].get(name)
        if series is None or len(series) != schedule["frames"]:
            raise ValueError("missing lifecycle phase: " + name)
        if any(math.isinf(value) or (math.isfinite(value) and value < 0) for value in series):
            raise ValueError("invalid lifecycle phase duration: " + name)
        # The production accumulator emits reset-zero rows after registration.
        # They are absence of work, not additional executions; NaN is no row.
        rows = [(indices[index], value) for index, value in enumerate(series)
                if math.isfinite(value) and value > 0]
        if len(rows) != 1:
            raise ValueError("each lifecycle phase requires one actual positive observation: " + name)
        observed[name] = {"frame": rows[0][0], "ms": rows[0][1]}
    capture, create, teardown, restore = [observed[name]["frame"] for name in PHASES]
    before, *during, after = schedule["observe_at"]
    if not (before < capture == create < during[0] <= during[-1] < teardown == restore < after):
        raise ValueError("lifecycle phase groups are out of order or outside scene observation intervals")
    return observed


def validate_errors(text):
    if re.search(r"\b(?:ERROR|FATAL)\s*-", text) or "single-instance lock already held" in text:
        raise ValueError("fresh editor execution reported a failure")


def _validate_resource_trace(trace, resource_trace):
    if resource_trace and not trace:
        raise ValueError("--resource-trace requires --trace")


def _validate_shader_provenance(trace, shader_provenance):
    if type(shader_provenance) is not bool:
        raise ValueError("shader_provenance must be boolean")
    if shader_provenance and not trace:
        raise ValueError("--shader-provenance requires --trace")


def _validate_filesystem_open_failures(trace, filesystem_open_failures):
    if type(filesystem_open_failures) is not bool:
        raise ValueError("filesystem_open_failures must be boolean")
    if filesystem_open_failures and not trace:
        raise ValueError("--filesystem-open-failures requires --trace")


def _validate_scope_witnesses(trace, scope_witnesses):
    if type(scope_witnesses) is not bool:
        raise ValueError("scope_witnesses must be boolean")
    if scope_witnesses and not trace:
        raise ValueError("--scope-witnesses requires --trace")


def _validate_scope_prefixes(trace, prefixes):
    from tools.lifecycle_trace_report import _scope_prefixes
    prefixes = _scope_prefixes(prefixes)
    if prefixes and not trace:
        raise ValueError("--scope-prefix requires --trace")
    return prefixes


def _validate_trace_history(trace, path=None, records=None, byte_limit=None):
    """Admit the existing exact-history storage tuple without filesystem access."""
    if all(value is None for value in (path, records, byte_limit)):
        return None
    if not trace:
        raise ValueError("disk history requires --trace")
    from tools.trace_history_storage import history_options
    return history_options(path, records, byte_limit)


def lifecycle_environment(scene, root, trace=False, *, resource_trace=False, shader_provenance=False,
                          filesystem_open_failures=False, scope_witnesses=False, scope_prefixes=(),
                          backend=None, gpu_adapter=None, gpu_tier=None, visible_not_focusable=False, vsync=None,
                          frames=240, play_at=20, stop_at=160, observe_at=(10, 80, 220), shot_at=100):
    schedule = _validate_schedule(frames, play_at, stop_at, observe_at, shot_at)
    selectors = _validate_renderer_selectors(backend, gpu_adapter, gpu_tier)
    _validate_visible_not_focusable(visible_not_focusable)
    _validate_vsync(vsync)
    _validate_resource_trace(trace, resource_trace)
    _validate_shader_provenance(trace, shader_provenance)
    _validate_filesystem_open_failures(trace, filesystem_open_failures)
    _validate_scope_witnesses(trace, scope_witnesses)
    _validate_scope_prefixes(trace, scope_prefixes)
    env = probe_environment(scene, root, schedule["frames"], False)
    if vsync is not None:
        env["JCE_LAUNCH_VSYNC"] = "1" if vsync == "on" else "0"
    if visible_not_focusable:
        env.update(JCE_WINDOW_HIDDEN="0", JCE_WINDOW_NOT_FOCUSABLE="1")
    # The portability owner strips inherited JCE_*; add only admitted requests.
    env.update({RENDERER_SELECTORS[name][1]: value for name, value in selectors.items()})
    env.update(JCE_DBG_AUTOPLAY=str(schedule["play_at"]), JCE_DBG_PLAY_STOP_AT=str(schedule["stop_at"]),
        JCE_EDITOR_OPERATION_METRICS="1", JCE_DBG_SCENE_OBSERVE_AT=",".join(map(str, schedule["observe_at"])),
        JCE_DBG_SCENE_OBSERVE_DIR=str(root), JCE_FRAME_DT_FIXED="0.016666",
        JCE_DBG_FOCUS_GAME="1", JCE_WINCAP_FRAME=str(schedule["shot_at"]))
    if trace:
        env.update(JCE_TRACE="1", JCE_TRACE_STREAM=str(root / "lifecycle-trace.json"),
                   JCE_TRACE_EXPORT="")
    if resource_trace:
        env["JCE_RESOURCE_TRACE"] = "1"
    if shader_provenance:
        env["JCE_RENDER_RESOURCE_DIAG"] = "1"
    if filesystem_open_failures:
        env["JCE_FS_OPEN_FAILURE_DETAIL"] = "1"
    return env


def probe(exe, scene, out, timeout=180, require_mutation=False, trace=False,
          *, resource_trace=False, require_terminal_ack=False, shader_provenance=False,
          filesystem_open_failures=False, scope_witnesses=False, scope_prefixes=(),
          backend=None, gpu_adapter=None, gpu_tier=None, visible_not_focusable=False, vsync=None,
          trace_history_db_path=None, disk_history_max_records=None, disk_history_max_bytes=None,
          frames=240, play_at=20, stop_at=160, observe_at=(10, 80, 220), shot_at=100):
    schedule = _validate_schedule(frames, play_at, stop_at, observe_at, shot_at)
    selectors = _validate_renderer_selectors(backend, gpu_adapter, gpu_tier)
    _validate_visible_not_focusable(visible_not_focusable)
    _validate_vsync(vsync)
    if type(require_terminal_ack) is not bool:
        raise ValueError("require_terminal_ack must be boolean")
    _validate_resource_trace(trace, resource_trace)
    _validate_shader_provenance(trace, shader_provenance)
    _validate_filesystem_open_failures(trace, filesystem_open_failures)
    _validate_scope_witnesses(trace, scope_witnesses)
    scope_prefixes = _validate_scope_prefixes(trace, scope_prefixes)
    history = _validate_trace_history(trace, trace_history_db_path,
                                      disk_history_max_records, disk_history_max_bytes)
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("positive finite timeout required")
    if history:
        path = history["path"]
        for candidate in (path, *(Path(str(path) + suffix) for suffix in ("-journal", "-wal", "-shm"))):
            if candidate.exists() or candidate.is_symlink():
                raise ValueError("disk history path or sidecar already exists; preserve it")
        if not path.parent.is_dir():
            raise ValueError("disk history parent directory must exist")
    exe, scene = Path(exe).resolve(strict=True), Path(scene).resolve(strict=True)
    root = Path(out).resolve() / uuid.uuid4().hex
    root.mkdir(parents=True, exist_ok=False)
    profile = root / "profile"
    profile.mkdir()
    preferences = {"startup_mode": 1}
    if "backend" in selectors:
        preferences["renderer"] = {"d3d11": "D3D11", "d3d12": "D3D12", "vulkan": "Vulkan", "opengl": "OpenGL"}[selectors["backend"]]
    (profile / "editor-preferences.json").write_text(
        json.dumps(preferences, separators=(",", ":")) + "\n", encoding="utf-8", newline="\n")
    if "gpu_adapter" in selectors:
        # Saved editor choice precedes the environment; request the same vendor
        # through the owned session rather than relying on that precedence.
        choice = {"auto": 0, "intel": 1, "nvidia": 2, "amd": 3}[selectors["gpu_adapter"]]
        (profile / "editor-session.json").write_text(
            json.dumps({"ui_state_int": {"editor.gpu_adapter": choice}}, separators=(",", ":")) + "\n",
            encoding="utf-8", newline="\n")
    env = lifecycle_environment(scene, root, trace, resource_trace=resource_trace,
                                shader_provenance=shader_provenance,
                                filesystem_open_failures=filesystem_open_failures,
                                visible_not_focusable=visible_not_focusable, vsync=vsync, **selectors, **schedule)
    terminal_token, terminal_trace, terminal_hashes = None, None, {}
    if require_terminal_ack:
        from tools.terminal_ack_observation import requested_trace_path
        terminal_trace = requested_trace_path(env, exe.parent)
        terminal_token = secrets.token_hex(16)
        # The normal environment filter intentionally removes inherited JCE_ keys.
        env["JCE_TERMINAL_ACK_TOKEN"] = terminal_token
    identity = {"exe": sha256(exe), "scene": sha256(scene), "validator": sha256(Path(__file__))}
    result = {"status": "FAIL", "identity": identity,
        "inputs": {"exe": str(exe), "scene": str(scene)}, "require_mutation": require_mutation,
        "schedule": dict(schedule, earliest_stop_editor_tick=play_at + stop_at,
                         stop_editor_tick_basis="requested play_at + stop_at; deferred Play admission may delay Stop; "
                                                "actual positive phase observations remain authoritative",
                         stop_at_unit="PLAYING ticks", other_frame_unit="editor ticks"),
        "continuous_cpu_trace_requested": bool(trace),
        "resource_observations_requested": bool(resource_trace),
        "environment": {key: value for key, value in env.items() if key.startswith("JCE_")},
        "scope": "one actual Play/Stop and complete authored scene restoration; not throughput timing",
        "limitations": ["phase ordering is observed at frame-group granularity, not an intraframe timestamp oracle",
                        "compiled-out INFO may leave actual renderer unavailable",
                        "static scene restoration does not prove script execution"]}
    if visible_not_focusable:
        result["limitations"].append("visible nonactivating window request; "
            "not physical UI interaction or presentation proof")
    if vsync is not None:
        result["vsync_requested"] = vsync
        result["limitations"].append("VSync is an explicit startup request; actual renderer reset/presentation "
                                    "policy requires independent observation")
    if history:
        result["trace_history_storage_requested"] = dict(history, path=str(history["path"]))
    if selectors:
        result["renderer_selectors_requested"] = selectors
        result["limitations"].append("requested selectors are startup requests; actual backend/adapter and "
                                    "effective quality/extent require independent observation")
    if require_terminal_ack:
        result["terminal_ack_requested"] = True
    if shader_provenance:
        result["shader_provenance_requested"] = True
    if filesystem_open_failures:
        result["filesystem_open_failures_requested"] = True
        result["limitations"].append("filesystem open-failure text remains in raw diagnostic artifacts; "
            "canonical filesystem_read_details observes typed CPU reads only, without decoded paths/errors or all-failure completeness")
    if scope_witnesses:
        result["scope_witnesses_requested"] = True
    if scope_prefixes:
        result["scope_prefixes_requested"] = list(scope_prefixes)
    history_reader_options = (dict(history_db_path=history["path"],
        disk_history_max_records=history["max_records"], disk_history_max_bytes=history["max_bytes"])
        if history else {})
    analysis_options = {}
    if scope_witnesses:
        analysis_options.update(scope_witnesses=True, required=["editor.play.begin", "editor.play.stop"])
    if scope_prefixes:
        analysis_options["scope_prefixes"] = scope_prefixes
    if resource_trace:
        analysis_options["resource_observations"] = True
    if shader_provenance:
        analysis_options["shader_provenance"] = True
    if filesystem_open_failures:
        analysis_options["filesystem_read_details"] = True
    try:
        process = run_process([str(exe)], env=env, cwd=str(exe.parent), timeout=timeout,
                              output_dir=root / "process", marker_path=root / "frames.csv")
        result["process"] = process
        validate_process(process)
        if require_terminal_ack:
            from tools.terminal_ack_observation import observe_terminal_ack
            observation = observe_terminal_ack(process, terminal_token,
                expected_stderr_path=root / "process" / "stderr.log", trace_path=terminal_trace,
                trace_analysis_options=analysis_options or None,
                return_trace_report=bool(trace),
                **{"trace_" + key: value for key, value in history_reader_options.items()})
            result["terminal_ack"] = observation
            # Reuse authoritative byte hashes; never scan the complete trace again
            # merely to inventory it. The owned stderr verification stays independent
            # of the process sampler's decoded text cache.
            for receipt in (observation["stderr"], observation["trace"]):
                if receipt is None:
                    continue
                path = Path(receipt["path"]).resolve()
                stat = path.stat()
                size = receipt.get("bytes", receipt.get("input_bytes"))
                if stat.st_size != size:
                    raise ValueError("validated terminal artifact size changed")
                terminal_hashes[path] = (receipt["sha256"], stat.st_size, stat.st_mtime_ns)
        elif shader_provenance or filesystem_open_failures or scope_witnesses or scope_prefixes or history:
            from tools import lifecycle_trace_report as trace_reader
            trace_path = root / "lifecycle-trace.json"
            options = dict(analysis_options)
            required = list(options.pop("required", ["engine.log_quiesce"]))
            if "engine.log_quiesce" not in required:
                required.append("engine.log_quiesce")
            trace_report = trace_reader.analyze_trace_file(trace_path, required, **options, **history_reader_options)
            stat = trace_path.stat()
            if list(trace_reader._fingerprint(stat)) != trace_report["source_path_fingerprint"]:
                raise ValueError("validated lifecycle trace changed before inventory")
            terminal_hashes[trace_path.resolve()] = (trace_report["sha256"], stat.st_size, stat.st_mtime_ns)
            result["trace_report"] = trace_report
        log = read_execution_log(root / "engine.log", process)
        validate_errors(log)
        result["backend_actual"] = actual_backend(log) or "UNAVAILABLE_COMPILED_OUT_INFO"
        frame_samples = read_frames(root / "frames.csv", warmup=0, sample_count=None)
        phases = validate_phases(frame_samples, **schedule)
        result["complete_frames"] = len(frame_samples["frame_indices"])
        result["phases"] = phases
        result["window_size"] = validate_image(read_png(root / "window.png"), root / "window.png")
        snapshots = [json.loads((root / ("scene-%d.json" % frame)).read_text(encoding="utf-8"))
                     for frame in schedule["observe_at"]]
        scene_controls = [validate_scenes(snapshots[0], during, snapshots[-1], require_mutation=require_mutation)
                          for during in snapshots[1:-1]]
        result["scene_control"] = scene_controls[0]
        if len(scene_controls) > 1:
            result["scene_controls"] = scene_controls
        if sha256(exe) != identity["exe"] or sha256(scene) != identity["scene"] or sha256(Path(__file__)) != identity["validator"]:
            raise ValueError("input executable/scene/validator changed during the witness")
        result["status"] = "PLAY_STOP_AUTHORED_SCENE_RESTORATION_OBSERVED"
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        def artifact_hash(path):
            observed = terminal_hashes.get(path.resolve())
            if observed is None:
                return sha256(path)
            digest, size, modified = observed
            stat = path.stat()
            if (stat.st_size, stat.st_mtime_ns) != (size, modified):
                raise ValueError("validated terminal artifact changed before inventory")
            return digest
        try:
            result["artifacts"] = {path.relative_to(root).as_posix(): artifact_hash(path)
                for path in root.rglob("*") if path.is_file() and path != root / "result.json"}
        except Exception as error:
            result["status"], result["error"] = "FAIL", str(error)
            raise
        finally:
            (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    return root / "result.json"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("exe", "scene", "out"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--visible-not-focusable", action="store_true",
                        help="Request a visible nonactivating window; default remains hidden")
    parser.add_argument("--vsync", choices=("on", "off"),
                        help="Explicit startup request after environment filtering; omitted retains the default")
    for name, (choices, _) in RENDERER_SELECTORS.items():
        parser.add_argument("--" + name.replace("_", "-"), metavar="{" + ",".join(choices) + "}",
                            help="Explicit startup request; omitted preserves the isolated profile default")
    parser.add_argument("--frames", type=int, default=240, help="total editor frames, 240..10000")
    parser.add_argument("--play-at", type=int, default=20, help="editor tick that enters Play")
    parser.add_argument("--stop-at", type=int, default=160, help="number of PLAYING ticks before Stop")
    parser.add_argument("--observe-at", type=int, nargs="+", default=(10, 80, 220),
                        metavar="TICK", help="3..16 ordered scene ticks: before, one or more during, after")
    parser.add_argument("--shot-at", type=int, default=100, help="whole-window capture tick during Play")
    parser.add_argument("--require-mutation", action="store_true")
    parser.add_argument("--trace", action="store_true",
                        help="Request continuous CPU trace; lifecycle completeness is validated separately")
    parser.add_argument("--resource-trace", action="store_true",
                        help="Request default-rate resource observations; requires --trace")
    parser.add_argument("--shader-provenance", action="store_true",
                        help="Request CPU shader acquisition/link provenance; requires --trace")
    parser.add_argument("--filesystem-open-failures", action="store_true",
                        help="Request CPU filesystem open-failure details; requires --trace")
    parser.add_argument("--scope-witnesses", action="store_true",
                        help="retain absolute CPU Play/Stop interval witnesses; requires --trace")
    parser.add_argument("--scope-prefix", action="append", default=[],
                        help="observe bounded CPU scope timings by name/thread for a prefix (repeatable); requires --trace")
    parser.add_argument("--trace-history-db", type=Path,
                        help="Fresh absolute SQLite history path; requires --trace and both disk budgets")
    parser.add_argument("--disk-history-max-records", type=int)
    parser.add_argument("--disk-history-max-bytes", type=int)
    parser.add_argument("--require-terminal-ack", action="store_true",
                        help="require the owned child's token-bound Engine tail; validate requested trace too")
    args = parser.parse_args()
    try:
        schedule = _validate_schedule(args.frames, args.play_at, args.stop_at, args.observe_at, args.shot_at)
        selectors = _validate_renderer_selectors(args.backend, args.gpu_adapter, args.gpu_tier)
        history = _validate_trace_history(args.trace, args.trace_history_db,
                                          args.disk_history_max_records, args.disk_history_max_bytes)
        _validate_resource_trace(args.trace, args.resource_trace)
        _validate_shader_provenance(args.trace, args.shader_provenance)
        _validate_filesystem_open_failures(args.trace, args.filesystem_open_failures)
        _validate_scope_witnesses(args.trace, args.scope_witnesses)
        scope_prefixes = _validate_scope_prefixes(args.trace, args.scope_prefix)
    except ValueError as error:
        parser.error(str(error))
    history_options = (dict(trace_history_db_path=history["path"],
        disk_history_max_records=history["max_records"], disk_history_max_bytes=history["max_bytes"])
        if history else {})
    print(probe(args.exe, args.scene, args.out, args.timeout, args.require_mutation, args.trace,
                resource_trace=args.resource_trace, require_terminal_ack=args.require_terminal_ack,
                shader_provenance=args.shader_provenance, filesystem_open_failures=args.filesystem_open_failures,
                scope_witnesses=args.scope_witnesses, scope_prefixes=scope_prefixes,
                visible_not_focusable=args.visible_not_focusable, vsync=args.vsync, **selectors, **history_options, **schedule))

if __name__ == "__main__":
    main()
