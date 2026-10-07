"""Unchanged complex static GLB liveness with asynchronous readiness controls."""
import argparse
import json
import math
from pathlib import Path
import re
import struct
import uuid

from check_asset_pixels import (MARKER, full_pixel_difference, pack_fixture,
                                read_png, transform, validate_still_controls)
from tools.benchmark_suite import player_counters, run_visual_witness, sha256_file, source_identity

READY = re.compile(r"benchmark-asset: ready elapsed_ms=(\d+) frame=(\d+)")


def glb_census(content):
    if len(content) < 20:
        raise ValueError("truncated GLB header")
    magic, version, length, json_length, json_type = struct.unpack_from("<IIIII", content)
    if (magic, version, length, json_type) != (0x46546c67, 2, len(content), 0x4e4f534a) or \
            json_length % 4 or 20 + json_length > len(content):
        raise ValueError("invalid GLB header/JSON chunk")
    doc = json.loads(content[20:20 + json_length])
    if doc.get("skins") or doc.get("animations") or doc.get("extensionsRequired"):
        raise ValueError("this base static witness excludes rigs and required extensions")
    primitives = [p for node in doc["nodes"] if "mesh" in node
                  for p in doc["meshes"][node["mesh"]]["primitives"]]
    counts = [doc["accessors"][p["attributes"]["POSITION"]]["count"] for p in primitives]
    if not counts or any(type(count) is not int or count <= 0 for count in counts):
        raise ValueError("positive position accessor counts required")
    return {"vertices": sum(counts), "primitives": len(primitives),
            "nodes": len(doc["nodes"]), "materials": len(doc.get("materials", [])),
            "images": len(doc.get("images", [])), "extensions_used": doc.get("extensionsUsed", [])}


def fixture(asset, scale, camera, pitch, present):
    pose = transform(*camera)
    pose["rotX"] = pitch
    entities = [{"id": 1, "name": "Camera", "components": [pose,
                 {"type": "Camera", "primary": True, "fov": 40, "nearClip": .01,
                  "farClip": 100, "clearMode": 1}]},
                {"id": 2, "name": "Sun", "components": [transform(),
                 {"type": "DirectionalLight", "dirX": -.5, "dirY": -1,
                  "dirZ": -.5, "intensity": 2}]}]
    if present:
        entities.append({"id": 3, "name": "StaticModel", "components": [transform(scale=scale),
                         {"type": "MeshRenderer", "meshPath": asset, "visible": True}]})
    return {"entities": entities}


def validate_import(log, census, present):
    validate_still_controls(log)
    ready = READY.findall(log)
    rows = MARKER.findall(log)
    if not present:
        if ready or rows:
            raise ValueError("empty control unexpectedly imported the model")
        return []
    if len(ready) != 1 or int(ready[0][1]) != 0 or not 0 <= int(ready[0][0]) < 60000:
        raise ValueError("one actual async readiness marker at fixture frame zero required")
    if len(rows) != 2 or [int(row[0]) for row in rows] != [20, 50]:
        raise ValueError("actual imported topology samples missing or duplicated")
    for row in rows:
        if tuple(map(int, row[1:8])) != (1, 0, census["vertices"], census["primitives"], 0, 0, 0) or \
                float(row[8]) != 0 or int(row[9]) <= 0:
            raise ValueError("actual static topology differs from original GLB census")
    return rows


def validate_resources(rows, present):
    for row in rows:
        if row["engine"]["entity_count"] != (3 if present else 2):
            raise ValueError("actual static scene population differs")
    for row in rows[1:]:
        gpu = row["model_gpu_driven"]
        if gpu["enabled"] or gpu["active"] or gpu["compute_dispatches"]:
            raise ValueError("the base CPU submission control unexpectedly used GPUScene")


def pixels_pass(deltas):
    if set(deltas) != {"coverage", "frame20_50", "repeat"} or \
            any(not math.isfinite(value) or not 0 <= value <= 1 for value in deltas.values()):
        return False
    return deltas["coverage"] > .002 and deltas["frame20_50"] < .0001 and deltas["repeat"] < .0001


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("exe", "asset", "packer", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "vulkan", "gl"), required=True)
    parser.add_argument("--camera", type=float, nargs=3, required=True, metavar=("X", "Y", "Z"))
    parser.add_argument("--pitch", type=float, required=True)
    parser.add_argument("--scale", type=float, default=1)
    args = parser.parse_args()
    if not all(path.is_file() for path in (args.exe, args.asset, args.packer)) or \
            args.asset.suffix.lower() != ".glb" or args.scale <= 0 or \
            not all(math.isfinite(v) for v in (*args.camera, args.pitch, args.scale)):
        parser.error("existing inputs, GLB model and finite positive scale/framing required")
    root = args.out.resolve() / uuid.uuid4().hex[:12]
    root.mkdir(parents=True, exist_ok=False)
    census = glb_census(args.asset.read_bytes())
    pak = pack_fixture(args.packer, args.asset, root)
    hashes = {name + "_sha256": sha256_file(path) for name, path in
              (("executable", args.exe), ("asset", args.asset), ("packer", args.packer), ("pak", pak))}
    result = {"status": "INCOMPLETE", "source": source_identity(), **hashes, "backend": args.backend,
              "census": census, "captures": {}, "rgb_threshold": 5,
              "scope": "base static imported liveness; async readiness, CPU submission, no timing",
              "limitations": ["optional glTF extensions and independent material fidelity are not certified",
                              "topology counts do not prove every occluded primitive is visible",
                              "modern device evidence is not a physical 512 MiB device test"]}
    output = root / "suite.json"
    images = {}
    try:
        for label, present, frame in (("empty", False, 50), ("base20", True, 20),
                                      ("base50", True, 50), ("repeat50", True, 50)):
            target = root / label
            target.mkdir()
            scene = target / "scene.json"
            scene.write_text(json.dumps(fixture(args.asset.name, args.scale, args.camera, args.pitch, present),
                                        indent=2) + "\n", encoding="utf-8", newline="\n")
            env = {"JCE_SCENE": str(scene), "JCE_BACKEND": args.backend,
                   "JCE_BENCH_ASSET_PAK": str(pak), "JCE_BENCH_ASSET_STILL": "1",
                   "JCE_BENCH_TEXT_CAPTURE": "1", "JCE_BENCH_SHOT_FRAME": str(frame),
                   "JCE_GPU_SCENE": "0", "JCE_HIZ_OCCLUSION": "0"}
            if present:
                env.update(JCE_BENCH_ASSET_PATH=args.asset.name, JCE_BENCH_WAIT_ASSET="1",
                           JCE_MAX_FRAMES="100000", JCE_MAX_SECONDS="75")
            process = run_visual_witness(env, target, exe=args.exe.resolve(), timeout=120, host="player")
            log = (target / "visual.log").read_text(encoding="utf-8")
            rows = validate_import(log, census, present)
            resources = player_counters((target / "engine_counters.jsonl").read_text(encoding="utf-8"))
            validate_resources(resources, present)
            images[label] = read_png(target / "frame.png")
            result["captures"][label] = {"process": process, "environment": env, "topology": rows,
                "resources": resources, "image_sha256": sha256_file(target / "frame.png"),
                "scene_sha256": sha256_file(scene), "log_sha256": sha256_file(target / "visual.log")}
            print(json.dumps({"capture": label, "topology": rows}), flush=True)
        deltas = {"coverage": full_pixel_difference(images["empty"], images["base50"], threshold=5),
                  "frame20_50": full_pixel_difference(images["base20"], images["base50"], threshold=5),
                  "repeat": full_pixel_difference(images["base50"], images["repeat50"], threshold=5)}
        result["deltas"] = deltas
        if not pixels_pass(deltas):
            raise ValueError("static model coverage/temporal/repeat controls failed")
        for name, path in (("executable", args.exe), ("asset", args.asset), ("packer", args.packer), ("pak", pak)):
            if sha256_file(path) != hashes[name + "_sha256"]:
                raise ValueError("a source artifact changed during the static suite")
        result["status"] = "STATIC_ASSET_LIVENESS_PASS"
    except Exception as error:
        result.update(status="FAILED", error=str(error))
        raise
    finally:
        output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"suite": str(output), "status": result["status"], "deltas": deltas}), flush=True)


if __name__ == "__main__":
    main()
