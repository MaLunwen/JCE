#!/usr/bin/env python3
"""Rehash actual normal-buffer controls before exporting their visual report."""
import argparse
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

from gbuffer_asset_suite import image, sha256_file, validate_box, validate_skin, write
from check_asset_pixels import validate_markers
from tools.jce_determinism import assert_backend


def checked(path, expected):
    if sha256_file(path) != expected:
        raise ValueError("raw input changed: " + str(path))


def load(path):
    result = json.loads(path.read_text(encoding="utf-8"))
    if result.get("status") != "GBUFFER_ASSET_CONTROLS_PASS":
        raise ValueError("normal suite did not pass")
    checked(Path(result["executable"]), result["executable_sha256"])
    for source in result["sources"].values():
        checked(Path(source["path"]), source["sha256"])
    pixels = {}
    for label, case in result["cases"].items():
        for entry in case["files"].values():
            checked(Path(entry["path"]), entry["sha256"])
        if case["process"]["process"]["returncode"] != 0:
            raise ValueError("normal consumer did not exit cleanly")
        log = Path(case["files"]["engine.log"]["path"]).read_text(encoding="utf-8")
        process = case["process"]
        actual = assert_backend(process["process"].get("stdout", "") + "\n" + log,
                                result["backend"])
        if actual != process["backend_actual"]:
            raise ValueError("stored backend differs from retained process output")
        if "] ERROR -" in log or "skinned normal G-buffer shader unavailable" in log \
                or "intermediate target: gbuffer.normal 1280x720" not in log:
            raise ValueError("normal target or program did not run")
        if label in ("frozen20", "frozen50", "moving20", "moving50", "repeat50"):
            moving = label in ("moving20", "moving50", "repeat50")
            markers = validate_markers(log, 1, 1728, 24, 3, moving)
            if [list(row) for row in markers] != case["markers"]:
                raise ValueError("stored model markers differ from retained log")
        pixels[label] = image(Path(case["files"]["target.png"]["path"]))
        if pixels[label].shape != (720, 1280, 4):
            raise ValueError("normal capture dimensions differ")
    actual_box = validate_box(pixels["primitive"], pixels["box"])
    actual_skin = validate_skin(pixels)
    if actual_box != result["box"] or actual_skin != result["skin"]:
        raise ValueError("stored normal metrics differ from retained pixels")
    return result, pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", action="append", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    loaded = [load(path) for path in args.suite]
    by_backend = {result["backend"]: (result, pixels) for result, pixels in loaded}
    backends = ("d3d11", "d3d12", "vulkan", "gl")
    if len(loaded) != 4 or set(by_backend) != set(backends):
        raise ValueError("need four distinct actual backends")
    if len({result["executable_sha256"] for result, _ in loaded}) != 1 \
            or len({json.dumps(result["sources"], sort_keys=True) for result, _ in loaded}) != 1:
        raise ValueError("backend source or binary identities differ")
    args.out.mkdir(parents=True, exist_ok=False)
    figure, axes = plt.subplots(2, 4, figsize=(16, 7))
    for column, backend in enumerate(backends):
        _, pixels = by_backend[backend]
        for row, label in enumerate(("frozen20", "moving50")):
            axes[row, column].imshow(pixels[label][:, :, :3])
            axes[row, column].set_title(backend.upper() + " / " + label)
            axes[row, column].axis("off")
    figure.suptitle("Original Fox: normal-buffer captures, fixed camera and clock controls")
    figure.tight_layout()
    figure.savefig(args.out / "normals.png", dpi=140)
    plt.close(figure)
    lines = ["# SDK normal-buffer asset controls", "",
             "All original files, archived executable, scenes, logs and images were rehashed.",
             "Backend and model topology/clock markers were revalidated from retained logs.",
             "Metrics were recomputed from actual RGBA captures. RGB panels show normal",
             "encoding; roughness remains in the validated alpha channel. These are",
             "normal/animation liveness controls, not performance or independent skin",
             "matrix/material fidelity measurements.", "", "![Actual normal captures](normals.png)", "",
             "| Backend | Unit Box exact RGBA | Empty coverage | Frozen delta | Moving delta | Repeat delta |",
             "| --- | --- | ---: | ---: | ---: | ---: |"]
    for backend in backends:
        result, _ = by_backend[backend]
        skin = result["skin"]
        lines.append("| %s | %s | %.8f | %.8f | %.8f | %.8f |" % (
            backend, result["box"]["exact_rgba_match"], skin["empty_coverage"],
            skin["frozen_delta"], skin["moving_delta"], skin["repeat_delta"]))
    lines.extend(["", "RGB difference threshold: two channel units. Frozen/repeat budget: 0.0001.",
                  "Moving delta must exceed 0.0001 and covered normal pixels must exceed 0.002.",
                  "The unit Box comparison requires exact RGBA equality with its primitive control.",
                  "", "Executable SHA-256: `" + loaded[0][0]["executable_sha256"] + "`.", ""])
    (args.out / "REPORT.md").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    write(args.out / "inputs.json", {"suites": [{"path": str(path.resolve()),
          "sha256": sha256_file(path)} for path in args.suite], "results": [r for r, _ in loaded]})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
