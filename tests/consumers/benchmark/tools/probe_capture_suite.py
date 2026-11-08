"""Actual SDK probe GPU workflow witness; no throughput or physical-UI claim."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import statistics
import struct
import sys
import uuid

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))
from tools.editor_portability_probe import sha256, validate_process
from tools.jce_determinism import assert_backend
from tools.lifecycle_trace_report import analyze_trace
from tools.perf_bench import read_execution_log
from tools.process_sampler import run_process
from startup_suite import CANONICAL_DEFAULTS, profile_values

MODES = ("hdr", "ldr", "cancel-ready", "cancel-exit")
WORKER = "renderer.reflection-probe.bake"
COUNTERS = ("readback.cpu_service_open", "readback.cpu_attached_queues",
            "readback.cpu_retiring_queues", "readback.cpu_owned_destinations",
            "readback.cpu_owned_destination_bytes")
COLORS = ((4, .1, .1), (.1, 4, .1), (.1, .1, 4),
          (4, 4, .1), (4, .1, 4), (.1, 4, 4))
AXES = ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1))
MAGIC = b"\xabKTX 11\xbb\r\n\x1a\n"
PROFILE_INPUT_BYTES = b"[renderer]\nvsync=false\n\n[performance]\nmachine_class=full\njob_workers=2\n"
PROFILE_REQUEST = {"renderer.vsync": "false", "performance.machine_class": "full",
                   "performance.job_workers": "2"}
PROFILE_VALIDATOR = ROOT / "tests/consumers/benchmark/tools/startup_suite.py"


def profile_before_launch(authored, runtime):
    """Bind an immutable input snapshot and an identical writable runtime seed."""
    if authored.read_bytes() != runtime.read_bytes():
        raise ValueError("runtime profile seed differs from input snapshot")
    intended = profile_values(authored)
    if intended != PROFILE_REQUEST:
        raise ValueError("probe profile request fields differ")
    return {"input_path": authored.name, "runtime_path": runtime.name,
            "input_sha256_before": sha256(authored), "runtime_sha256_before": sha256(runtime),
            "requested": intended, "status": "IDENTICAL_RUNTIME_SEED_OBSERVED"}


def observe_profile_outputs(authored, runtime, observation):
    """Keep post-run bytes observable even when another workflow check fails."""
    errors = {}
    for label, path in (("input", authored), ("runtime", runtime)):
        try:
            observation[label + "_sha256_after"] = sha256(path)
        except OSError as error:
            observation[label + "_sha256_after"] = None
            errors[label] = str(error)
    observation["read_errors"] = errors
    observation["input_snapshot_unchanged"] = (
        observation["input_sha256_after"] == observation["input_sha256_before"])
    runtime_hash = observation["runtime_sha256_after"]
    observation["runtime_mutated"] = (runtime_hash != observation["runtime_sha256_before"]
                                      if runtime_hash is not None else None)


def validate_profile_after(authored, runtime, observation, backend):
    """Validate persisted intent, not a claim of independently measured GPU state."""
    observe_profile_outputs(authored, runtime, observation)
    if not observation["input_snapshot_unchanged"]:
        raise ValueError("profile input snapshot changed or unavailable")
    if observation["read_errors"]:
        raise ValueError("runtime profile output unavailable")
    actual = profile_values(runtime)
    observation["runtime_fields"] = actual
    if observation["requested"] != PROFILE_REQUEST:
        raise ValueError("probe profile request observation changed")
    for key, value in observation["requested"].items():
        if actual.get(key) != value:
            raise ValueError("requested runtime profile field changed or missing: " + key)
    if backend not in ("d3d11", "d3d12", "vulkan", "gl"):
        raise ValueError("unsupported profile backend request")
    resolved = "opengl" if backend == "gl" else backend
    # Reuse the existing canonical persistence policy. This Engine also emits
    # the existing maximized default when it serializes its complete config.
    defaults = {**CANONICAL_DEFAULTS, "window.maximized": "false"}
    for key in set(actual) - set(observation["requested"]):
        if key == "renderer.backend_resolved":
            if actual[key] != resolved:
                raise ValueError("resolved profile backend differs")
        elif key not in defaults or actual[key] != defaults[key]:
            raise ValueError("unexpected runtime profile mutation: " + key)
    observation["resolved_backend"] = actual.get("renderer.backend_resolved")
    observation["status"] = "PERSISTED_REQUEST_FIELDS_PRESERVED"
    observation["scope"] = "immutable authored bytes and writable runtime persistence; not independent effective GPU settings"
    return observation


def fixture(mode):
    if mode not in MODES:
        raise ValueError("unknown capture mode")
    entities = []

    def add(name, position, scale, component):
        entities.append({"id": len(entities) + 1, "parentId": 0, "name": name,
            "components": [{"type": "Transform",
                **dict(zip(("posX", "posY", "posZ"), position)),
                **dict(zip(("scaleX", "scaleY", "scaleZ"), scale))}, component]})

    add("Camera", (0, 0, 0), (1, 1, 1), {"type": "Camera", "primary": True,
        "fov": 60, "nearClip": .1, "farClip": 20, "clearMode": 2})
    add("CaptureProbe", (0, 0, 0), (1, 1, 1), {"type": "ReflectionProbe",
        "mode": 0, "resolution": 16, "hdr": mode != "ldr", "nearClip": .1,
        "farClip": 20, "intensity": 0, "boxSizeX": 1, "boxSizeY": 1, "boxSizeZ": 1})
    for face, (axis, color) in enumerate(zip(AXES, COLORS)):
        add("FaceMarker%d" % face, tuple(v * 3 for v in axis), (1.5, 1.5, 1.5),
            {"type": "MeshRenderer", "meshShape": 0, "visible": True,
             "baseColorR": 0, "baseColorG": 0, "baseColorB": 0, "baseColorA": 1,
             "roughness": 1, "metallic": 0, "emissiveR": color[0],
             "emissiveG": color[1], "emissiveB": color[2]})
    return {"entities": entities, "rendering": {"iblEnabled": False, "dynamicGI": 0,
        "ambient": {"color": [0, 0, 0], "intensity": 0},
        "postfx": {"tonemap": False, "bloom": False, "ssao": False, "ssr": False,
                   "fxaa": False, "vignette": False, "autoExposure": False}}}


def decode_ktx(data, *, hdr, sidecar=False):
    """Independent bounded KTX1 inspection, never an Engine implementation."""
    if len(data) < 64 or len(data) > 2 * 1024 * 1024 or data[:12] != MAGIC:
        raise ValueError("missing/budgeted KTX1 header")
    endian, kind, size, fmt, internal, base, width, height, depth, arrays, faces, mips, keys = struct.unpack_from("<13I", data, 12)
    expected_kind, expected_size, expected_internal = (0x140B, 2, 0x881A) if hdr else (0x1401, 1, 0x8058)
    # Older SDK output may carry bimg's exact (0,1,0) convention. Qualify each
    # observed header independently; normalized typed SDK output is distinct.
    typed_header = (kind, size, fmt) == (expected_kind, expected_size, 0x1908)
    pinned_header = (kind, size, fmt) == (0, 1, 0)
    bytes_per_pixel = 8 if hdr else 4
    expected_width = 32 if sidecar else 16
    expected_mips = 1 if sidecar else 5
    if (endian != 0x04030201 or internal != expected_internal or
        not (typed_header or pinned_header) or base != 0x1908 or width != expected_width or height != width or
        depth != 0 or arrays != 0 or faces != 6 or mips != expected_mips or keys > len(data) - 64):
        raise ValueError("actual KTX format/face/dimension/mip contract differs")
    offset = 64 + keys
    decoded = []
    for mip in range(mips):
        w = max(1, width >> mip)
        if offset + 4 > len(data):
            raise ValueError("truncated mip size")
        byte_count, = struct.unpack_from("<I", data, offset)
        offset += 4
        if byte_count != w * w * bytes_per_pixel:
            raise ValueError("mip imageSize is not a tightly packed face")
        level = []
        for _ in range(6):
            if offset + byte_count > len(data):
                raise ValueError("truncated cube face")
            raw = data[offset:offset + byte_count]
            values = list(struct.unpack("<%de" % (w * w * 4), raw)) if hdr else [v / 255 for v in raw]
            if any(not math.isfinite(v) or v < 0 for v in values):
                raise ValueError("nonfinite/negative captured radiance")
            level.append(values)
            offset += (byte_count + 3) & ~3
        decoded.append(level)
    if offset != len(data):
        raise ValueError("trailing/unaccounted KTX payload")
    return {"width": width, "mips": mips, "hdr": hdr, "levels": decoded,
            "header_layout": "typed" if typed_header else "pinned_bimg_zero_gl_type"}


def validate_directions(cube):
    width = cube["width"]
    samples = []
    for face, values in enumerate(cube["levels"][0]):
        colors = [[] for _ in range(3)]
        for y in range(width // 2 - 1, width // 2 + 1):
            for x in range(width // 2 - 1, width // 2 + 1):
                for channel in range(3):
                    colors[channel].append(values[(y * width + x) * 4 + channel])
        center = [statistics.median(v) for v in colors]
        for actual, expected in zip(center, COLORS[face]):
            if expected > 1:
                if actual < (2 if cube["hdr"] else .98):
                    raise ValueError("directional emissive marker missing or clamped HDR")
            elif actual > .35:
                raise ValueError("face order/directional marker channels differ")
        samples.append(center)
    return samples


def validate_host(mode, report, events):
    if (report.get("schema") != 1 or report.get("case") != mode or
        report.get("complete") is not True or report.get("failed") is not False or
        report.get("trace_lost") != "0"):
        raise ValueError("host did not finish its actual owned workflow")
    baseline = [e for e in events if e["event"] == "zero_baseline"]
    if len(baseline) != 1 or any(baseline[0][k] != 0 for k in ("attached", "retiring", "destinations", "bytes", "stop_releases")) or baseline[0]["service"] != 1:
        raise ValueError("isolated readback zero baseline unavailable")
    polls = [e for e in events if e["event"] == "capture_poll"]
    if not polls or len(polls) != report["polls"] or any(e["a"] != e["b"] for e in polls):
        raise ValueError("missing actual polls or an extra implicit frame submit")
    begins = [e for e in events if e["event"] == "capture_begin"]
    done = [e for e in events if e["event"] == "capture_faces_done_bake_submitted"]
    if len(begins) != (2 if mode == "cancel-ready" else 1) or len(done) != (0 if mode == "cancel-exit" else 1):
        raise ValueError("actual capture begin/terminal count differs")
    if mode != "cancel-exit" and (len(polls) < (13 if mode == "cancel-ready" else 12) or polls[-1]["c"] != "2"):
        raise ValueError("six render/readback face transitions unavailable")
    cancelled = mode.startswith("cancel-")
    cancels = [e for e in events if e["event"] == "cancel_pending"]
    if cancelled:
        if len(cancels) != 1:
            raise ValueError("one actual pending cancellation required")
        e = cancels[0]
        if (e["frame"] != polls[0]["frame"] or e["a"] != e["b"] or e["c"] != "1" or e["service"] != 1 or
            e["attached"] != 0 or e["retiring"] <= 0 or e["destinations"] <= 0 or
            e["bytes"] <= 0 or e["stop_releases"] != 0 or report["pending_retained"] is not True):
            raise ValueError("pending CPU destination retention was not observed")
    elif cancels:
        raise ValueError("unexpected cancellation")
    if mode == "cancel-ready":
        ready = [e for e in events if e["event"] == "collected_ready"]
        if len(ready) != 1 or report["collected_ready"] is not True:
            raise ValueError("normal central collect was not observed")
        e = ready[0]
        if e["a"] == e["b"] or e["service"] != 1 or any(e[k] != 0 for k in ("attached", "retiring", "destinations", "bytes", "stop_releases")):
            raise ValueError("retirement was not a live-service ready collection")
    expected_workers = 0 if mode == "cancel-exit" else 1
    if report["worker_submits"] != expected_workers or report["worker_ends"] != expected_workers:
        raise ValueError("actual bake worker count differs")


def validate_trace(document, mode, host):
    analysis = analyze_trace(document,
        ["engine.app_init", "engine.app_exit", "engine.async_shutdown", "engine.renderer_destroy"],
        require_counter_last_zero=COUNTERS)
    return validate_scope_projection(document, analysis, mode, host)


def validate_scope_projection(document, analysis, mode, host):
    """Additional consumer rules, after the real full trace validator succeeds."""
    workers = [s for s in analysis["scopes"] if s["name"] == WORKER and s["category"] == "task"]
    expected = 0 if mode == "cancel-exit" else 1
    if len(workers) != expected or any(s["task_kind"] != 1 or s["state"] != 0 or
            s["owner"] != int(host["worker_task"]) for s in workers):
        raise ValueError("real successful ASYNC bake scope unavailable")
    # CLI --require intentionally accepts SERVICE only. The existing analyzer
    # validates every B/E pair; this additional check identifies the real ASYNC.
    events = document["traceEvents"]
    turns = [e for e in events if e.get("ph") == "C" and e.get("name") == "bench.probe.turn"]
    exits = [e for e in events if e.get("ph") == "C" and e.get("name") == "bench.probe.exit"]
    if len(turns) != host["turns"] or len(exits) != 1 or not turns:
        raise ValueError("complete host turn boundaries unavailable")
    boundaries = turns + exits
    advances = [e for e in events if e.get("ph") == "E" and e.get("name") == "renderer.frame_advance_cpu"]
    for first, last in zip(boundaries, boundaries[1:]):
        if first["tid"] != last["tid"] or first["ts"] > last["ts"]:
            raise ValueError("host turn order/thread differs")
        matches = [e for e in advances if e["tid"] == first["tid"] and first["ts"] <= e["ts"] < last["ts"]]
        if len(matches) != 1:
            raise ValueError("one ordinary central submit per diagnostic draw required")
    releases = [e for e in events if e.get("ph") == "C" and
                e.get("name") == "readback.backend_stop_cpu_releases"]
    if not releases:
        raise ValueError("backend-stop release observation unavailable")
    last_releases = releases[-1].get("args", {}).get("value")
    if type(last_releases) is not int or last_releases < 0:
        raise ValueError("invalid backend-stop release observation")
    classification = "NOT_REQUESTED"
    if mode.startswith("cancel-"):
        cancels = [e for e in events if e.get("ph") == "C" and e.get("name") == "bench.probe.cancel"]
        destroy_begin = [e for e in events if e.get("ph") == "B" and e.get("name") == "engine.renderer_destroy"]
        destroy_end = [e for e in events if e.get("ph") == "E" and e.get("name") == "engine.renderer_destroy"]
        if len(cancels) != 1 or len(destroy_begin) != 1 or len(destroy_end) != 1:
            raise ValueError("cancel/renderer teardown boundaries unavailable")
        ledger, samples = {}, []
        for event in events:
            name = event.get("name")
            if event.get("ph") != "C" or name not in (*COUNTERS, "readback.backend_stop_cpu_releases"):
                continue
            ledger[name] = event.get("args", {}).get("value")
            if name == "readback.backend_stop_cpu_releases" and len(ledger) == 6:
                samples.append((event, dict(ledger)))
        def zero(row):
            return all(row.get(name) == 0 for name in COUNTERS[1:])
        after_cancel = [(event, row) for event, row in samples if event["ts"] >= cancels[0]["ts"]]
        retained = [(event, row) for event, row in after_cancel if row[COUNTERS[0]] == 1 and
            row[COUNTERS[2]] > 0 and row[COUNTERS[3]] > 0 and row[COUNTERS[4]] > 0]
        ready = [(event, row) for event, row in after_cancel if event["ts"] < destroy_begin[0]["ts"] and
            row[COUNTERS[0]] == 1 and zero(row) and row["readback.backend_stop_cpu_releases"] == 0]
        if not retained:
            raise ValueError("actual trace lacks retained pending destinations after cancel")
        if mode == "cancel-ready":
            if not ready or any(e.get("args", {}).get("value") != 0 for e in releases):
                raise ValueError("cancel-ready did not collect while backend remained live")
            classification = "READY_COLLECT_BEFORE_STOP_OBSERVED"
        elif last_releases:
            stopped = [(event, row) for event, row in after_cancel if
                destroy_begin[0]["ts"] <= event["ts"] <= destroy_end[0]["ts"] and
                row[COUNTERS[0]] == 0 and zero(row) and row["readback.backend_stop_cpu_releases"] > 0]
            if not stopped:
                raise ValueError("after-stop release lacks actual renderer-destroy ownership")
            classification = "BACKEND_STOP_CPU_RELEASE_OBSERVED"
        elif ready:
            classification = "READY_COLLECT_BEFORE_STOP_OBSERVED_AFTER_STOP_BRANCH_UNAVAILABLE"
        else:
            raise ValueError("cancel-exit reclamation path is unqualified")
    return {"status": analysis["status"], "turns": len(turns), "async_bakes": len(workers),
            "counter_controls": analysis["counter_controls"], "capture": analysis["capture"],
            "backend_stop_cpu_releases": last_releases,
            "cpu_reclamation_path": classification}


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8", newline="\n")


def run_case(exe, mode, backend, directory, timeout, gpu_adapter=None):
    directory.mkdir(parents=True, exist_ok=False)
    scene, config = directory / "scene.json", directory / "profile.ini"
    authored = directory / "profile.input.ini"
    write(scene, fixture(mode))
    authored.write_bytes(PROFILE_INPUT_BYTES)
    config.write_bytes(PROFILE_INPUT_BYTES)
    profile = profile_before_launch(authored, config)
    identity = {"exe": sha256(exe), "scene": sha256(scene), "config": sha256(authored),
                "validator": sha256(Path(__file__)), "profile_validator": sha256(PROFILE_VALIDATOR)}
    source_paths = ("src/main.c", "src/probe_capture_witness.c", "src/probe_capture_witness.h", "CMakeLists.txt")
    consumer = ROOT / "tests/consumers/benchmark"
    source_identity = {name: sha256(consumer / name) for name in source_paths}
    env = {k: v for k, v in os.environ.items() if not k.startswith("JCE_")}
    env.update(JCE_SCENE=str(scene), JCE_WINDOW_HIDDEN="1", JCE_MULTI_INSTANCE="1",
        JCE_BACKEND=backend, JCE_BENCH_CONFIG_FILE=str(config),
        JCE_BENCH_PROBE_CAPTURE=mode, JCE_BENCH_PROBE_CAPTURE_DIR=str(directory),
        JCE_MAX_FRAMES="3600", JCE_MAX_SECONDS="55", JCE_TRACE="1",
        JCE_TRACE_STREAM=str(directory / "trace.json"), JCE_TRACE_EXPORT="",
        JCE_LOG_FILE=str(directory / "engine.log"), JCE_KPI_FRAME_LOG=str(directory / "frames.csv"),
        JCE_FRAME_LOG_CONTEXT="1", JCE_PERF_LOG="0")
    if gpu_adapter:
        env["JCE_GPU_ADAPTER"] = gpu_adapter
    result = {"status": "FAIL", "case": mode, "identity": identity, "consumer_source": source_identity,
        "profile": profile,
        "environment": {k: v for k, v in env.items() if k.startswith("JCE_")},
        "scope": "actual hidden SDK capture/CPU ownership/file witness; not Editor UI or throughput",
        "limitations": ["capture DONE means bake submit; no public bake handle is exposed",
                        "no post-capture bake cancel/handle-release claim",
                        "ready collection and backend-stop counters are CPU ownership, not GPU idle",
                        "RGBA8 source fallback requires actual device capability; LDR output does not prove it",
                        "when a legacy zero-type uncompressed KTX header is observed, it is compatibility-only; typed headers are qualified separately",
                        "axis marker centers verify direction/face identity, not corner rotation or whole-scene fidelity"]}
    write(directory / "intent.json", result)
    try:
        if profile_before_launch(authored, config) != profile:
            raise ValueError("profile seed changed before launch")
        process = run_process([str(exe)], env=env, cwd=str(exe.parent), timeout=timeout,
                              output_dir=directory / "process", marker_path=directory / "probe-host.json")
        result["process"] = process
        observe_profile_outputs(authored, config, profile)
        validate_process(process)
        log = read_execution_log(directory / "engine.log", process)
        if re.search(r"\b(?:ERROR|FATAL)\s*-", log):
            raise ValueError("actual execution reported an error")
        result["backend_actual"] = assert_backend(log, backend)
        host = json.loads((directory / "probe-host.json").read_text(encoding="utf-8"))
        events = [json.loads(line) for line in (directory / "probe-events.jsonl").read_text(encoding="utf-8").splitlines()]
        validate_host(mode, host, events)
        trace = directory / "trace.json"
        if trace.stat().st_size > 64 * 1024 * 1024:
            raise ValueError("trace inspection budget exceeded")
        result["trace"] = validate_trace(json.loads(trace.read_text(encoding="utf-8")), mode, host)
        result["host"] = host
        for filename in ("cancelled.ktx", "cancelled.irr.ktx"):
            if (directory / filename).exists():
                raise ValueError("cancelled capture published an artifact")
        if mode == "cancel-exit":
            if any((directory / name).exists() for name in ("probe.ktx", "probe.irr.ktx")):
                raise ValueError("cancel-exit unexpectedly baked")
        else:
            hdr = mode != "ldr"
            cube = decode_ktx((directory / "probe.ktx").read_bytes(), hdr=hdr)
            irr = decode_ktx((directory / "probe.irr.ktx").read_bytes(), hdr=hdr, sidecar=True)
            result["ktx"] = {"width": cube["width"], "mips": cube["mips"], "hdr": hdr,
                             "face_center_rgb": validate_directions(cube), "irradiance_width": irr["width"],
                             "header_layout": cube["header_layout"], "irradiance_header_layout": irr["header_layout"],
                             "ktx1_standard_header_valid": cube["header_layout"] == irr["header_layout"] == "typed",
                             "container_issue": "PINNED_WRITER_NONCONFORMANT_UNCOMPRESSED_HEADER"
                                if "pinned_bimg_zero_gl_type" in (cube["header_layout"], irr["header_layout"]) else None}
        validate_profile_after(authored, config, profile, backend)
        if identity != {"exe": sha256(exe), "scene": sha256(scene), "config": sha256(authored),
                        "validator": sha256(Path(__file__)), "profile_validator": sha256(PROFILE_VALIDATOR)}:
            raise ValueError("inputs changed during workflow")
        if source_identity != {name: sha256(consumer / name) for name in source_paths}:
            raise ValueError("consumer source changed during workflow")
        result["status"] = "ACTUAL_PROBE_WORKFLOW_OBSERVED"
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        observe_profile_outputs(authored, config, profile)
        result["artifacts"] = {p.relative_to(directory).as_posix(): sha256(p)
            for p in directory.rglob("*") if p.is_file() and p.name != "result.json"}
        write(directory / "result.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "vulkan", "gl"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--stage", choices=("complete", "cancel", "all"), default="complete")
    parser.add_argument("--gpu-adapter", choices=("auto", "intel", "nvidia", "amd"))
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    if not math.isfinite(args.timeout) or args.timeout < 60 or args.timeout > 300:
        parser.error("timeout must be finite in 60..300 seconds")
    root = args.out.resolve() / uuid.uuid4().hex
    root.mkdir(parents=True, exist_ok=False)
    cases = MODES[:2] if args.stage == "complete" else MODES[2:] if args.stage == "cancel" else MODES
    suite = {"status": "INCOMPLETE", "cases": {}, "exe_sha256": sha256(exe)}
    try:
        for mode in cases:
            suite["cases"][mode] = run_case(exe, mode, args.backend, root / mode,
                                               args.timeout, args.gpu_adapter)
            print(mode + ": ACTUAL_PROBE_WORKFLOW_OBSERVED", flush=True)
        suite["status"] = "SELECTED_PROBE_WORKFLOWS_OBSERVED"
    finally:
        write(root / "suite.json", suite)
    print(root / "suite.json")


if __name__ == "__main__":
    main()
