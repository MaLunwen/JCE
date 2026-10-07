#!/usr/bin/env python3
"""Export independently validated shadow liveness evidence, not R10 timing."""
import argparse
import json
import re
from pathlib import Path

import numpy as np
from PIL import Image

from report_lights import BACKENDS, checked
from shadow_suite import CASES, image, mask, primitive_environment, scene, validate, write
from tools.jce_determinism import assert_backend


def load(path):
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("status") != "SHADOW_PIXEL_LIVENESS_PASS":
        raise ValueError("shadow suite did not pass")
    if value.get("backend") not in BACKENDS or set(value.get("cases", {})) != set(CASES):
        raise ValueError("unsupported backend or incomplete seven-case controls")
    checked(Path(value["executable"]), value["executable_sha256"])
    controls = value["controls"]
    if set(controls) != {"camera", "distinct_casters", "primitive_instancing",
                         "diagnostics", "sun_direction", "receiver_tilt_deg"}:
        raise ValueError("unsupported fixture configuration")
    pixels = {}
    for label in CASES:
        case = value["cases"][label]
        files = case["files"]
        if not {"scene.json", "engine.log", "target.png"} <= set(files) or \
                set(files) - {"scene.json", "engine.log", "target.png", "csm_uniforms.log"}:
            raise ValueError("missing or unsupported raw evidence")
        for entry in files.values():
            checked(Path(entry["path"]), entry["sha256"])
        authored = scene(label, camera_mode=controls["camera"],
            distinct_casters=controls["distinct_casters"],
            sun_direction=controls["sun_direction"],
            receiver_tilt_deg=controls["receiver_tilt_deg"])
        retained = json.loads(Path(files["scene.json"]["path"]).read_text(encoding="utf-8"))
        if retained != authored or case["camera"] != authored["entities"][0]["components"]:
            raise ValueError("raw scene differs from regenerated fixture")
        process = case["process"]
        if process["process"].get("returncode") != 0 or process["process"].get("timed_out"):
            raise ValueError("consumer failed")
        log = Path(files["engine.log"]["path"]).read_text(encoding="utf-8")
        if assert_backend(log, value["backend"]) != process.get("backend_actual") or "] ERROR -" in log:
            raise ValueError("actual backend or renderer error")
        env = case["environment"]
        expected = {"JCE_BACKEND": value["backend"], "JCE_GPU_TIER": "high",
            "JCE_SCENE": files["scene.json"]["path"], "JCE_BENCH_ASSET_STILL": "1",
            "JCE_BENCH_SHOT_FRAME": "50", "JCE_BENCH_PLAYER_SHOT": files["target.png"]["path"]}
        expected.update(primitive_environment(controls["primitive_instancing"]))
        if controls["diagnostics"]:
            expected.update(JCE_DBG_CSM_STATS="1", JCE_DBG_SHPROF="1",
                JCE_DISABLE_SHADOWCACHE="1", JCE_BENCH_SHADOW_TARGETS="1",
                JCE_DBG_CSM_LOG=str(Path(files["scene.json"]["path"]).parent / "csm_uniforms.log"))
        if env != expected:
            raise ValueError("capture configuration differs")
        targets = []
        if controls["diagnostics"]:
            counts = re.findall(r"benchmark-shadow-targets: frame=50 count=(\d+)", log)
            raw = re.findall(r"benchmark-shadow-target: frame=50 index=(\d+) name=(\S+) width=(\d+) height=(\d+) texture=(\d+) kind=(\d+)", log)
            if len(counts) != 1 or len(raw) != int(counts[0]) or \
                    [int(row[0]) for row in raw] != list(range(len(raw))):
                raise ValueError("incomplete actual shadow target inventory")
            targets = [{"index": int(i), "name": name, "width": int(w),
                "height": int(h), "texture": int(texture), "kind": int(kind)}
                for i, name, w, h, texture, kind in raw]
        if case.get("targets", []) != targets:
            raise ValueError("stored actual shadow targets differ from raw log")
        pixels[label] = image(Path(files["target.png"]["path"]))
    metrics = validate(pixels)
    if metrics != value.get("metrics"):
        raise ValueError("stored metrics differ from recomputed pixels")
    return value, metrics, pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    loaded = [load(path) for path in args.suite]
    args.out.mkdir(parents=True, exist_ok=False)
    rows = []
    lines = ["# Shadow pixel evidence", "",
        "Correctness controls only: not R10 performance, independent shadow precision or complete acceptance.",
        "Different configurations and executable hashes are separate evidence identities.", "",
        "| Evidence | Backend | Binary SHA-256 | Configuration | Shadow delta | Movement outside geometry |",
        "|---|---|---|---|---:|---:|"]
    for index, (value, metrics, pixels) in enumerate(loaded):
        changed = mask(pixels["on"], pixels["off"])
        movement = mask(pixels["on"], pixels["moved-on"]) & ~mask(pixels["off"], pixels["moved-off"])
        rgb = np.zeros((*changed.shape, 3), dtype=np.uint8)
        rgb[changed] = (255, 180, 0)
        rgb[movement] = (0, 220, 255)
        name = "changed-%02u.png" % index
        Image.fromarray(rgb).save(args.out / name)
        config = json.dumps(value["controls"], sort_keys=True)
        lines.append("| %u | %s | %s | %s | %.8f | %.8f |" % (index,
            value["backend"], value["executable_sha256"], config,
            metrics["shadow_delta"], metrics["shadow_movement_outside_geometry"]))
        rows.append({"suite": str(args.suite[index].resolve()),
            "suite_sha256": __import__("gbuffer_asset_suite").sha256_file(args.suite[index]),
            "backend": value["backend"], "executable_sha256": value["executable_sha256"],
            "controls": value["controls"], "metrics": metrics, "visual": name,
            "actual_targets": {label: value["cases"][label].get("targets", []) for label in CASES}})
    lines.extend(["", "Amber: shadow on/off changes. Cyan: movement outside unshadowed geometry.", ""])
    for row in rows:
        lines.append("![Changed pixels](%s)" % row["visual"])
    (args.out / "REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    write(args.out / "recomputed.json", {"scope": "shadow pixel liveness only", "evidence": rows})


if __name__ == "__main__":
    main()
