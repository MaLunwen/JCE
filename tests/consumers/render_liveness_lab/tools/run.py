"""Run the actual SDK application and retain independent liveness evidence."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))
from tools.process_sampler import run_process
from tools.perf_bench import read_execution_log
from tools.jce_determinism import DETERMINISM, assert_backend, read_png
from validate import PHASES, validate_records, validate_pixels


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8", newline="\n")


def source_identity():
    source = Path(__file__).resolve().parents[1]
    names = ("CMakeLists.txt", "jce_project.json", "src/main.c", "src/fixture.c",
             "src/fixture.h", "tools/prepare_inputs.py", "tools/validate.py", "tools/run.py",
             "tools/test_validate.py", "resources/lab.txt", "AGENTS.md", "README.md")
    return {name: sha(source / name) for name in names}


def execute(exe, inputs, out, timeout):
    exe, inputs, out = (Path(p).resolve() for p in (exe, inputs, out))
    if not out.is_relative_to(ROOT / "build") or out.exists():
        raise ValueError("fresh run output below repository build/ required")
    manifest_path = inputs / "inputs.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("state") != "PACKED_AND_BYTE_VERIFIED" or manifest.get("schema") != 1:
        raise ValueError("actual packed/verified inputs required")
    archives = {}
    for name in ("anchor", "missing_colour", "missing_depth", "full"):
        entry = manifest["archives"][name]
        path = (inputs / entry["path"]).resolve()
        if not path.is_relative_to(inputs) or sha(path) != entry["sha256"] or path.stat().st_size != entry["bytes"]:
            raise ValueError("actual archive identity changed")
        archives[name] = path
    identity = {"exe": sha(exe), "inputs": sha(manifest_path), "sources": source_identity(),
                "archives": {name: sha(path) for name, path in archives.items()}}
    out.mkdir(parents=True, exist_ok=False)
    launch = out / "launch"
    launch.mkdir()
    config = out / "engine.ini"
    config.write_text("[performance]\nmachine_class=full\n[window]\nwidth=960\nheight=540\n",
                      encoding="utf-8", newline="\n")
    log_path = out / "engine.log"
    log_path.touch(exist_ok=False)
    settings = dict(DETERMINISM)
    settings.update({"JCE_WINDOW_HIDDEN": "1", "JCE_HEADLESS": "0",
        "JCE_MULTI_INSTANCE": "1", "JCE_AUDIO_DISABLE": "1", "JCE_GPU_TIER": "high",
        "JCE_NO_GRASS": "0", "JCE_GRASS_DENSITY": "1", "JCE_GRASS_CULL": "0",
        "JCE_NO_POSTFX": "0", "JCE_PERSIST_GRASS": "1",
        "JCE_DISABLE_SHADOWCACHE": "1", "JCE_PERF_LOG": "0", "JCE_TRACE": "1",
        "JCE_BACKEND": manifest["backend"], "JCE_MAX_FRAMES": "10000", "JCE_MAX_SECONDS": "180",
        "JCE_LOG_FILE": str(log_path), "JCE_TRACE_STREAM": str(out / "trace.jsonl"),
        "JCE_TRACE_EXPORT": str(out / "trace.json"), "JCE_LAB_OUTPUT": str(out),
        "JCE_LAB_CONFIG": str(config), "JCE_LAB_ANCHOR": str(archives["anchor"]),
        "JCE_LAB_MISSING_COLOUR": str(archives["missing_colour"]),
        "JCE_LAB_MISSING_DEPTH": str(archives["missing_depth"]),
        "JCE_LAB_FULL": str(archives["full"])})
    environment = dict(os.environ)
    environment.update(settings)
    result = {"state": "INCOMPLETE", "identity": identity,
              "environment": settings,
              "scope": "controlled GPU pixels and CPU submission, not a performance benchmark"}
    write(out / "intent.json", result)
    try:
        process = run_process([str(exe)], env=environment, cwd=str(launch),
                              timeout=timeout, output_dir=out / "process")
        result["process"] = {key: value for key, value in process.items() if key not in ("samples", "stdout", "stderr")}
        if process["timed_out"] or process["returncode"] != 0:
            raise ValueError("actual native process failed or timed out")
        log = read_execution_log(log_path, process)
        result["backend_actual"] = assert_backend(log, manifest["backend"])
        if (out / "failure.json").exists():
            failure = json.loads((out / "failure.json").read_text(encoding="utf-8"))
            raise ValueError("actual lab failed: " + failure["reason"] + " phase=" + str(failure["phase"]))
        complete = json.loads((out / "complete.json").read_text(encoding="utf-8"))
        if complete != {"state": "CAPTURES_COMPLETE", "phases": len(PHASES)}:
            raise ValueError("actual complete capture receipt unavailable")
        records = [json.loads((out / f"{i:02d}-{name}.json").read_text(encoding="utf-8"))
                   for i, name in enumerate(PHASES)]
        images = {name: read_png(out / f"{i:02d}-{name}.png") for i, name in enumerate(PHASES)}
        result["records"] = validate_records(records)
        result["pixels"] = validate_pixels(images)
        for name in ("trace.jsonl", "trace.json"):
            if not (out / name).is_file() or (out / name).stat().st_size == 0:
                raise ValueError("actual lifecycle trace unavailable")
        # Intentional absent grass stages are the fixture's two error cases.
        errors = [line for line in log.splitlines() if " ERROR -" in line]
        allowed = [f"not found in PAK: shaders/fs_grass_{suffix}.bin"
                   for suffix in ("dx11", "spv", "glsl", "essl", "mtl")]
        allowed += [f"not found in PAK: shaders/fs_grass_shadow_{suffix}.bin"
                    for suffix in ("dx11", "spv", "glsl", "essl", "mtl")]
        if any(not any(marker in line for marker in allowed) for line in errors):
            raise ValueError("unexpected engine error outside controlled absent shaders")
        if ("continuous trace capture incomplete" in log or "runtime trace export failed" in log):
            raise ValueError("lifecycle trace incomplete")
        current = {"exe": sha(exe), "inputs": sha(manifest_path), "sources": source_identity(),
                   "archives": {name: sha(path) for name, path in archives.items()}}
        if current != identity:
            raise ValueError("source/application/archive identity changed during run")
        result["state"] = "ACTUAL_GRASS_COLOUR_DEPTH_AND_RECEIVER_SHADOW_OBSERVED"
    except Exception as error:
        result.update(state="FAIL", error=str(error))
        raise
    finally:
        result["raw_sha256"] = {str(path.relative_to(out)): sha(path) for path in out.rglob("*")
                               if path.is_file() and path.name != "result.json"}
        write(out / "result.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("exe", "inputs", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=240)
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("positive finite timeout required")
    try:
        execute(args.exe, args.inputs, args.out, args.timeout)
    except Exception as error:
        print(str(error), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
