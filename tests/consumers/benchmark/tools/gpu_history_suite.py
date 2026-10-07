"""Imported-model HiZ wall reveal; history correctness precedes timings."""
import argparse
import json
import math
from pathlib import Path
import re
import uuid

from gpu_model_suite import (fixture, full_pixel_difference, player_counters, read_png,
                             run_visual_witness, sha256_file, source_identity,
                             validate_gpu, validate_model)
from occlusion_suite import REVEAL_FRAMES, cell_changes, validate_history
from check_asset_pixels import pack_fixture
from gpu_counter_evidence import read_survivors

MOTION_FRAMES = (51, 66, 79, 80, 81, 82, 90)
MOTION_COUNTER_FRAMES = (20, 50, *MOTION_FRAMES)


def expected_survivors(count, frame, reveal, motion=False):
    """The wall hides the left half until its frame-80 removal."""
    if motion:
        return count if 51 <= frame <= 81 else count // 2
    return count if reveal and frame >= 80 else count // 2


def validate_camera_motion(log):
    """Observe all thirty SDK camera updates, restoration and settled pose."""
    number = r"(-?\d+\.\d+)"
    marker = r"benchmark-camera: frame=(\d+) offset=" + number + r" yaw=" + number
    marker += r" pos=" + ",".join([number] * 3) + r" forward=" + ",".join([number] * 3)
    rows = re.findall(marker, log)
    if [int(row[0]) for row in rows] != [20, *range(50, 83), 90]:
        raise ValueError("continuous camera observations missing, duplicated or out of order")
    for row in rows:
        frame = int(row[0])
        wave = math.sin((frame - 50) * math.pi / 32) if 51 <= frame <= 80 else 0
        offset, yaw = .2 * wave, .03 * wave
        expected = (offset, yaw, offset, 0, 10, math.sin(yaw), 0, -math.cos(yaw))
        if any(not math.isfinite(float(actual)) or abs(float(actual) - wanted) > 1e-6
               for actual, wanted in zip(row[1:], expected)):
            raise ValueError("actual camera pose differs from independent continuous route")
    return rows


def validate_occluder_motion(log):
    number = r"(-?\d+\.\d+)"
    marker = r"benchmark-occluder: frame=(\d+) offset=" + number
    marker += r" pos=" + ",".join([number] * 3)
    rows = re.findall(marker, log)
    if [int(row[0]) for row in rows] != [20, *range(50, 83), 90]:
        raise ValueError("continuous occluder observations missing, duplicated or out of order")
    for row in rows:
        frame = int(row[0])
        wave = math.sin((frame - 50) * math.pi / 32) if 51 <= frame <= 80 else 0
        offset = 1.8 * wave
        if any(not math.isfinite(float(actual)) or abs(float(actual) - wanted) > 1e-6
               for actual, wanted in zip(row[1:], (offset, -1.67 + offset, 0, 0))):
            raise ValueError("actual occluder pose differs from independent continuous route")
    return rows


def validate_motion(log, motion, has_models):
    """The empty control has a camera but never creates the fixture wall."""
    if motion == "camera":
        return validate_camera_motion(log)
    if motion != "occluder":
        raise ValueError("unknown motion witness")
    if has_models:
        return validate_occluder_motion(log)
    if "benchmark-occluder:" in log:
        raise ValueError("empty control unexpectedly contains a moving occluder")
    return []


def peak_revealed_cells(empty, peak, count):
    """Require newly exposed left models outside the independently projected wall."""
    width, height = empty[:2]
    differences = cell_changes(empty, peak, count)
    side = math.ceil(math.sqrt((count + 1) // 2))
    focal = height / (2 * math.tan(math.radians(30)))
    step = 1.8 * focal / 15 / max(side - 1, 1)
    wall_x = -1.67 + 1.8
    wall_left = width / 2 + (wall_x - 1.4) * focal / (10 - .15)
    cells = []
    for index in range(0, count, 2):
        slot = index // 2
        x = -3.4 + 1.8 * (slot % side) / (side - 1) if side > 1 else -2.5
        right = math.ceil(width / 2 + x * focal / 15 + step * .4)
        if right < wall_left - 2:
            cells.append({"index": index, "changed_pixels": differences[index]})
    if not cells or any(cell["changed_pixels"] <= 0 for cell in cells):
        raise ValueError("a newly exposed left model lacks independent peak pixel coverage")
    return cells


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--packer", type=Path, required=True)
    parser.add_argument("--asset", type=Path, required=True)
    parser.add_argument("--vertices", type=int, required=True)
    parser.add_argument("--count", type=int, default=128)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "gl", "vulkan"), default="d3d11")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--survivors", action="store_true",
                        help="require actual compacted counts before and after wall removal")
    parser.add_argument("--motion", choices=("camera", "occluder"),
                        help="continuous translation/yaw at frames 51..80, restoration at 81")
    args = parser.parse_args()
    history_frames = MOTION_FRAMES if args.motion else REVEAL_FRAMES
    if any(not path.is_file() for path in (args.exe, args.packer, args.asset)) or \
            args.asset.suffix.lower() != ".glb" or not 2 <= args.count <= 512 or args.vertices <= 0:
        parser.error("existing SDK tools/GLB, positive topology and count 2..512 required")
    root = args.out.resolve() / uuid.uuid4().hex[:12]
    root.mkdir(parents=True, exist_ok=False)
    pak, scene = pack_fixture(args.packer, args.asset, root), root / "scene.json"
    scene.write_text(json.dumps(fixture(args.count), indent=2) + "\n", encoding="utf-8", newline="\n")
    intent = {"source": source_identity(), "executable_sha256": sha256_file(args.exe),
              "asset_sha256": sha256_file(args.asset), "pak_sha256": sha256_file(pak),
              "packer_sha256": sha256_file(args.packer),
              "scene_sha256": sha256_file(scene), "count": args.count, "vertices": args.vertices,
              "backend": args.backend, "motion": args.motion,
              "scope": "HiZ imported-model %s motion" % args.motion if args.motion else "HiZ imported-model wall/reveal, hardware queries disabled"}
    (root / "intent.json").write_text(json.dumps(intent, indent=2) + "\n", encoding="utf-8", newline="\n")
    cases = [("empty", "cpu", 50, False), ("cpu", "cpu", 50, False),
             ("hiz", "hiz", 50, False), ("repeat", "hiz", 50, False)]
    cases += [("reveal-%s-%u" % (mode, frame), mode, frame, True)
              for frame in history_frames for mode in ("cpu", "hiz")]
    captures, images, counters, counter_errors = {}, {}, {}, {}
    for label, mode, frame, reveal in cases:
        target = root / label
        env = {"JCE_SCENE": str(scene), "JCE_BACKEND": args.backend,
               "JCE_BENCH_ASSET_STILL": "1", "JCE_BENCH_TEXT_CAPTURE": "1",
               "JCE_BENCH_SHOT_FRAME": str(frame), "JCE_BENCH_ASSET_PAK": str(pak),
               "JCE_GPU_SCENE": "0" if mode == "cpu" else "force",
               "JCE_HIZ_OCCLUSION": "1" if mode == "hiz" else "0"}
        if label != "empty":
            env.update(JCE_BENCH_AUTOSPAWN="3,%u" % args.count, JCE_BENCH_OCCLUSION="off",
                       JCE_BENCH_OCCLUSION_MODEL=args.asset.name, JCE_BENCH_ASSET_PATH=args.asset.name)
        if args.motion:
            env["JCE_BENCH_CAMERA_MOTION" if args.motion == "camera" else
                "JCE_BENCH_OCCLUSION_MOTION"] = "oscillate"
        if reveal and not args.motion:
            env["JCE_BENCH_OCCLUSION_REVEAL"] = "1"
        if args.survivors and mode == "hiz":
            env["JCE_BENCH_GPU_SURVIVORS"] = str(target / "survivors.jsonl")
        process = run_visual_witness(env, target, exe=args.exe.resolve(), timeout=1800,
            expected_workload=(3, args.count) if label != "empty" else None, host="player")
        log = (target / "visual.log").read_text(encoding="utf-8")
        resources = player_counters((target / "engine_counters.jsonl").read_text(encoding="utf-8"))
        if label != "empty":
            validate_model(log, args.count, args.vertices, wall=True, reveal=reveal and not args.motion)
            validate_gpu(resources, args.count, mode, extra_entities=4)
        if args.motion:
            validate_motion(log, args.motion, label != "empty")
        images[label] = read_png(target / "frame.png")
        captures[label] = {"process": process, "resources": resources, "environment": env,
                           "image": str(target / "frame.png"), "image_sha256": sha256_file(target / "frame.png")}
        if "JCE_BENCH_GPU_SURVIVORS" in env:
            try:
                counters[label] = read_survivors(target / "survivors.jsonl", args.count,
                    lambda source_frame: expected_survivors(args.count, source_frame, reveal, bool(args.motion)),
                    frames=MOTION_COUNTER_FRAMES if args.motion else (20, 50, *REVEAL_FRAMES))
            except (ValueError, OSError, KeyError) as error:
                counter_errors[label] = str(error)
        print(json.dumps({"capture": label, "gpu": resources[-1].get("model_gpu_driven")}), flush=True)
    static = {"coverage": full_pixel_difference(images["empty"], images["cpu"]),
              "cpu_hiz": full_pixel_difference(images["cpu"], images["hiz"], threshold=5),
              "repeat_noise": full_pixel_difference(images["hiz"], images["repeat"], threshold=5)}
    history = {str(frame): full_pixel_difference(images["reveal-cpu-%u" % frame],
               images["reveal-hiz-%u" % frame], threshold=5) for frame in history_frames}
    cells = {str(frame): cell_changes(images["reveal-cpu-%u" % frame],
                                     images["reveal-hiz-%u" % frame], args.count) for frame in history_frames}
    coverage = cell_changes(images["empty"], images["reveal-cpu-90"], args.count)
    motion_change = full_pixel_difference(images["cpu"], images["reveal-cpu-66" if args.motion else "reveal-cpu-79"], threshold=5)
    # Preserve a failed GPU artifact and its measurements before rejecting it.
    result = dict(intent, status="FAILED", captures=captures, static=static,
                  history=history, cell_changes=cells, revealed_cell_coverage=coverage,
                  gpu_survivors=counters, counter_errors=counter_errors,
                  motion_pixel_change=motion_change,
                  limitations=["no timing" if args.survivors else "no timing or independently read GPU survivor count",
                               "%u sampled history frames, not exhaustive motion acceptance" % len(history_frames)])
    output = root / "suite.json"
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    if counter_errors:
        raise ValueError("actual HiZ survivor counts failed: " + json.dumps(counter_errors))
    if args.motion == "occluder":
        result["peak_revealed_cells"] = peak_revealed_cells(images["empty"], images["reveal-cpu-66"], args.count)
    validate_history(history, history_frames)
    if args.motion and motion_change <= .0001:
        raise ValueError("continuous SDK motion route did not change the actual image")
    if static["coverage"] <= 0 or static["cpu_hiz"] >= 0.0001 or static["repeat_noise"] >= 0.0001 \
            or any(value <= 0 for value in coverage) or any(any(values) for values in cells.values()):
        raise ValueError("HiZ static/reveal geometry has missing or changed per-object pixels")
    result["status"] = args.motion.upper() + "_MOTION_WITNESS_PASS" if args.motion else "REVEAL_WITNESS_PASS"
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(output), "status": result["status"], "history": history}))


if __name__ == "__main__":
    main()
