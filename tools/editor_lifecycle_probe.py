"""Hidden editor Play/Stop scene restoration witness, independent of perf INFO logs."""
import argparse
import copy
import json
import math
from pathlib import Path
import re
import uuid

from benchmark_suite import read_frames
from editor_portability_probe import probe_environment, sha256, validate_frames, validate_image, validate_process
from jce_determinism import actual_backend, read_png
from perf_bench import read_execution_log
from process_sampler import run_process

PHASES = ("ed_play_capture", "ed_play_create", "ed_play_teardown", "ed_play_restore")


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
    return result


def validate_scenes(before, during, after, require_mutation=False):
    values = [canonical_snapshot(value) for value in (before, during, after)]
    if values[0] != values[2]:
        raise ValueError("authored scene was not restored exactly under handle renaming")
    changed = values[0] != values[1]
    if require_mutation and not changed:
        raise ValueError("required runtime scene mutation was not observed")
    return {"restored": True, "during_mutation_observed": changed,
            "normalization": "bijective id/parentId renaming by retained entity order; all other fields retained",
            "script_execution": "UNAVAILABLE: scene mutation alone does not identify its producer"}


def validate_phases(samples):
    indices = list(samples["frame_indices"])
    if indices != list(range(240)):
        raise ValueError("exactly 240 contiguous frames required")
    observed = {}
    for name in PHASES:
        series = samples["phase_ms"].get(name)
        if series is None or len(series) != 240:
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
    if not (10 < capture == create < 80 < teardown == restore < 220):
        raise ValueError("lifecycle phase groups are out of order or outside scene observation intervals")
    return observed


def validate_errors(text):
    if re.search(r"\b(?:ERROR|FATAL)\s*-", text) or "single-instance lock already held" in text:
        raise ValueError("fresh editor execution reported a failure")


def lifecycle_environment(scene, root):
    env = probe_environment(scene, root, 240, False)
    env.update(JCE_DBG_AUTOPLAY="20", JCE_DBG_PLAY_STOP_AT="160",
        JCE_EDITOR_OPERATION_METRICS="1", JCE_DBG_SCENE_OBSERVE_AT="10,80,220",
        JCE_DBG_SCENE_OBSERVE_DIR=str(root), JCE_FRAME_DT_FIXED="0.016666",
        JCE_DBG_FOCUS_GAME="1", JCE_WINCAP_FRAME="100")
    return env


def probe(exe, scene, out, timeout=180, require_mutation=False):
    exe, scene = Path(exe).resolve(strict=True), Path(scene).resolve(strict=True)
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("positive finite timeout required")
    root = Path(out).resolve() / uuid.uuid4().hex
    root.mkdir(parents=True, exist_ok=False)
    profile = root / "profile"
    profile.mkdir()
    (profile / "editor-preferences.json").write_text('{"startup_mode":1}\n', encoding="utf-8", newline="\n")
    env = lifecycle_environment(scene, root)
    identity = {"exe": sha256(exe), "scene": sha256(scene), "validator": sha256(Path(__file__))}
    result = {"status": "FAIL", "identity": identity,
        "inputs": {"exe": str(exe), "scene": str(scene)}, "require_mutation": require_mutation,
        "environment": {key: value for key, value in env.items() if key.startswith("JCE_")},
        "scope": "one actual Play/Stop and complete authored scene restoration; not throughput timing",
        "limitations": ["phase ordering is observed at frame-group granularity, not an intraframe timestamp oracle",
                        "compiled-out INFO may leave actual renderer unavailable",
                        "static scene restoration does not prove script execution"]}
    try:
        process = run_process([str(exe)], env=env, cwd=str(exe.parent), timeout=timeout,
                              output_dir=root / "process", marker_path=root / "frames.csv")
        result["process"] = process
        validate_process(process)
        log = read_execution_log(root / "engine.log", process)
        validate_errors(log)
        result["backend_actual"] = actual_backend(log) or "UNAVAILABLE_COMPILED_OUT_INFO"
        result["complete_frames"] = validate_frames(root / "frames.csv", 240)
        result["phases"] = validate_phases(read_frames(root / "frames.csv", warmup=0, sample_count=None))
        result["window_size"] = validate_image(read_png(root / "window.png"), root / "window.png")
        snapshots = [json.loads((root / ("scene-%d.json" % frame)).read_text(encoding="utf-8"))
                     for frame in (10, 80, 220)]
        result["scene_control"] = validate_scenes(*snapshots, require_mutation=require_mutation)
        if sha256(exe) != identity["exe"] or sha256(scene) != identity["scene"] or sha256(Path(__file__)) != identity["validator"]:
            raise ValueError("input executable/scene/validator changed during the witness")
        result["status"] = "PLAY_STOP_AUTHORED_SCENE_RESTORATION_OBSERVED"
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        result["artifacts"] = {path.relative_to(root).as_posix(): sha256(path)
            for path in root.rglob("*") if path.is_file() and path != root / "result.json"}
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    return root / "result.json"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("exe", "scene", "out"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--require-mutation", action="store_true")
    args = parser.parse_args()
    print(probe(args.exe, args.scene, args.out, args.timeout, args.require_mutation))

if __name__ == "__main__":
    main()
