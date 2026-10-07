"""Revalidate sampled camera/occluder GPU history before plotting a full matrix."""
import argparse
import json
from pathlib import Path

from gpu_history_suite import (MOTION_FRAMES, MOTION_COUNTER_FRAMES, expected_survivors,
                               peak_revealed_cells, validate_motion)
from gpu_model_suite import full_pixel_difference, player_counters, read_png, sha256_file
from gpu_model_suite import validate_gpu, validate_model
from gpu_counter_evidence import read_survivors
from occlusion_suite import cell_changes, validate_history
from tools.perf_bench import assert_backend

BACKENDS = ("d3d11", "d3d12", "vulkan", "gl")
MOTIONS = ("camera", "occluder")


def validate_inventory(entries):
    pairs = [(entry["motion"], entry["backend"]) for entry in entries]
    if len(pairs) != 8 or set(pairs) != {(motion, backend)
                                      for motion in MOTIONS for backend in BACKENDS}:
        raise ValueError("one complete camera/occluder matrix on four backends required")
    if len({entry["executable_sha256"] for entry in entries}) != 1:
        raise ValueError("motion matrix mixes different executable builds")


def checked_image(root, label, entry):
    path = root / label / "frame.png"
    if Path(entry["image"]).resolve() != path.resolve() or \
            sha256_file(path) != entry["image_sha256"]:
        raise ValueError("a raw capture moved or changed")
    return read_png(path)


def revalidate(path, exe, asset, packer):
    data = json.loads(path.read_text(encoding="utf-8"))
    motion, backend, count = (data[key] for key in ("motion", "backend", "count"))
    if motion not in MOTIONS or backend not in BACKENDS or \
            data["status"] != motion.upper() + "_MOTION_WITNESS_PASS":
        raise ValueError("completed motion evidence required")
    for key, artifact in (("executable_sha256", exe), ("asset_sha256", asset),
                          ("packer_sha256", packer), ("scene_sha256", path.parent / "scene.json"),
                          ("pak_sha256", path.parent / "fixture.pak")):
        if sha256_file(artifact) != data[key]:
            raise ValueError("a source artifact changed: " + key)
    intent = json.loads((path.parent / "intent.json").read_text(encoding="utf-8"))
    if any(data[key] != value for key, value in intent.items()):
        raise ValueError("the completed suite disagrees with its prelaunch identity")
    labels = ["empty", "cpu", "hiz", "repeat"]
    labels += ["reveal-%s-%u" % (mode, frame) for frame in MOTION_FRAMES for mode in ("cpu", "hiz")]
    if set(data["captures"]) != set(labels):
        raise ValueError("missing or unexpected control captures")
    images, hashes, counters = {}, {}, {}
    for label in labels:
        entry, target = data["captures"][label], path.parent / label
        process = entry["process"]["process"]
        if process["returncode"] or process["timed_out"]:
            raise ValueError("a capture process did not complete cleanly")
        log = (target / "visual.log").read_text(encoding="utf-8")
        stdout = (target / "visual_process/stdout.log").read_text(encoding="utf-8")
        if assert_backend(stdout + "\n" + log, backend) != entry["process"]["backend_actual"]:
            raise ValueError("the actual renderer differs from the retained backend")
        resources = player_counters((target / "engine_counters.jsonl").read_text(encoding="utf-8"))
        if resources != entry["resources"]:
            raise ValueError("raw resource observations differ from the retained summary")
        mode = "cpu" if label == "empty" or label == "cpu" or label.startswith("reveal-cpu-") else "hiz"
        if label != "empty":
            validate_model(log, count, data["vertices"], wall=True, reveal=False)
            validate_gpu(resources, count, mode, extra_entities=4)
        validate_motion(log, motion, label != "empty")
        images[label] = checked_image(path.parent, label, entry)
        hashes[label] = {"image": entry["image_sha256"], "log": sha256_file(target / "visual.log"),
                         "resources": sha256_file(target / "engine_counters.jsonl")}
        if mode == "hiz":
            actual = read_survivors(target / "survivors.jsonl", count,
                lambda frame: expected_survivors(count, frame, False, True), frames=MOTION_COUNTER_FRAMES)
            if actual != data["gpu_survivors"][label]:
                raise ValueError("raw GPU counters differ from the retained summary")
            counters[label] = actual
    history = {str(frame): full_pixel_difference(images["reveal-cpu-%u" % frame],
               images["reveal-hiz-%u" % frame], threshold=5) for frame in MOTION_FRAMES}
    validate_history(history, MOTION_FRAMES)
    if history != data["history"] or any(any(cell_changes(images["reveal-cpu-%u" % frame],
            images["reveal-hiz-%u" % frame], count)) for frame in MOTION_FRAMES):
        raise ValueError("raw per-object history differs")
    if full_pixel_difference(images["empty"], images["cpu"]) <= 0 or \
            full_pixel_difference(images["cpu"], images["hiz"], threshold=5) >= .0001 or \
            full_pixel_difference(images["hiz"], images["repeat"], threshold=5) >= .0001:
        raise ValueError("static population/parity/noise control failed")
    change = full_pixel_difference(images["cpu"], images["reveal-cpu-66"], threshold=5)
    if change <= .0001 or change != data["motion_pixel_change"]:
        raise ValueError("the peak image did not reproduce recorded motion")
    cells = peak_revealed_cells(images["empty"], images["reveal-cpu-66"], count) if motion == "occluder" else []
    return {"suite": str(path.resolve()), "suite_sha256": sha256_file(path), "backend": backend,
            "motion": motion, "count": count, "executable_sha256": data["executable_sha256"],
            "history": history, "motion_pixel_change": change, "peak_revealed_cells": cells,
            "survivors": [row["survivors"] for row in counters["hiz"]["rows"]],
            "raw_counters": counters, "raw_capture_hashes": hashes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", type=Path, action="append", required=True)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--asset", type=Path, required=True)
    parser.add_argument("--packer", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    entries = [revalidate(path, args.exe, args.asset, args.packer) for path in args.suite]
    validate_inventory(entries)
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    args.out.mkdir(parents=True, exist_ok=False)
    figure, axes = plt.subplots(1, 2, figsize=(11, 4), sharey=True)
    for axis, motion in zip(axes, MOTIONS):
        for backend, marker in zip(BACKENDS, ("o", "s", "x", "+")):
            entry = next(row for row in entries if row["motion"] == motion and row["backend"] == backend)
            axis.scatter(range(len(MOTION_COUNTER_FRAMES)), entry["survivors"],
                         label=backend, marker=marker, s=55)
        axis.set_xticks(range(len(MOTION_COUNTER_FRAMES)),
                       labels=[str(frame) for frame in MOTION_COUNTER_FRAMES])
        axis.set_xlabel("Sampled source frame (categorical spacing)")
        axis.set_title(motion.capitalize() + " motion")
        axis.grid(alpha=.2)
        axis.legend(fontsize=8)
    axes[0].set_ylabel("Actual compacted GPU model records")
    figure.suptitle("First move 51; peak 66; restored pose 81; no timing claim")
    figure.tight_layout()
    figure.savefig(args.out / "motion-survivors.png", dpi=150)
    figure.savefig(args.out / "motion-survivors.svg")
    plt.close(figure)
    manifest = {"scope": "sampled GPU history correctness, not throughput or exhaustive motion",
                "rgb_threshold": 5, "counter_frames": MOTION_COUNTER_FRAMES,
                "picture_frames": MOTION_FRAMES, "entries": entries}
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    lines = ["# Sampled camera and occluder GPU history", "",
             "All raw captures, pose logs, actual GPU words and source artifacts were revalidated. The eight cases use one unchanged executable.", "",
             "![Actual GPU counters](motion-survivors.png)", "",
             "| Motion | Backend | Max differing pixels % | Peak image change % | Newly exposed left cells |", "| --- | --- | ---: | ---: | ---: |"]
    for entry in sorted(entries, key=lambda row: (row["motion"], BACKENDS.index(row["backend"]))):
        lines.append("| %s | %s | %.8f | %.5f | %s |" % (entry["motion"], entry["backend"],
                     max(entry["history"].values()) * 100, entry["motion_pixel_change"] * 100,
                     len(entry["peak_revealed_cells"]) if entry["motion"] == "occluder" else "n/a"))
    lines += ["", "Changed pixels use the same RGB delta > 5 and original per-object controls. Newly exposed cells exclude the independently projected wall, with a two-pixel guard; moving wall pixels alone fail.", "",
              "Camera/occluder updates run at 51..80 and restore at 81. The raw depth counters cover nine frames; CPU/HiZ pictures cover seven. Unobserved frames are not inferred. This does not certify deformation, LOD, all scenes, physical old devices or performance.", "",
              "Executable SHA256: `" + entries[0]["executable_sha256"] + "`.", ""]
    lines += ["- [%s/%s raw suite](%s)" % (entry["motion"], entry["backend"], entry["suite"].replace("\\", "/")) for entry in entries]
    (args.out / "REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"report": str(args.out / "REPORT.md"), "cases": len(entries)}))


if __name__ == "__main__":
    main()
