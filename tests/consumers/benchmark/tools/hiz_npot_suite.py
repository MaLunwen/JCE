"""Actual GPU survivor proof for NPOT depth footprints; never a timing suite."""
import argparse
import json
from pathlib import Path
import uuid

from gpu_counter_evidence import read_survivors
from culling_suite import sha256_file, source_identity
from tools.benchmark_suite import host_render_environment
from tools.perf_bench import run


def expected(frame):
    return 0 if frame in (50, 90) else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "vulkan", "gl"), required=True)
    parser.add_argument("--shape", choices=("square", "reduce", "thin"), default="square")
    args = parser.parse_args()
    if not args.exe.is_file():
        parser.error("existing SDK-consumer binary required")
    root = args.out.resolve() / uuid.uuid4().hex[:12]
    root.mkdir(parents=True, exist_ok=False)
    env = dict(host_render_environment("player", root), JCE_BACKEND=args.backend,
               JCE_BENCH_HIZ_NPOT=args.shape, JCE_BENCH_GPU_SURVIVORS=str(root / "survivors.jsonl"))
    result = dict(status="FAILED", source=source_identity(), backend=args.backend, shape=args.shape,
                  executable_sha256=sha256_file(args.exe), environment=env,
                  scope="SDK GPUScene with synthetic depth; actual asynchronous counters",
                  dimensions=[10, 10] if args.shape == "reduce" else [1, 5] if args.shape == "thin" else [5, 5],
                  fixtures={20: "clear source row", 50: "fully covered", 79: "clear source column",
                            80: "fully clear", 81: "clear source pixel", 82: "clear last row", 90: "fully covered"},
                  limitations=["diagnostic only; no rendered-scene or performance claim"])
    output = root / "suite.json"
    try:
        result["process"] = run(env, root / "engine.log", frames=181, exe=args.exe.resolve())
        log = (root / "engine.log").read_text(encoding="utf-8")
        if "] ERROR -" in log or log.count("benchmark-hiz-npot: initialized") != 1:
            raise ValueError("NPOT diagnostic did not initialize and dispatch cleanly")
        result["gpu_survivors"] = read_survivors(root / "survivors.jsonl", 2, expected)
        result["status"] = "PASS"
    except (ValueError, RuntimeError, OSError, KeyError) as error:
        result["error"] = str(error)
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(output), "status": result["status"], "error": result.get("error")}))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
