#!/usr/bin/env python3
"""Maintenance-command hierarchy observations; physical input latency unavailable."""
import argparse
import json
import math
import statistics
from pathlib import Path
import re
import sys
from PIL import Image
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from tools.perf_bench import run
from tools.jce_determinism import assert_backend
from tools.benchmark_suite import sha256_file, read_frames, frame_summary, percentile


def validate_observations(text, count, layout="flat"):
    rows = re.findall(r"hierarchy-population: loaded=(\d+) roots=(\d+) rows=(\d+) capacity=(\d+) frame=(\d+)", text)
    if len(rows) != 1:
        raise ValueError("missing or repeated hierarchy population")
    loaded, roots, visible, capacity, frame = map(int, rows[0])
    expected_roots = count if layout == "flat" else 1
    if loaded != count or frame != 50 or roots != expected_roots or visible != loaded or capacity < visible:
        raise ValueError("hierarchy population truncated or differs")
    requests = re.findall(r"inspector-request: generation=(\d+) entity=(\d+) frame=(\d+)", text)
    complete = re.findall(r"inspector-complete: generation=(\d+) entity=(\d+) frame=(\d+) status=(\w+) latency_ms=([\d.]+)", text)
    request_map = {int(g): (int(e), int(f)) for g, e, f in requests}
    if len(request_map) != len(requests):
        raise ValueError("duplicate inspector generation")
    results = []
    for g, e, f, status, ms in complete:
        generation, entity, frame = int(g), int(e), int(f)
        if generation not in request_map or status != "drawn":
            continue
        expected_entity, requested_frame = request_map[generation]
        if entity != expected_entity or frame < requested_frame:
            raise ValueError("invalid inspector consumption identity")
        # Every completion generation is unique; no repeated draw can manufacture evidence.
        if any(row["generation"] == generation for row in results):
            raise ValueError("duplicate inspector completion")
        if not math.isfinite(float(ms)) or float(ms) < 0:
            raise ValueError("invalid inspector latency")
        results.append(dict(generation=generation, entity=entity, frame=frame, latency_ms=float(ms)))
    if not results:
        raise ValueError("no actual Inspector draw completion")
    return dict(loaded=loaded, roots=roots, rows=visible, capacity=capacity, inspector=results)


def hierarchy_cache_environment(disabled):
    return {"JCE_DISABLE_HIERARCHY_CACHE": "1"} if disabled else {}


def validate_cache_observations(text, disabled):
    rows = re.findall(r"hierarchy-flat-cache: frame=(\d+) eligible=(\d+) reused=(\d+) rows=(\d+) roots=(\d+)", text)
    if not rows: raise ValueError("actual hierarchy cache observations missing")
    values = [dict(frame=int(f), eligible=int(e), reused=int(r), rows=int(n), roots=int(root)) for f,e,r,n,root in rows]
    if len({row["frame"] for row in values}) != len(values):
        raise ValueError("duplicate hierarchy cache frame")
    if any(row["eligible"] not in (0,1) or row["reused"] not in (0,1) or row["reused"] > row["eligible"] for row in values):
        raise ValueError("invalid actual hierarchy cache state")
    if disabled and any(row["reused"] for row in values):
        raise ValueError("disabled cache unexpectedly reused hierarchy")
    if not disabled and not any(row["reused"] for row in values):
        raise ValueError("eligible default hierarchy never reused cached rows")
    return dict(disabled_requested=disabled, observations=values,
                reuse_count=sum(row["reused"] for row in values), rebuild_count=sum(1-row["reused"] for row in values))

def compare_cache_controls(current, previous_path):
    previous = json.loads(previous_path.read_text(encoding="utf-8"))
    for field in ("executable_sha256", "scene_sha256", "backend_actual"):
        if current[field] != previous.get(field): raise ValueError("cache A/B input or backend differs")
    if current["hierarchy_cache"]["disabled_requested"] == previous["hierarchy_cache"]["disabled_requested"]:
        raise ValueError("cache A/B requires opposite explicit controls")
    for name, digest in previous["raw_sha256"].items():
        path = previous_path.parent/name
        if not path.is_file() or sha256_file(path) != digest: raise ValueError("previous A/B raw evidence changed")
    def row_states(result):
        return [(row["frame"], row["rows"], row["roots"]) for row in result["hierarchy_cache"]["observations"]]
    if row_states(current) != row_states(previous): raise ValueError("cache changes actual visible row/root timeline")
    if isinstance(current["observations"], dict):
        def inspector(result):
            return [(row["generation"], row["entity"], row["frame"]) for row in result["observations"]["inspector"]]
        for field in ("loaded", "roots", "rows", "capacity"):
            if current["observations"][field] != previous["observations"][field]: raise ValueError("cache changes actual population")
        if inspector(current) != inspector(previous): raise ValueError("cache changes actual Inspector consumption state")
    elif current["observations"] != previous["observations"] or current.get("events") != previous.get("events"):
        raise ValueError("cache changes actual scoped relation/local state")
    return dict(status="SAME_INPUT_STATE_OBSERVATIONS", previous_result=str(previous_path),
                previous_result_sha256=sha256_file(previous_path), scope="state correctness; no timing ratio inferred")

def validate_capture(path):
    if not path.is_file() or not path.stat().st_size:
        raise ValueError("editor capture missing")
    with Image.open(path) as image:
        if image.format != "PNG" or min(image.size) <= 0:
            raise ValueError("invalid editor PNG capture")
        image.verify()
    with Image.open(path) as image:
        image.load()
    return image.size

def operation_phases(samples):
    required = ("ed_hier_roots", "ed_hier_flat", "ed_select_entity",
                "ed_rename_entity", "ed_reparent_entity", "ed_hierarchy_reveal_scroll",
                "ed_inspector_response", "ed_inspector_draw")
    result = {}
    for name in required:
        series = samples["phase_ms"].get(name)
        if series is None or len(series) != len(samples["frame_indices"]):
            raise ValueError("missing or unaligned operation phase: " + name)
        if any(math.isinf(value) or math.isfinite(value) and value < 0 for value in series):
            raise ValueError("invalid operation phase duration: " + name)
        observed = [(int(frame), float(value)) for frame, value in
                    zip(samples["frame_indices"], series) if math.isfinite(value) and value > 0]
        zero_count = sum(math.isfinite(value) and value == 0 for value in series)
        if not observed:
            if not zero_count:
                raise ValueError("no measured operation phase records: " + name)
            result[name] = dict(status="UNAVAILABLE_BELOW_CSV_RESOLUTION",
                                scope="recorded zero values; no positive duration resolved",
                                csv_resolution_ms=0.0001, positive_observations=[], positive_count=0,
                                zero_count=zero_count, absent_count=len(series)-zero_count,
                                median_ms=None, p95_ms=None, p99_ms=None, max_ms=None)
            continue
        values = [value for _, value in observed]
        result[name] = dict(status="OBSERVED", csv_resolution_ms=0.0001,
                            scope="per-frame accumulated CPU phase; positive observations only",
                            positive_observations=[dict(frame=f, ms=v) for f, v in observed],
                            positive_count=len(values), zero_count=zero_count,
                            absent_count=len(series)-len(values)-zero_count,
                            median_ms=statistics.median(values), p95_ms=percentile(values, .95),
                            p99_ms=percentile(values, .99), max_ms=max(values))
    return result


def validate_rename_history(text):
    entities = []
    for frame, phase in ((91, "undo"), (111, "redo")):
        rows = re.findall(r"rename-history: frame=" + str(frame) + " phase=" + phase +
                          r" entity=(\d+) correct=1 name=(\S+)", text)
        if len(rows) != 1:
            raise ValueError("actual rename Undo/Redo result missing: " + phase)
        entities.append(int(rows[0][0]))
    if entities[0] != entities[1] or entities[0] == 0:
        raise ValueError("rename Undo/Redo entity identity changed")


def validate_display_name(text, expected, first_frame, last_frame=200):
    rows = re.findall(r"inspector-complete: generation=\d+ entity=\d+ frame=(\d+) status=drawn latency_ms=[\d.]+ display_name=(\S+)", text)
    if not any(first_frame <= int(frame) <= last_frame and name == expected for frame, name in rows):
        raise ValueError("Inspector did not consume/display expected name: " + expected)


def scene(count, layout="flat"):
    if layout not in ("flat", "wide") or count < 3:
        raise ValueError("valid layout and at least three nodes required")
    result = {"entities": [{"id": i + 1, "name": "Node%u" % i,
                           "components": [{"type": "Transform", "posX": 0, "posY": 0, "posZ": 0,
                                           "scaleX": 1, "scaleY": 1, "scaleZ": 1}]}
                         for i in range(count)]}
    if layout == "wide":
        # Public JSON loader accepts parentId; source ID 1 maps to Node0.
        for entity in result["entities"][1:]:
            entity["parentId"] = 1
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--count", type=int, choices=(10000, 100000, 200000), required=True)
    parser.add_argument("--disable-hierarchy-cache", action="store_true")
    parser.add_argument("--compare-with", type=Path, help="opposite cache control result from same binary/fixture")
    parser.add_argument("--rename-history", action="store_true")
    parser.add_argument("--layout", choices=("flat", "wide"), default="flat")
    parser.add_argument("--backend", default="d3d11")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    fixture, log, frames = out / "scene.json", out / "editor.log", out / "frames.csv"
    fixture.write_text(json.dumps(scene(args.count, args.layout)) + "\n", encoding="utf-8", newline="\n")
    env = {"JCE_SCENE": str(fixture), "JCE_BACKEND": args.backend, "JCE_MULTI_INSTANCE": "1",
           "JCE_DBG_SCENE_LOAD_SYNC": "1",
           "JCE_EDITOR_PROFILE_DIR": str(out / "profile"), "JCE_EDITOR_OPERATION_METRICS": "1",
           "JCE_DBG_SELECT": "Node0@60", "JCE_DBG_RENAME": "Node0:Renamed@80",
           "JCE_DBG_REPARENT": "Renamed:Node1@100", "JCE_DBG_SCROLL": "Node%u@70" % (args.count-1),
           "JCE_WINCAP_FRAME": "120", "JCE_WINCAP_PATH": str(out / "editor.png")}
    fixture_digest = sha256_file(fixture)
    if args.layout == "wide":
        # Selection before frame50 expands the root through the ordinary reveal path.
        # Rename/reparent a sibling, never the root onto its descendant (cycle).
        env["JCE_DBG_SELECT"] = "Node%u@30" % (args.count - 1)
        env["JCE_DBG_RENAME"] = "Node2:Renamed@80"
    if args.rename_history:
        env["JCE_DBG_UNDO"] = "90"
        env["JCE_DBG_REDO"] = "110"
        env["JCE_DBG_RENAME_VERIFY"] = "91,111"
        env["JCE_DBG_REPARENT"] = "Renamed:Node1@130"
        env["JCE_WINCAP_FRAME"] = "150"
    env.update(hierarchy_cache_environment(args.disable_hierarchy_cache))
    digest = sha256_file(args.exe)
    process = run(env, log, frames=200, timeout=600, exe=args.exe.resolve(), samples_path=frames)
    text = log.read_text(encoding="utf-8", errors="replace")
    actual_backend = assert_backend(text + "\n" + Path(process["process"]["stdout_path"]).read_text(encoding="utf-8", errors="replace"), args.backend)
    if process["process"].get("returncode") != 0 or process["process"].get("timed_out"):
        raise ValueError("editor did not exit normally")
    if actual_backend != process["backend_actual"] or sha256_file(fixture) != fixture_digest:
        raise ValueError("actual backend or source fixture differs")
    if "] ERROR -" in text or sha256_file(args.exe) != digest:
        raise ValueError("editor ERROR or binary changed")
    cache_observations = validate_cache_observations(text, args.disable_hierarchy_cache)
    observations = validate_observations(text, args.count, args.layout)
    for operation in ("rename", "reparent"):
        if len(re.findall(r"editor-command: operation=" + operation + r" entity=\d+ frame=\d+ correct=1 parent=\d+ name=\S+", text)) != 1:
            raise ValueError("missing actual operation: " + operation)
    if not re.search(r"hierarchy-reveal: entity=\d+ visible=1 frame=70", text):
        raise ValueError("target row was not submitted for reveal")
    if args.rename_history:
        validate_rename_history(text)
    if args.layout == "flat":
        validate_display_name(text, "Renamed", 80, 89 if args.rename_history else 200)
        if args.rename_history:
            validate_display_name(text, "Node0", 90, 109)
            validate_display_name(text, "Renamed", 110)
    picture = out / "editor.png"
    capture_dimensions = validate_capture(picture)
    samples = read_frames(frames, warmup=0, sample_count=200)
    phases = operation_phases(samples)
    result = dict(status="COMMAND_OBSERVATIONS", functional_status="COMMAND_OBSERVATIONS",
                  metrics_status="INCOMPLETE" if any(row["status"] != "OBSERVED" for row in phases.values()) else "OBSERVED",
                  metrics_claim="CPU operation observations only; unresolved phases prevent complete D01 performance acceptance",
                  layout=args.layout, scope=__doc__, setup_scope="existing synchronous scene opener before UI frame loop; excluded from UI phase timings; process RSS/elapsed include setup", environment=env,
                  executable_sha256=digest, scene_sha256=fixture_digest, backend_actual=actual_backend,
                  log_sha256=sha256_file(log), frames_sha256=sha256_file(frames),
                  capture_sha256=sha256_file(picture), capture_dimensions=capture_dimensions, process=process,
                  raw_sha256={str(p.relative_to(out)):sha256_file(p) for p in out.rglob("*") if p.is_file()},
                  observations=observations, hierarchy_cache=cache_observations, operation_phases=phases, frame=frame_summary(samples),
                  acceptance_scope={"protocol": "D01", "workload_count": args.count,
                    "physical_input_to_display": "UNAVAILABLE",
                    "scroll_scope": "programmatic reveal and submitted target row; not continuous wheel scrolling",
                    "repetition_scope": "one process; rename/reparent are single operation observations",
                    "publication_status": "requires repeated exclusive runs and protocol review; no performance PASS inferred"})
    if args.compare_with:
        result["cache_comparison"] = compare_cache_controls(result, args.compare_with.resolve())
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")

if __name__ == "__main__":
    main()
