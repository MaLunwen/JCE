"""Fixed-wall hardware-query A/B and independently captured reveal history."""
import argparse
import json
import math
from pathlib import Path
import re
import statistics
import sys
import uuid

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from check_asset_pixels import full_pixel_difference, image_pair_size, validate_still_controls
from culling_suite import fixture
from tools.benchmark_suite import (frame_summary, host_render_environment, read_frames,
                                   run_visual_witness, sha256_file, source_identity)
from tools.jce_determinism import read_png
from tools.perf_bench import run

MARKER = re.compile(r"benchmark-occlusion: frame=(\d+) enabled=(\d+) total=(\d+) "
                    r"visible=(\d+) occluded=(\d+) warm=(\d+) pending=(\d+) unqueried=(\d+)")
REVEAL_FRAMES = (79, 80, 81, 82, 90)
QUERY_POOL = re.compile(r"occlusion culler created \(entities=\d+, queries=(\d+)/(\d+), share=1, view=252\)")


def validate_history(history, frames=REVEAL_FRAMES):
    if set(history) != {str(frame) for frame in frames} or any(
            not isinstance(value, (int, float)) or not math.isfinite(value) or
            not 0 <= value < 0.0001 for value in history.values()):
        raise ValueError("revealed geometry differs from the disabled-query control")


def cell_changes(first, second, count):
    width, height = image_pair_size(first, second)
    side = math.ceil(math.sqrt((count + 1) // 2))
    focal = height / (2 * math.tan(math.radians(30))) / 15
    step_x, step_y = (1.8 * focal / max(side - 1, 1), 3.6 * focal / max(side - 1, 1))
    ca, cb, a, b = first[2], second[2], first[3], second[3]
    result = []
    for index in range(count):
        slot = index // 2
        x = -0.9 + 1.8 * (slot % side) / (side - 1) if side > 1 else 0
        y = -1.8 + 3.6 * (slot // side) / (side - 1) if side > 1 else 0
        cx = width / 2 + (x + (2.5 if index % 2 else -2.5)) * focal
        cy = height / 2 - y * focal
        box = (math.floor(cx - step_x * 0.4), math.floor(cy - step_y * 0.4),
               math.ceil(cx + step_x * 0.4), math.ceil(cy + step_y * 0.4))
        if not 0 <= box[0] < box[2] <= width or not 0 <= box[1] < box[3] <= height:
            raise ValueError("a projected occlusion cell leaves the viewport")
        result.append(sum(max(abs(a[(py * width + px) * ca + channel] -
                                  b[(py * width + px) * cb + channel]) for channel in range(3)) > 5
                          for py in range(box[1], box[3]) for px in range(box[0], box[2])))
    return result


def validate_counts(log, count, enabled, reveal):
    validate_still_controls(log)
    if "UNAVAILABLE hardware queries" in log:
        raise ValueError("hardware queries are unavailable; this is not a PASS")
    if "] ERROR -" in log or "benchmark-player: entered Play" not in log:
        raise ValueError("occlusion Player did not finish a clean simulation")
    initialized = "benchmark-occlusion: initialized count=%u hidden=%u visible=%u enabled=%u reveal=%u" \
        % (count, (count + 1) // 2, count // 2, enabled, reveal)
    if log.count(initialized) != 1:
        raise ValueError("known hidden/visible population was not initialized")
    pools = [tuple(map(int, values)) for values in QUERY_POOL.findall(log)]
    if enabled and (len(pools) != 1 or not 0 < pools[0][0] <= pools[0][1]):
        raise ValueError("actual hardware query budget missing or invalid")
    rows = [tuple(map(int, values)) for values in MARKER.findall(log)]
    expected_frames = [20, 50] + (list(range(79, 91)) if reveal else [])
    if [row[0] for row in rows] != expected_frames:
        raise ValueError("occlusion history samples missing or duplicated")
    for frame, active, total, visible, occluded, warm, pending, unqueried in rows:
        expected = count + 3 + (0 if reveal and frame >= 80 else 1)
        if active != enabled or total != (expected if enabled else 0) or \
                total != visible + occluded + warm + pending + unqueried or \
                (enabled and frame < 80 and occluded > (count + 1) // 2):
            raise ValueError("occlusion counters violate independent population bounds")
        if enabled and total - unqueried - warm > pools[0][0]:
            raise ValueError("reported query coverage exceeds the hardware budget")
        if enabled and frame in (20, 50) and pools[0][0] >= count + 3 and \
                (unqueried or occluded != (count + 1) // 2):
            raise ValueError("a budget-fitting static hidden population was not fully rejected")
    if reveal and log.count("benchmark-occlusion: reveal frame=80") != 1:
        raise ValueError("the wall reveal route did not run")
    if enabled and not any(row[4] > 0 for row in rows if row[0] in (20, 50)):
        raise ValueError("queries were enabled but hid no known occluded object")
    return rows


def environment(scene, backend, mode, count, reveal=False, shot=50):
    env = {"JCE_SCENE": str(scene), "JCE_BACKEND": backend,
           "JCE_BENCH_ASSET_STILL": "1", "JCE_BENCH_TEXT_CAPTURE": "1",
           "JCE_BENCH_OCCLUSION": mode, "JCE_BENCH_AUTOSPAWN": "3,%u" % count,
           "JCE_BENCH_SHOT_FRAME": str(shot)}
    if reveal:
        env["JCE_BENCH_OCCLUSION_REVEAL"] = "1"
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--count", type=int, default=128)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "gl", "vulkan"), default="d3d11")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--samples", type=int, default=300)
    parser.add_argument("--witness-only", action="store_true")
    parser.add_argument("--static-only", action="store_true", help="diagnostic preflight, excludes reveal acceptance")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not args.exe.is_file() or not 2 <= args.count <= 200000 or \
            not 1 <= args.repeats <= 20 or not 300 <= args.samples <= 100000:
        parser.error("existing executable and valid count/repeats/samples required")
    root = args.out.resolve() / uuid.uuid4().hex[:12]
    root.mkdir(parents=True, exist_ok=False)
    scene = root / "scene.json"
    scene.write_text(json.dumps(fixture(args.count), indent=2) + "\n", encoding="utf-8", newline="\n")
    source, exe_hash = source_identity(), sha256_file(args.exe)
    (root / "intent.json").write_text(json.dumps({"source": source,
        "executable_sha256": exe_hash, "scene_sha256": sha256_file(scene),
        "count": args.count, "backend": args.backend, "repeats": args.repeats,
        "samples": args.samples, "static_only": args.static_only,
        "witness_only": args.witness_only}, indent=2) + "\n", encoding="utf-8", newline="\n")
    captures, images = {}, {}
    cases = [("empty", None, False, 50), ("off", "off", False, 50),
             ("on", "on", False, 50), ("repeat", "on", False, 50)]
    if not args.static_only:
        cases += [("reveal-%s-%u" % (mode, frame), mode, True, frame)
                  for frame in REVEAL_FRAMES for mode in ("off", "on")]
    for label, mode, reveal, shot in cases:
        target = root / label
        env = environment(scene, args.backend, mode, args.count, reveal, shot)
        if mode is None:
            env.pop("JCE_BENCH_OCCLUSION")
            env.pop("JCE_BENCH_AUTOSPAWN")
        process = run_visual_witness(env, target, exe=args.exe.resolve(), timeout=1800,
                                     expected_workload=(3, args.count) if mode else None, host="player")
        counters = validate_counts((target / "visual.log").read_text(encoding="utf-8"),
                                   args.count, mode == "on", reveal) if mode else []
        captures[label] = {"process": process, "counters": counters,
                           "image": str(target / "frame.png"),
                           "image_sha256": sha256_file(target / "frame.png")}
        images[label] = read_png(target / "frame.png")
        print(json.dumps({"capture": label, "counters": counters}), flush=True)
    differences = {"fixture_coverage": full_pixel_difference(images["empty"], images["off"]),
                   "on_off": full_pixel_difference(images["on"], images["off"]),
                   "repeat_noise": full_pixel_difference(images["on"], images["repeat"])}
    if differences["fixture_coverage"] <= 0 or any(differences[key] >= 0.0001
                                                 for key in ("on_off", "repeat_noise")):
        raise ValueError("occlusion changed visible geometry or produced unstable pixels")
    history = {str(frame): full_pixel_difference(images["reveal-on-%u" % frame],
                                                 images["reveal-off-%u" % frame], threshold=5)
               for frame in REVEAL_FRAMES} if not args.static_only else {}
    # Do not erase history artifacts by resetting the culler in this consumer.
    # Every sampled reveal frame, including the first, must match the control.
    if history:
        validate_history(history)
    visible_cells = cell_changes(images["empty"], images["off"], args.count)[1::2]
    if any(value <= 0 for value in visible_cells):
        raise ValueError("a known visible object has no independent cell coverage")
    reveal_cells = {}
    if not args.static_only:
        coverage = cell_changes(images["empty"], images["reveal-off-90"], args.count)
        if any(value <= 0 for value in coverage):
            raise ValueError("a revealed object has no independent cell coverage")
        for frame in REVEAL_FRAMES:
            changes = cell_changes(images["reveal-on-%u" % frame], images["reveal-off-%u" % frame], args.count)
            if any(changes):
                raise ValueError("a known object differs at a sampled reveal frame")
            reveal_cells[str(frame)] = changes
    runs = []
    if not args.witness_only:
        for index in range(args.repeats):
            for mode in (("off", "on") if index % 2 == 0 else ("on", "off")):
                target = root / ("run-%02u-%s" % (index, mode))
                target.mkdir()
                env = environment(scene, args.backend, mode, args.count)
                env.pop("JCE_BENCH_SHOT_FRAME")
                env.update(host_render_environment("player", target))
                (target / "intent.json").write_text(json.dumps({"environment": env,
                    "source": source, "executable_sha256": exe_hash, "warmup": 120,
                    "samples": args.samples}, indent=2) + "\n", encoding="utf-8", newline="\n")
                process = run(env, target / "engine.log", frames=121 + args.samples,
                              timeout=1800, exe=args.exe.resolve(), samples_path=target / "frames.csv",
                              expected_workload=(3, args.count))
                counters = validate_counts((target / "engine.log").read_text(encoding="utf-8"),
                                           args.count, mode == "on", False)
                summary = frame_summary(read_frames(target / "frames.csv", warmup=120,
                                                     sample_count=args.samples))
                runs.append({"status": "PASS", "index": index, "mode": mode, "directory": str(target),
                             "frame": summary, "process": process, "counters": counters,
                             "raw_sha256": sha256_file(target / "frames.csv")})
                print(json.dumps({"mode": mode, "index": index,
                                  "median_ms": summary["frame_median_ms"]}), flush=True)
    result = {"status": "STATIC_PREFLIGHT" if args.static_only else
              ("WITNESS_PASS" if args.witness_only else "PASS"), "count": args.count,
              "source": source, "executable_sha256": exe_hash, "scene_sha256": sha256_file(scene),
              "scope": "SDK Player hardware-query occlusion; fixed wall, known populations; query budget retained; not HiZ/GPUScene",
              "comparison_name": "hardware queries", "history_rgb_threshold": 5,
              "differences": differences, "reveal_history_difference": history,
              "visible_cell_coverage": visible_cells, "reveal_cell_difference": reveal_cells,
              "captures": captures, "runs": runs,
              "median_frame_ms": {mode: statistics.median(row["frame"]["frame_median_ms"]
                  for row in runs if row["mode"] == mode) for mode in ("off", "on")} if runs else {}}
    output = root / "suite.json"
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(output), "status": result["status"],
                      "differences": differences, "history": history, "median": result["median_frame_ms"]}))


if __name__ == "__main__":
    main()
