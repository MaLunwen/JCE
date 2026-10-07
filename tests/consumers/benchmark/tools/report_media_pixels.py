"""Recalculate retained seek-picture RGB evidence before drawing comparisons."""
import argparse
import json
from pathlib import Path

import numpy as np

from culling_suite import sha256_file
from media_pixel_suite import PICTURE, compare_rgba, reference_yuv420


def load_picture(row):
    directory = Path(row["directory"])
    native, reference = directory / "native.rgba", directory / "reference.rgba"
    if sha256_file(Path(row["file"])) != row["asset_sha256"] or \
            sha256_file(native) != row["raw_sha256"] or \
            sha256_file(reference) != row["reference_sha256"]:
        raise ValueError("retained media/picture changed")
    width, height = row["width"], row["height"]
    if width <= 0 or height <= 0 or abs(row["native_pts"] - row["reference_pts"]) > 0.00001:
        raise ValueError("picture dimensions or PTS disagree")
    process = row["process"]
    log = Path(process["stdout_path"]).read_text(encoding="utf-8", errors="replace")
    picture = PICTURE.search(log)
    if process["returncode"] or process["timed_out"] or log != process["stdout"] or not picture or \
            (int(picture[1]), int(picture[2]), float(picture[3]), int(picture[4])) != \
            (width, height, row["native_pts"], width * height * 4):
        raise ValueError("native picture/PTS does not match retained probe output")
    frames = json.loads((directory / "reference-frames.json").read_text(encoding="utf-8"))["frames"]
    origin = float(frames[0]["best_effort_timestamp_time"])
    frame = frames[row["reference_frame_index"]]
    if abs(float(frame["best_effort_timestamp_time"]) - origin - row["reference_pts"]) > 1e-12 or \
            (int(frame["width"]), int(frame["height"])) != (width, height):
        raise ValueError("reference PTS does not match independently decoded frame records")
    pixels = [np.frombuffer(path.read_bytes(), np.uint8).reshape(height, width, 4)
              for path in (native, reference)]
    if row.get("oracle") == "nearest-yuv":
        yuv = directory / "reference.yuv"
        if sha256_file(yuv) != row["reference_yuv_sha256"] or not np.array_equal(
                reference_yuv420(yuv.read_bytes(), width, height, frame), pixels[1]):
            raise ValueError("independent nearest YUV reference was changed")
    observation = compare_rgba(*pixels)
    for key, value in observation.items():
        if abs(value - row[key]) > 1e-12:
            raise ValueError("recorded RGB observations disagree with raw pictures")
    if (observation["changed_fraction"] <= 0.001) != (row["status"] == "PASS"):
        raise ValueError("picture acceptance disagrees with the declared budget")
    return observation


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", type=Path, action="append", required=True)
    parser.add_argument("--label", action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if len(args.suite) != len(args.label):
        parser.error("one label per input suite required")
    args.out.mkdir(parents=True, exist_ok=False)
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(10, 5))
    lines = ["# Independent media picture comparisons", "",
            "Raw CPU seek pictures, matched by independently decoded PTS. RGB threshold: "
             "5 bytes; allowed changed fraction: 0.001. Decode timing, GPU/editor "
             "presentation, HDR and post-seek audio resume are separate acceptance requirements.", "",
             "| Revision | Clip | Native / reference PTS | Changed RGB pixels | Mean / max error | Status |",
             "| --- | --- | --- | --- | --- | --- |"]
    count = len(args.suite)
    names, oracle = None, None
    identities = []
    for index, (path, label) in enumerate(zip(args.suite, args.label)):
        suite = json.loads(path.read_text(encoding="utf-8"))
        current_oracle = suite.get("oracle", "ffmpeg")
        if oracle is not None and oracle != current_oracle:
            raise ValueError("comparison suites use different chroma reference policies")
        oracle = current_oracle
        runs = suite["runs"]
        assets = [(row["file"], row["asset_sha256"]) for row in runs]
        if names is not None and names != assets:
            raise ValueError("comparison suites use different assets or ordering")
        names = assets
        observations = [load_picture(row) for row in runs]
        ax.bar(np.arange(len(runs)) + (index - (count - 1) / 2) * 0.8 / count,
               [100 * row["changed_fraction"] for row in observations],
               width=0.8 / count, label=label)
        for row in runs:
            lines.append("| %s | %s | %.9f / %.9f | %.6f%% | %.4f / %u | %s |" % (
                label, Path(row["file"]).name, row["native_pts"], row["reference_pts"],
                100 * row["changed_fraction"], row["mean_rgb_error"], row["max_rgb_error"], row["status"]))
        identities.extend(["", "Declared oracle: `%s`." % suite.get("oracle", "ffmpeg"),
                           "Source suite `%s`, SHA256 `%s`. Executable identities: `%s`." % (
            path.resolve(), sha256_file(path), json.dumps(suite["executables"], sort_keys=True)), ""])
    fig.set_size_inches(max(10, 1.2 * len(names)), 6)
    ax.set_xticks(range(len(names)), [Path(name).name for name, _ in names],
                  rotation=35, ha="right", fontsize=8)
    ax.set_ylabel("Pixels exceeding RGB error 5 (%)")
    ax.set_title("Independent same-PTS color witnesses; failed controls retained")
    ax.legend()
    fig.tight_layout()
    fig.savefig(args.out / "color.png", dpi=160)
    plt.close(fig)
    lines.extend(identities)
    lines.extend(["![RGB comparison](color.png)", "",
                  "Reference tools are external validation inputs, not an engine dependency. "
                  "A passing sample does not prove every picture or codec/color profile correct.", "",
                  "Matrix/range reference: [ITU-T H.273 (2024)]"
                  "(https://www.itu.int/rec/T-REC-H.273-202407-I/en). "
                  "Reference conversion: [FFmpeg scaler documentation]"
                  "(https://www.ffmpeg.org/ffmpeg-scaler.html)."])
    (args.out / "REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(args.out / "REPORT.md")


if __name__ == "__main__":
    main()
