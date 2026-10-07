"""Revalidate retained distance-LOD evidence before plotting captures."""
import argparse
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

from lod_suite import FRAMES, difference, image, sha256_file, validate, write
from tools.jce_determinism import assert_backend


def checked(path, expected):
    if sha256_file(path) != expected:
        raise ValueError("raw input changed: " + str(path))


def load(path, archived_executable):
    result = json.loads(path.read_text(encoding="utf-8"))
    if result.get("status") != "LOD_ROUTE_CONTROLS_PASS":
        raise ValueError("LOD suite did not pass")
    checked(archived_executable, result["executable_sha256"])
    if set(result["cases"]) != {mode + "-%u" % frame for frame in FRAMES
                                for mode in ("empty", "off", "on", "repeat")}:
        raise ValueError("complete LOD capture matrix required")
    captures = {}
    for label, case in result["cases"].items():
        for entry in case["files"].values():
            checked(Path(entry["path"]), entry["sha256"])
        process = case["process"]
        if process["process"]["returncode"] != 0:
            raise ValueError("LOD consumer did not exit cleanly")
        log = Path(case["files"]["engine.log"]["path"]).read_text(encoding="utf-8")
        actual = assert_backend(process["process"].get("stdout", "") + "\n" + log,
                                result["backend"])
        if actual != process["backend_actual"] or "] ERROR -" in log:
            raise ValueError("backend identity or renderer error")
        rows = validate(log, label.split("-")[0] in ("on", "repeat"))
        if rows != case["markers"]:
            raise ValueError("stored LOD markers differ from retained log")
        captures[label] = image(Path(case["files"]["target.png"]["path"]))
        if captures[label].shape != (720, 1280, 4):
            raise ValueError("LOD capture dimensions differ")
    for frame in FRAMES:
        pixels = {mode: captures[mode + "-%u" % frame]
                  for mode in ("empty", "off", "on", "repeat")}
        metrics = {"coverage": difference(pixels["off"], pixels["empty"]),
                   "parity": difference(pixels["off"], pixels["on"]),
                   "repeat": difference(pixels["on"], pixels["repeat"])}
        if metrics != result["pixels"][str(frame)] or metrics["coverage"] <= .002 \
                or metrics["parity"] > .001 or metrics["repeat"] > .0001:
            raise ValueError("LOD pixel controls differ or fail")
    return result, captures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", action="append", type=Path, required=True)
    parser.add_argument("--archived-exe", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    loaded = [load(path, args.archived_exe) for path in args.suite]
    backends = ("d3d11", "d3d12", "vulkan", "gl")
    results = {result["backend"]: (result, pixels) for result, pixels in loaded}
    if len(loaded) != 4 or set(results) != set(backends):
        raise ValueError("four distinct actual backends required")
    args.out.mkdir(parents=True, exist_ok=False)
    figure, axes = plt.subplots(4, 4, figsize=(16, 12))
    for row, backend in enumerate(backends):
        for column, frame in enumerate((20, 50, 80, 180)):
            axes[row, column].imshow(results[backend][1]["on-%u" % frame][:, :, :3])
            axes[row, column].set_title("%s / frame %u" % (backend, frame))
            axes[row, column].axis("off")
    figure.tight_layout()
    figure.savefig(args.out / "lod.png", dpi=130)
    plt.close(figure)
    lines = ["# Global distance LOD: four Windows backends", "",
             "Executable, scenes, logs and captures were rehashed. All 200 selection",
             "frames per case were reparsed against the independent distance/hysteresis oracle.",
             "Eight capture points pass empty, disabled-LOD and repeat controls.", "",
             "![Actual near, transition, far and return captures](lod.png)", "",
             "The fixture contains 64 mesh entities; camera and light must not receive LOD meshes.",
             "Input mesh triangles: 2048 / 128 / 2. These are not measured GPU primitive counts.",
             "Raw OpenGL captures have the opposite vertical orientation from the other backends.",
             "Within-backend controls pass; cross-backend display orientation is not certified.",
             "No throughput, cooked asset LOD, streaming or cross-engine speed claim is made.", "",
             "Executable SHA-256: `" + sha256_file(args.archived_exe) + "`.", ""]
    (args.out / "REPORT.md").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    write(args.out / "inputs.json", {"suites": [{"path": str(path.resolve()),
          "sha256": sha256_file(path)} for path in args.suite]})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
