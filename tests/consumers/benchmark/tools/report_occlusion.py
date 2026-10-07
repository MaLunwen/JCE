"""Recalculate fixed-wall reveal evidence from byte-verified GPU captures."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[4]))
from check_asset_pixels import full_pixel_difference
from occlusion_suite import REVEAL_FRAMES, cell_changes, validate_history
from tools.benchmark_suite import sha256_file
from tools.jce_determinism import read_png


def capture_history(path, require_repaired=False):
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("status") not in ("PASS", "WITNESS_PASS") or not data.get("captures"):
        raise ValueError("completed fixed-wall evidence required")
    history, hashes, cell_differences = {}, {}, {}
    for frame in REVEAL_FRAMES:
        images = []
        for mode in ("on", "off"):
            label = "reveal-%s-%u" % (mode, frame)
            entry = data["captures"][label]
            image = Path(entry["image"])
            digest = sha256_file(image)
            if digest != entry["image_sha256"]:
                raise ValueError("a raw occlusion capture changed")
            hashes[label] = digest
            images.append(read_png(image))
        history[str(frame)] = full_pixel_difference(*images, threshold=5)
        cell_differences[str(frame)] = cell_changes(*images, data["count"])
        if require_repaired and any(cell_differences[str(frame)]):
            raise ValueError("a repaired object cell still differs from query-off")
    if require_repaired:
        validate_history(history)
        empty = data["captures"]["empty"]
        revealed = data["captures"]["reveal-off-90"]
        for entry in (empty, revealed):
            if sha256_file(Path(entry["image"])) != entry["image_sha256"]:
                raise ValueError("a coverage control changed")
        coverage = cell_changes(read_png(Path(empty["image"])), read_png(Path(revealed["image"])), data["count"])
        if any(value <= 0 for value in coverage):
            raise ValueError("a revealed object has no cell coverage")
    return data, history, hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    before, old, old_hashes = capture_history(args.before)
    after, new, new_hashes = capture_history(args.after, require_repaired=True)
    if before["scene_sha256"] != after["scene_sha256"] or before["count"] != after["count"]:
        raise ValueError("the before/after scene arrangement changed")
    if not any(old[str(frame)] >= 0.0001 for frame in (80, 81, 82)):
        raise ValueError("the before control did not reproduce a reveal defect")
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    args.out.mkdir(parents=True, exist_ok=False)
    figure, axis = plt.subplots(figsize=(9, 4))
    for label, history, color in (("Before fix", old, "#b55f36"), ("After fix", new, "#246fa8")):
        axis.scatter(REVEAL_FRAMES, [history[str(frame)] * 100 for frame in REVEAL_FRAMES],
                     color=color, label=label, s=45)
    axis.axvline(80, color="#555555", linestyle=":", label="Wall moved (frame 80)")
    axis.set_xticks(REVEAL_FRAMES)
    axis.set_xlabel("Independently captured simulation frame")
    axis.set_ylabel("Pixels differing from query-off control (%)")
    axis.set_title("Same fixed-wall scene; RGB delta > 5; no history reset in consumer")
    axis.grid(alpha=0.2)
    axis.legend()
    figure.tight_layout()
    figure.savefig(args.out / "reveal-history.png", dpi=150)
    figure.savefig(args.out / "reveal-history.svg")
    plt.close(figure)
    manifest = {"rgb_threshold": 5, "scope": "sampled GPU reveal parity, not continuous video or HiZ",
                "before": {"suite": str(args.before.resolve()), "suite_sha256": sha256_file(args.before),
                           "executable_sha256": before["executable_sha256"], "history": old, "captures": old_hashes},
                "after": {"suite": str(args.after.resolve()), "suite_sha256": sha256_file(args.after),
                          "executable_sha256": after["executable_sha256"], "history": new, "captures": new_hashes}}
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    lines = ["# Hardware-query reveal regression", "",
             "The wall moves at frame 80. Source captures were rehashed and both series recalculated at the same RGB threshold (5).", "",
             "![Reveal history](reveal-history.png)", "",
             "| Frame | Before difference % | After difference % |", "| --- | ---: | ---: |"]
    lines += ["| %u | %.8f | %.8f |" % (frame, old[str(frame)] * 100, new[str(frame)] * 100)
              for frame in REVEAL_FRAMES]
    lines += ["", "Only these five independent captures are measured; intermediate frames are not inferred.", "",
              "The generic pooled history reset is conservative: notified scene/camera/target changes give up old rejection. Static query results remain reusable.", "",
              "[Before raw suite](%s). [After raw suite](%s)." %
              (args.before.resolve().as_posix(), args.after.resolve().as_posix()), "",
              "This regression does not establish a general performance gain, continuous visual parity, GPUScene/HiZ correctness or full release readiness.", ""]
    (args.out / "REPORT.md").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(json.dumps({"report": str(args.out / "REPORT.md"), "before": old, "after": new}))


if __name__ == "__main__":
    main()
