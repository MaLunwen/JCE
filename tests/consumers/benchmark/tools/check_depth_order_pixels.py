"""Opt-in public SDK Water/opaque diagnostic; default prepares, never launches."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from tools.benchmark_suite import run_visual_witness
from check_asset_pixels import validate_still_controls
from depth_order_oracle import CASES, scene, validate


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + "\n",
                          encoding="utf-8", newline="\n")


def sources():
    base = Path(__file__).resolve().parents[1]
    names = ("CMakeLists.txt", "jce_project.json", "src/main.c", "src/asset_witness.c", "src/mutations.c", "AGENTS.md",
             "tools/check_depth_order_pixels.py", "tools/depth_order_oracle.py",
             "tools/test_depth_order_pixels.py")
    return {name: sha(base / name) for name in names}


def execute(exe, out, backend="d3d11", launch=False, cases=None):
    exe, out = Path(exe).resolve(), Path(out).resolve()
    selected = tuple(CASES if cases is None else cases)
    if not selected or len(set(selected)) != len(selected) or any(case not in CASES for case in selected):
        raise ValueError("unique registered depth-order cases required")
    if not exe.is_file() or out.exists():
        raise ValueError("explicit existing benchmark EXE and fresh output required")
    out.mkdir(parents=True, exist_ok=False)
    identity = dict(executable_sha256=sha(exe), executable_bytes=exe.stat().st_size,
                    consumer_sources=sources())
    result = dict(status="PREPARED_NOT_EXECUTED", identity=identity, cases={},
                  native_executed=False, actual_readback_qualified=False,
                  gpu_completion_known=False, full_lifecycle_qualified=False,
                  all_required_cases=set(selected) == set(CASES),
                  scope="static diagnostic, not performance or complete lifecycle qualification")
    try:
        for case in selected:
            directory = out / case
            directory.mkdir()
            path = directory / "scene.json"
            write(path, scene(case))
            result["cases"][case] = dict(scene_sha256=sha(path))
        write(out / "intent.json", result)
        if not launch:
            return result
        result["native_executed"] = True
        for case in selected:
            directory = out / case
            path = directory / "scene.json"
            env = dict(JCE_SCENE=str(path), JCE_BACKEND=backend,
                       JCE_WINDOW_HIDDEN="1", JCE_HEADLESS="0", JCE_GPU_TIER="high",
                       JCE_BENCH_ASSET_STILL="1", JCE_BENCH_SHOT_FRAME="60",
                       JCE_LOG_TAA="1", JCE_TRACE="0", JCE_CVAR="",
                       JCE_RP_FORCE="", JCE_NO_GRASS="1",
                       JCE_LAUNCH_TARGET_FPS="60")
            result["cases"][case]["environment"] = env
            write(out / "result.json", result)
            child = run_visual_witness(env, directory, exe=exe, timeout=180,
                                       host="player")
            validate_still_controls((directory / "visual.log").read_text(encoding="utf-8"))
            if sha(path) != result["cases"][case]["scene_sha256"]:
                raise ValueError("authored scene changed during actual process")
            png = directory / "frame.png"
            with Image.open(png) as image:
                dimensions = list(image.size)
            result["cases"][case].update(child=child, png_sha256=sha(png),
                                          png_bytes=png.stat().st_size, dimensions=dimensions)
        images = {}
        for case in selected:
            with Image.open(out / case / "frame.png") as image:
                images[case] = np.asarray(image.convert("RGB"))
        if result["all_required_cases"]:
            result["oracle"] = validate(images)
        if identity != dict(executable_sha256=sha(exe), executable_bytes=exe.stat().st_size,
                            consumer_sources=sources()):
            raise ValueError("benchmark executable or owned consumer source changed")
        result["status"] = ("ACTUAL_STATIC_WATER_OPAQUE_DEPTH_OBSERVED" if result["all_required_cases"]
                            else "CAPTURE_SUBSET_NOT_QUALIFIED")
        result["actual_readback_qualified"] = result["all_required_cases"]
    except Exception as error:
        result.update(status="FAIL", error=str(error))
        raise
    finally:
        result["raw_sha256"] = {str(p.relative_to(out)): sha(p) for p in out.rglob("*")
                                if p.is_file() and p.name != "result.json"}
        write(out / "result.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--backend", default="d3d11")
    parser.add_argument("--case", action="append", choices=CASES,
                        help="repeat to capture a diagnostic subset; subsets never qualify the oracle")
    parser.add_argument("--execute", action="store_true")
    args = parser.parse_args()
    try:
        result = execute(args.exe, args.out, args.backend, args.execute, args.case)
    except (OSError, ValueError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    print(result["status"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
