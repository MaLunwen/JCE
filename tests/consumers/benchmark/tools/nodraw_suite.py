#!/usr/bin/env python3
"""Assetless LineRenderer warm NONE/add/remove correctness, not throughput."""
import argparse
import json
import re
from pathlib import Path
import numpy as np
from gbuffer_asset_suite import image, write, sha256_file
from postfx_fixture import fixture
from tools.benchmark_suite import source_identity
from tools.jce_determinism import assert_backend
from tools.perf_bench import run

MODES = ("empty", "steady", "add", "cycle")

def scene():
    result = fixture(empty=True)
    camera, sun = result["entities"]
    camera["components"][0].update(posX=0, posY=0, posZ=30, rotX=0, rotY=0, rotZ=0)
    camera["components"][1].update(orthographic=True, orthoSize=30, nearClip=.1, farClip=60, clearMode=2)
    sun["components"][1].update(intensity=0, castsShadow=False)
    result["rendering"]["postfx"].update(ssao=False, ssr=False, bloom=False, autoExposure=False, fxaa=False)
    result["entities"].append({"id": 3, "name": "TransitionLine", "components": [
        {"type": "Transform", "scaleX": 1, "scaleY": 1, "scaleZ": 1}]})
    return result

def validate_markers(text, mode):
    rows = re.findall(r"benchmark-nodraw: mode=(\w+) frame=(\d+) added=(\d+) removed=(\d+) lines=(\d+) target=(\d+) present=(\d+) flags=(\d+)", text)
    if len(rows) != 5 or [int(row[1]) for row in rows] != [1, 30, 50, 70, 90]:
        raise ValueError("actual complete transition observations missing")
    target = rows[0][5]
    for row in rows:
        frame = int(row[1])
        expected = int(mode == "steady" or mode == "add" and frame >= 30 or mode == "cycle" and 30 <= frame < 70)
        added = int(mode == "steady" and frame == 1 or mode in ("add", "cycle") and frame == 30)
        removed = int(mode == "cycle" and frame == 70)
        if row[0] != mode or int(target) == 0 or row[5] != target or tuple(map(int, (row[2], row[3], row[4], row[6]))) != (added, removed, expected, expected):
            raise ValueError("actual component population or operation differs")
    return rows

def compare(left, right):
    if left.shape != (720, 1280, 4) or right.shape != left.shape:
        raise ValueError("capture dimensions differ")
    delta = np.abs(left[..., :3].astype(np.int16) - right[..., :3].astype(np.int16))
    return dict(changed_pixels=int(np.count_nonzero(np.max(delta, axis=2) > 2)),
                mean_error=float(delta.mean()), max_error=int(delta.max()))

def validate_pixels(pictures):
    results = {}
    for cache in ("on", "off"):
        empty = pictures[(cache, "empty")]
        for mode in ("steady", "add"):
            changed = compare(pictures[(cache, mode)], empty)
            if changed["changed_pixels"] < 64: raise ValueError("actual line never visibly participates")
            results[cache+"_"+mode+"_changed"] = changed
        for name, left, right in (("warm_steady", "add", "steady"), ("remove_empty", "cycle", "empty")):
            delta = compare(pictures[(cache, left)], pictures[(cache, right)])
            if delta["mean_error"] > .01 or delta["max_error"] > 2:
                raise ValueError("warm transition differs from independent steady control")
            results[cache+"_"+name] = delta
    for mode in MODES:
        delta = compare(pictures[("on", mode)], pictures[("off", mode)])
        if delta["mean_error"] > .01 or delta["max_error"] > 2: raise ValueError("cache state changes actual pixels")
        results[mode+"_cache"] = delta
    return results

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "vulkan", "gl"), default="d3d11")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(); args.exe = args.exe.resolve(); args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    source = args.out/"scene.json"; write(source, scene())
    digest, fixture_hash = sha256_file(args.exe), sha256_file(source)
    result = dict(status="INCOMPLETE", scope=__doc__, source=source_identity(), executable=str(args.exe),
                  executable_sha256=digest, scene_sha256=fixture_hash, cases={})
    pictures = {}
    try:
        for cache in ("on", "off"):
            for mode in MODES:
                directory = args.out/(cache+"_"+mode); directory.mkdir()
                shot = 90 if mode == "cycle" else 50
                env = dict(JCE_SCENE=str(source), JCE_BACKEND=args.backend, JCE_MULTI_INSTANCE="1",
                    JCE_WINDOW_HIDDEN="1", JCE_GPU_TIER="high", JCE_BENCH_ASSET_STILL="1",
                    JCE_BENCH_NODRAW=mode, JCE_BENCH_SHOT_FRAME=str(shot),
                    JCE_BENCH_PLAYER_SHOT=str(directory/"frame.png"), JCE_DBG_LOD_STATS="1",
                    JCE_DISABLE_ECULL_METADATA="1" if cache == "off" else "0",
                    JCE_DISABLE_KINDCACHE="1" if cache == "off" else "0")
                process = run(env, directory/"engine.log", frames=200, exe=args.exe, samples_path=directory/"frames.csv")
                case = dict(environment=env, process=process); result["cases"][cache+"_"+mode] = case
                text = (directory/"engine.log").read_text(encoding="utf-8", errors="replace")
                state = process["process"]
                if state.get("returncode") != 0 or state.get("timed_out"): raise ValueError("consumer process did not exit normally")
                stdout = Path(state["stdout_path"]).read_text(encoding="utf-8", errors="replace")
                actual = assert_backend(text+"\n"+stdout, args.backend)
                if actual != process.get("backend_actual") or "] ERROR -" in text: raise ValueError("actual backend mismatch or consumer error")
                case["backend_actual"] = actual; case["observations"] = validate_markers(text, mode)
                pictures[(cache, mode)] = image(directory/"frame.png")
                case["raw_sha256"] = {str(p.relative_to(directory)): sha256_file(p) for p in directory.rglob("*") if p.is_file()}
                if sha256_file(args.exe) != digest or sha256_file(source) != fixture_hash: raise ValueError("input changed")
        result["pixels"] = validate_pixels(pictures); result["status"] = "PASS"
    except Exception as error:
        result.update(status="FAIL", error=str(error)); raise
    finally:
        result["raw_sha256"] = {str(p.relative_to(args.out)): sha256_file(p) for p in args.out.rglob("*") if p.is_file() and p.name != "result.json"}
        write(args.out/"result.json", result)

if __name__ == "__main__": main()
