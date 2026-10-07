"""SDK Player frustum A/B with known front/offscreen/behind populations."""
import argparse
import json
from pathlib import Path
import re
import statistics
import sys
import uuid

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from check_asset_pixels import full_pixel_difference, validate_still_controls
from tools.benchmark_scene import scene_document
from tools.benchmark_suite import (frame_summary, host_render_environment, player_counters,
                                   read_frames, run_visual_witness, sha256_file, source_identity)
from tools.jce_determinism import read_png
from tools.perf_bench import run, read_execution_log
from tools.jce_determinism import assert_backend

MARKER = re.compile(r"benchmark-culling: frame=(\d+) enabled=(\d+) candidates=(\d+) "
                    r"visible=(\d+) culled=(\d+) expected_visible=(\d+)")


def validate_counts(log, count, enabled):
    rows = [tuple(map(int, values)) for values in MARKER.findall(log)]
    visible = (count + 2) // 3 + 3 if enabled else count + 3
    expected = [(frame, int(enabled), count + 3, visible, count + 3 - visible, visible)
                for frame in (20, 50)]
    if rows != expected:
        raise ValueError("culling counters differ from independently known groups")
    validate_still_controls(log)
    if "] ERROR -" in log or "benchmark-player: entered Play" not in log:
        raise ValueError("the culling Player did not complete a clean simulation")
    return rows


def fixture(count):
    document = scene_document("entity", count)
    camera = document["entities"][0]["components"]
    camera[0].update(posY=0, posZ=10, rotX=0)
    camera[1].update(orthographic=False, farClip=2000)
    return document


def base_environment(scene, backend, mode=None, count=0):
    env = {"JCE_SCENE": str(scene), "JCE_BACKEND": backend,
           "JCE_BENCH_ASSET_STILL": "1", "JCE_BENCH_TEXT_CAPTURE": "1"}
    if mode:
        env.update(JCE_BENCH_CULLING=mode, JCE_BENCH_AUTOSPAWN="3,%u" % count)
    return env


def check_resources(path, count, enabled):
    rows = player_counters(path.read_text(encoding="utf-8"))
    expected_visible = (count + 2) // 3 + 3 if enabled else count + 3
    if len(rows)<2:raise ValueError("actual culling resource samples unavailable")
    for row in rows[1:]:
        cull = row["entity_culling"]
        if row["engine"]["entity_count"] != count + 3 or \
                cull["enabled"] != enabled or cull["candidates"] != count + 3 or \
                cull["visible"] != expected_visible or \
                cull["culled"] != count + 3 - expected_visible:
            raise ValueError("the actual sampled culling population changed")
    return rows


def validate_process(metrics, log, backend):
    process=metrics['process']
    if process['returncode']!=0 or process['timed_out']:
        raise ValueError('abnormal culling process exit')
    actual=assert_backend(log,backend)
    if actual!=metrics['backend_actual']:
        raise ValueError('actual culling backend differs from process')
    return actual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--count", type=int, default=200000)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "gl", "vulkan"), default="d3d11")
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--samples", type=int, default=600)
    parser.add_argument("--witness-only", action="store_true")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not args.exe.is_file() or not 3 <= args.count <= 2000000 or \
            not 1 <= args.repeats <= 20 or not 300 <= args.samples <= 100000:
        parser.error("existing executable, count 3..2000000 and valid repeats/samples required")
    root = args.out.resolve() / uuid.uuid4().hex[:12]
    root.mkdir(parents=True, exist_ok=False)
    scene = root / "scene.json"
    scene.write_text(json.dumps(fixture(args.count), indent=2) + "\n", encoding="utf-8", newline="\n")
    identity, exe_hash = source_identity(), sha256_file(args.exe)
    scene_hash=sha256_file(scene)
    witnesses, images = {}, {}
    for label, mode in (("empty", None), ("off", "off"), ("on", "on"), ("repeat", "on")):
        target = root / label
        env = base_environment(scene, args.backend, mode, args.count)
        witness = run_visual_witness(env, target, exe=args.exe.resolve(), timeout=1800,
                                     expected_workload=(3, args.count) if mode else None, host="player")
        log = (target / "visual.log").read_text(encoding="utf-8")
        witnesses[label] = {"process": witness, "image_sha256": sha256_file(target / "frame.png")}
        if mode:
            witnesses[label]["counters"] = validate_counts(log, args.count, mode == "on")
            witnesses[label]["resources"] = check_resources(target / "engine_counters.jsonl",
                                                              args.count, mode == "on")
        images[label] = read_png(target / "frame.png")
    differences = {"front_coverage": full_pixel_difference(images["empty"], images["on"]),
                   "on_off": full_pixel_difference(images["on"], images["off"]),
                   "repeat_noise": full_pixel_difference(images["on"], images["repeat"])}
    if differences["front_coverage"] <= 0 or differences["on_off"] >= 0.0001 or \
            differences["repeat_noise"] >= 0.0001:
        raise ValueError("culling pixels are empty, mismatched or unstable")
    results = []
    if not args.witness_only:
        for index in range(args.repeats):
            # Alternate order so a fixed first/second run bias is observable.
            for mode in (("off", "on") if index % 2 == 0 else ("on", "off")):
                target = root / ("run-%02u-%s" % (index, mode))
                target.mkdir()
                env = base_environment(scene, args.backend, mode, args.count)
                env.update(host_render_environment("player", target))
                (target / "intent.json").write_text(json.dumps({"environment": env,
                    "source": identity, "executable_sha256": exe_hash, "warmup": 120,
                    "samples": args.samples}, indent=2) + "\n", encoding="utf-8", newline="\n")
                metrics = run(env, target / "engine.log", frames=121 + args.samples,
                              timeout=1800, exe=args.exe.resolve(), samples_path=target / "frames.csv",
                              expected_workload=(3, args.count))
                log=read_execution_log(target / "engine.log",metrics["process"])
                actual=validate_process(metrics,log,args.backend)
                validate_counts(log, args.count, mode == "on")
                if sha256_file(args.exe)!=exe_hash or sha256_file(scene)!=scene_hash:
                    raise ValueError("immutable culling inputs changed")
                resources = check_resources(target / "engine_counters.jsonl", args.count, mode == "on")
                row = {"status": "PASS", "index": index, "mode": mode, "directory": str(target),
                       "frame": frame_summary(read_frames(target / "frames.csv", warmup=120,
                                                           sample_count=args.samples)),
                       "process": metrics, "actual_backend": actual, "resources": resources,
                       "files_sha256": {str(v.relative_to(target)):sha256_file(v) for v in target.rglob("*") if v.is_file()},
                       "raw_sha256": sha256_file(target / "frames.csv")}
                results.append(row)
                print(json.dumps({"mode": mode, "index": index,
                                  "median_ms": row["frame"]["frame_median_ms"]}), flush=True)
    result = {"status": "WITNESS_PASS" if args.witness_only else "PASS", "count": args.count,
              "source": identity, "executable_sha256": exe_hash, "scene_sha256": scene_hash,
              "validator_sha256": sha256_file(Path(__file__)), "warmup":120, "samples":args.samples, "repeats":args.repeats,
              "scope": "whole SDK Player, fixed front/offscreen/behind groups; shadows/TAA disabled; CPU frustum culling, not GPU HiZ",
              "differences": differences, "witnesses": witnesses, "runs": results,
              "median_frame_ms": {mode: statistics.median(
                  row["frame"]["frame_median_ms"] for row in results if row["mode"] == mode)
                  for mode in ("on", "off")} if results else {}}
    output = root / "suite.json"
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"status": result["status"], "output": str(output),
                      "differences": differences, "median_frame_ms": result["median_frame_ms"]}))


if __name__ == "__main__":
    main()
