"""Rehash and plot actual stock SDK post-processing pixel controls."""
import argparse
import json
from pathlib import Path

from postfx_fixture import EFFECTS
from postfx_suite import image_metrics, read_png, sha256_file, validate_log, validate_metrics
from tools.perf_bench import assert_backend

BACKENDS = ("d3d11", "d3d12", "vulkan", "gl")
LABELS = ("empty", "base", "repeat", *EFFECTS[1:])


def revalidate(path, executable, scenes=None):
    data = json.loads(path.read_text(encoding="utf-8"))
    if data["status"] != "POSTFX_LIVENESS_PASS" or set(data["cases"]) != set(LABELS):
        raise ValueError("complete stock SDK effect controls required")
    if sha256_file(executable) != data["executable_sha256"]:
        raise ValueError("archived consumer does not match the matrix")
    images = {}
    for label in LABELS:
        target, entry = path.parent / label, data["cases"][label]
        scene_name = "base" if label == "repeat" else label
        if scenes is None or sha256_file(scenes / (scene_name + ".scene.json")) != entry["scene_sha256"]:
            raise ValueError("archived fixture source differs: " + label)
        image, log_path = target / "frame.png", target / "engine.log"
        if sha256_file(image) != entry["image_sha256"] or \
                sha256_file(log_path) != entry["log_sha256"]:
            raise ValueError("raw capture changed: " + label)
        log = log_path.read_text(encoding="utf-8")
        if assert_backend(log, data["backend"]) != entry["process"]["backend_actual"]:
            raise ValueError("actual backend changed")
        views = validate_log(log, "base" if label in ("empty", "repeat") else label)
        if {str(key): value for key, value in views.items()} != entry["views"]:
            raise ValueError("raw GPU view observations differ")
        images[label] = read_png(image)
    metrics = image_metrics(images)
    validate_metrics(metrics)
    if any(metrics[key] != data[key] for key in metrics):
        raise ValueError("retained pixel summary differs from raw pictures")
    return {"backend": data["backend"], "suite": str(path.resolve()),
            "suite_sha256": sha256_file(path), "executable_sha256": data["executable_sha256"],
            **metrics}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", type=Path, action="append", required=True)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--scenes", type=Path, required=True,
                        help="unchanged fixture input directory retained with this matrix")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    rows = [revalidate(path, args.exe, args.scenes) for path in args.suite]
    if len(rows) != 4 or {row["backend"] for row in rows} != set(BACKENDS) or \
            len({row["executable_sha256"] for row in rows}) != 1:
        raise ValueError("one complete four-backend matrix with one binary required")
    rows.sort(key=lambda row: BACKENDS.index(row["backend"]))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    args.out.mkdir(parents=True, exist_ok=False)
    figure, axes = plt.subplots(2, 4, figsize=(16, 5))
    for column, row in enumerate(rows):
        root = Path(row["suite"]).parent
        for axis, label in zip(axes[:, column], ("base", "ssr")):
            axis.imshow(plt.imread(root / label / "frame.png"))
            axis.set_title(row["backend"].upper() + ": " + label.upper())
            axis.axis("off")
    figure.tight_layout()
    figure.savefig(args.out / "postfx-captures.png", dpi=150)
    plt.close(figure)
    figure, axis = plt.subplots(figsize=(11, 4))
    for index, row in enumerate(rows):
        axis.bar([i + (index - 1.5) * .19 for i in range(5)],
                 [100 * row["effect_changed_fractions"][effect] for effect in EFFECTS[1:]],
                 width=.19, label=row["backend"])
    axis.set_xticks(range(5), labels=[effect.upper() for effect in EFFECTS[1:]])
    axis.set_ylabel("Pixels differing from base (%)")
    axis.set_title("Actual post-effect liveness; RGB threshold 2; no timing or fidelity claim")
    axis.legend()
    figure.tight_layout()
    figure.savefig(args.out / "postfx-pixels.png", dpi=150)
    figure.savefig(args.out / "postfx-pixels.svg")
    plt.close(figure)
    lines = ["# Stock SDK post-processing correctness", "",
             "Four actual graphics backends; one archived executable; each raw image/log was rehashed.", "",
             "![Actual base and SSR captures](postfx-captures.png)", "",
             "![Raw-derived effect coverage](postfx-pixels.png)", "",
             "| Backend | Repeat | TAA | SSAO | SSR | Bloom | Fog |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for row in rows:
        values = [row["repeat_fraction"],
                  *[row["effect_changed_fractions"][effect] for effect in EFFECTS[1:]]]
        lines.append("| " + row["backend"] + " | " +
                     " | ".join(f"{value:.8f}" for value in values) + " |")
    lines += ["", "Values are pixel fractions, not milliseconds or speed ratios.", "",
              "The fixture is a first-party procedural PBR room. Pass presence plus a visible change",
              "does not certify physical lighting, motion quality, TAA convergence or external-engine parity.",
              "Concurrent dependency/unit builds exclude these captures from formal performance evidence.", "",
              "The depth-prepass cache must distinguish depth-only and normal/albedo-producing outputs.",
              "Primitive G-buffer draws must bind their base colour as well as roughness.", "",
              "Executable SHA-256: `" + rows[0]["executable_sha256"] + "`."]
    (args.out / "REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    (args.out / "manifest.json").write_text(json.dumps({"rows": rows}, indent=2) + "\n",
                                             encoding="utf-8", newline="\n")
    print(args.out / "REPORT.md")


if __name__ == "__main__":
    main()
