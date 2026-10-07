"""Canonical static-model GPUScene/HiZ controls through the installed SDK."""
import argparse
import json
import math
import re
from pathlib import Path
import sys
import uuid

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from check_asset_pixels import (MARKER, full_pixel_difference, image_pair_size,
                                pack_fixture, validate_still_controls)
from culling_suite import fixture, validate_counts
from tools.benchmark_suite import player_counters, run_visual_witness, sha256_file, source_identity
from tools.jce_determinism import read_png
from gpu_counter_evidence import read_survivors


def validate_model(log, count, vertices, wall=False, reveal=False):
    validate_still_controls(log)
    rows = MARKER.findall(log)
    if len(rows) != 2 or [int(row[0]) for row in rows] != [20, 50]:
        raise ValueError("static imported-model observations missing or duplicated")
    for row in rows:
        if tuple(map(int, row[1:8])) != (count, 0, vertices, 1, 0, 0, 0) or \
                float(row[8]) != 0 or int(row[9]) <= 0:
            raise ValueError("actual static model population/topology differs")
    if wall:
        from occlusion_suite import validate_counts as validate_wall
        validate_wall(log, count, False, reveal)
    else:
        validate_counts(log, count, False)


def validate_gpu(rows, count, mode, extra_entities=3):
    samples = rows[1:]
    if not samples or samples[-1]["event"] != "exit":
        raise ValueError("complete GPU resource observations required")
    for row in samples:
        gpu = row["model_gpu_driven"]
        if row["engine"]["entity_count"] != count + extra_entities:
            raise ValueError("the actual static model scene population changed")
        if mode == "cpu":
            if gpu["enabled"] or gpu["active"] or gpu["compute_dispatches"]:
                raise ValueError("CPU control accidentally used GPUScene")
            continue
        if not gpu["supported"]:
            raise ValueError("GPUScene device/program path UNAVAILABLE; not a PASS")
        if not all(gpu[key] for key in ("enabled", "active", "forced", "dispatch_succeeded")) or \
                gpu["candidate_records"] != count or gpu["input_records"] != count or \
                gpu["compute_dispatches"] < 1 or gpu["upload_calls"] < 1 or \
                gpu["uploaded_bytes"] <= 0 or gpu["cpu_fallback_submits"]:
            raise ValueError("requested GPU model path did not execute completely")
        if gpu["hiz_enabled"] != (mode == "hiz"):
            raise ValueError("actual HiZ state differs from the requested control")
        if mode == "fixed":
            if gpu["indirect_ready"] or gpu["indirect_submits"] or gpu["fixed_count_submits"] < 1 \
                    or gpu["compute_dispatches"] != 1:
                raise ValueError("explicit fixed-count compute ablation did not execute")
        elif gpu["indirect_supported"] and (not gpu["indirect_ready"] or gpu["indirect_submits"] < 1):
            raise ValueError("supported indirect path fell back instead of executing")
        if not gpu["indirect_supported"] and gpu["fixed_count_submits"] < 1:
            raise ValueError("neither indirect nor declared fixed-count fallback drew")
    return samples


def validate_policy(log, mode):
    values = re.findall(r"benchmark-asset: indirect policy=(\d+)", log)
    if values != ["0" if mode == "fixed" else "1"]:
        raise ValueError("actual public indirect policy differs from the control")


def front_cells(first, second, count):
    """Independent projection of the fixture's front group, not engine bounds."""
    width, height = image_pair_size(first, second)
    front = (count + 2) // 3
    side = math.ceil(math.sqrt(front))
    focal = height / (2 * math.tan(math.radians(30))) / 15
    step_x = 8 * focal / max(side - 1, 1)
    step_y = 4 * focal / max(side - 1, 1)
    if min(step_x, step_y) < 2:
        raise ValueError("front cells are too dense for independent pixel coverage")
    ca, cb, a, b = first[2], second[2], first[3], second[3]
    result = []
    for slot in range(front):
        x = -4 + 8 * (slot % side) / (side - 1) if side > 1 else 0
        y = -2 + 4 * (slot // side) / (side - 1) if side > 1 else 0
        cx, cy = width / 2 + x * focal, height / 2 - y * focal
        box = (math.floor(cx - step_x * .35), math.floor(cy - step_y * .35),
               math.ceil(cx + step_x * .35), math.ceil(cy + step_y * .35))
        if not 0 <= box[0] < box[2] <= width or not 0 <= box[1] < box[3] <= height:
            raise ValueError("a front cell leaves the viewport")
        result.append(sum(max(abs(a[(py * width + px) * ca + channel] -
                                  b[(py * width + px) * cb + channel]) for channel in range(3)) > 5
                          for py in range(box[1], box[3]) for px in range(box[0], box[2])))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--packer", type=Path, required=True)
    parser.add_argument("--asset", type=Path, required=True)
    parser.add_argument("--vertices", type=int, required=True)
    parser.add_argument("--count", type=int, default=6000)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "gl", "vulkan"), default="d3d11")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--survivors", action="store_true",
                        help="require actual async compacted GPU counts, outside timing")
    args = parser.parse_args()
    if any(not path.is_file() for path in (args.exe, args.packer, args.asset)) or \
            args.asset.suffix.lower() != ".glb" or not 3 <= args.count <= 200000 or args.vertices <= 0:
        parser.error("existing SDK tools/GLB and positive topology/count required")
    root = args.out.resolve() / uuid.uuid4().hex[:12]
    root.mkdir(parents=True, exist_ok=False)
    pak = pack_fixture(args.packer, args.asset, root)
    scene = root / "scene.json"
    scene.write_text(json.dumps(fixture(args.count), indent=2) + "\n", encoding="utf-8", newline="\n")
    intent = {"source": source_identity(), "executable_sha256": sha256_file(args.exe),
              "asset_sha256": sha256_file(args.asset), "pak_sha256": sha256_file(pak),
              "packer_sha256": sha256_file(args.packer),
              "scene_sha256": sha256_file(scene), "count": args.count, "vertices": args.vertices,
              "asset_name": args.asset.name, "backend": args.backend,
              "scope": "static GLB input groups; compute/indirect/HiZ activity and pixels" +
                       ("; actual asynchronous GPU survivors" if args.survivors else
                        "; no GPU survivor readback")}
    (root / "intent.json").write_text(json.dumps(intent, indent=2) + "\n", encoding="utf-8", newline="\n")
    captures, images, counters = {}, {}, {}
    for label, mode, frame in (("empty", "cpu", 50), ("cpu", "cpu", 50),
                               ("fixed", "fixed", 50), ("gpu", "gpu", 50),
                               ("hiz", "hiz", 50), ("repeat", "hiz", 50)):
        target = root / label
        env = {"JCE_SCENE": str(scene), "JCE_BACKEND": args.backend,
               "JCE_BENCH_ASSET_STILL": "1", "JCE_BENCH_TEXT_CAPTURE": "1",
               "JCE_BENCH_SHOT_FRAME": str(frame), "JCE_BENCH_ASSET_PAK": str(pak),
               "JCE_GPU_SCENE": "0" if mode == "cpu" else "force",
               "JCE_BENCH_GPU_INDIRECT": "0" if mode == "fixed" else "1",
               "JCE_HIZ_OCCLUSION": "1" if mode == "hiz" else "0"}
        if label != "empty":
            env.update(JCE_BENCH_AUTOSPAWN="3,%u" % args.count, JCE_BENCH_CULLING="off",
                       JCE_BENCH_CULLING_MODEL=args.asset.name, JCE_BENCH_ASSET_PATH=args.asset.name)
        if args.survivors and mode in ("gpu", "hiz"):
            env["JCE_BENCH_GPU_SURVIVORS"] = str(target / "survivors.jsonl")
        process = run_visual_witness(env, target, exe=args.exe.resolve(), timeout=1800,
                                     expected_workload=(3, args.count) if label != "empty" else None, host="player")
        log = (target / "visual.log").read_text(encoding="utf-8")
        resources = player_counters((target / "engine_counters.jsonl").read_text(encoding="utf-8"))
        if label != "empty":
            validate_model(log, args.count, args.vertices)
            validate_policy(log, mode)
            validate_gpu(resources, args.count, mode)
        captures[label] = {"process": process, "resources": resources, "image": str(target / "frame.png"),
                           "image_sha256": sha256_file(target / "frame.png"), "environment": env}
        images[label] = read_png(target / "frame.png")
        if "JCE_BENCH_GPU_SURVIVORS" in env:
            counters[label] = read_survivors(target / "survivors.jsonl", args.count,
                                            (args.count + 2) // 3)
        print(json.dumps({"capture": label, "gpu": resources[-1].get("model_gpu_driven")}), flush=True)
    differences = {"coverage": full_pixel_difference(images["empty"], images["cpu"]),
                   "cpu_fixed": full_pixel_difference(images["cpu"], images["fixed"]),
                   "cpu_gpu": full_pixel_difference(images["cpu"], images["gpu"]),
                   "cpu_hiz": full_pixel_difference(images["cpu"], images["hiz"]),
                   "repeat_noise": full_pixel_difference(images["hiz"], images["repeat"])}
    cells = {"coverage": front_cells(images["empty"], images["cpu"], args.count)}
    cells.update({mode: front_cells(images["cpu"], images[mode], args.count)
                  for mode in ("fixed", "gpu", "hiz", "repeat")})
    if any(value <= 0 for value in cells["coverage"]) or any(
            any(cells[mode]) for mode in ("fixed", "gpu", "hiz", "repeat")):
        raise ValueError("a known front object is missing or differs in the GPU path")
    if differences["coverage"] <= 0 or any(differences[key] >= 0.0001
            for key in ("cpu_fixed", "cpu_gpu", "cpu_hiz", "repeat_noise")):
        raise ValueError("GPU-driven pixels are empty, mismatched or unstable")
    result = dict(intent, status="STATIC_WITNESS_PASS", captures=captures, differences=differences, cells=cells,
                  gpu_survivors=counters,
                  limitations=["no moving-occluder/camera history acceptance yet", "no timings",
                               "actual survivor readback is diagnostic, excluded from timing" if args.survivors else
                               "input records are not independently read GPU survivors"])
    output = root / "suite.json"
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(output), "status": result["status"], "differences": differences}))


if __name__ == "__main__":
    main()
