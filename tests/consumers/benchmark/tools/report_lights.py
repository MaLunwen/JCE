#!/usr/bin/env python3
"""Rehash and revalidate original light-patch evidence before visual export."""
import argparse
import json
from pathlib import Path

import numpy as np

from light_suite import (COUNT, HEIGHT, ORTHO_HEIGHT, WIDTH, case_specs, image,
                         roi, scene, sha256_file, validate, write)
from tools.jce_determinism import assert_backend

BACKENDS = ("d3d11", "d3d12", "vulkan", "gl")
KINDS = ("point", "spot")


def checked(path, expected):
    if sha256_file(path) != expected:
        raise ValueError("raw input changed: " + str(path))


def load(path):
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("status") != "LIGHT_PATCH_CONTROLS_PASS":
        raise ValueError("light suite did not pass")
    if value.get("backend") not in BACKENDS:
        raise ValueError("unsupported Windows backend")
    checked(Path(value["executable"]), value["executable_sha256"])
    expected = {kind + "/" + row[0] for kind in KINDS for row in case_specs()}
    if set(value["cases"]) != expected:
        raise ValueError("complete 24-case light matrix required")
    projection = {"width": WIDTH, "height": HEIGHT, "ortho_height": ORTHO_HEIGHT,
                  "rois": [list(roi(i)) for i in range(COUNT)]}
    if value.get("projection") != projection:
        raise ValueError("stored projection differs from independent fixture")
    computed = {}
    for kind in KINDS:
        pixels = {}
        for label, active, first, geometry in case_specs():
            case = value["cases"][kind + "/" + label]
            if set(case["files"]) != {"scene.json", "engine.log", "target.png"}:
                raise ValueError("complete case raw provenance required")
            for entry in case["files"].values():
                checked(Path(entry["path"]), entry["sha256"])
            retained_scene = json.loads(Path(case["files"]["scene.json"]["path"]).read_text(encoding="utf-8"))
            if retained_scene != scene(kind, active, first=first, geometry=geometry):
                raise ValueError("retained light configuration differs")
            process = case["process"]
            if process["process"].get("returncode") != 0 or process["process"].get("timed_out"):
                raise ValueError("light consumer failed or timed out")
            log = Path(case["files"]["engine.log"]["path"]).read_text(encoding="utf-8")
            actual = assert_backend(log, value["backend"])
            if actual != process.get("backend_actual") or "] ERROR -" in log or                     "JCE_FORWARDPLUS set -> r.forwardplus ON" not in log:
                raise ValueError("backend, renderer or explicit Forward+ authority failed")
            env = case["environment"]
            if env.get("JCE_BACKEND") != value["backend"] or env.get("JCE_FORWARDPLUS") != "1":
                raise ValueError("requested light backend/authority differs")
            pixels[label] = image(Path(case["files"]["target.png"]["path"]))
        computed[kind] = validate(pixels)
        if computed[kind] != value.get(kind):
            raise ValueError("stored patch metrics differ from original pixels")
    return value, computed


def matrix(loaded):
    if len(loaded) != 4 or {row[0]["backend"] for row in loaded} != set(BACKENDS):
        raise ValueError("four distinct actual Windows backends required")
    if len({row[0]["executable_sha256"] for row in loaded}) != 1:
        raise ValueError("mixed executable identities")
    return {value["backend"]: (value, metrics) for value, metrics in loaded}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    results = matrix([load(path) for path in args.suite])
    args.out.mkdir(parents=True, exist_ok=False)
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    rows = [results[backend][1][kind]["hundred"] for kind in KINDS for backend in BACKENDS]
    figure, axis = plt.subplots(figsize=(15, 5))
    heat = axis.imshow(np.asarray(rows), aspect="auto", cmap="viridis", vmin=0)
    axis.set_yticks(range(8), [kind + " / " + backend for kind in KINDS for backend in BACKENDS])
    axis.set_xlabel("Independent receiving patch index (0..99)")
    axis.set_title("Original-pixel light response: correctness, not performance")
    figure.colorbar(heat, ax=axis, label="Mean RGB increase over zero-light baseline (8-bit units)")
    figure.tight_layout()
    figure.savefig(args.out / "lights.png", dpi=140)
    plt.close(figure)
    lines = ["# Four-backend independent light participation", "",
        "All 24 cases per backend were rehashed and independently revalidated from original pixels.",
        "Retained scenes, actual backend logs and the same tested executable were checked.", "",
        "![100 independent receiving-patch responses](lights.png)", "",
        "| Kind | Backend | Ten positive ROIs | Hundred positive ROIs |", "| --- | --- | ---: | ---: |"]
    for kind in KINDS:
        for backend in BACKENDS:
            metrics = results[backend][1][kind]
            lines.append("| %s | %s | %u | %u |" % (kind, backend,
                sum(v >= 8 for v in metrics["ten"]), sum(v >= 8 for v in metrics["hundred"])))
    lines.extend(["", "Every case retains 100 authored light components. Ten/hundred counts refer",
        "to positive-intensity lights and observed independent patch responses, not throughput.",
        "Omitted-last, authored-order late/reordered, repeat and geometry controls also pass.",
        "No shadow, overlapping-froxel capacity, physical light fidelity, GPU timing or cross-engine speed claim.", "",
        "Executable SHA-256: `" + results[BACKENDS[0]][0]["executable_sha256"] + "`.", ""])
    (args.out / "REPORT.md").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    write(args.out / "recomputed.json", {"status": "LIGHT_RAW_CONTROLS_PASS",
        "metrics": {backend: results[backend][1] for backend in BACKENDS},
        "executable_sha256": results[BACKENDS[0]][0]["executable_sha256"],
        "inputs": [{"path": str(path.resolve()), "sha256": sha256_file(path)} for path in args.suite]})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
