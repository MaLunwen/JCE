"""Revalidate four-backend static asset captures and optional before/after pairs."""
import argparse
import json
from pathlib import Path

from check_asset_pixels import full_pixel_difference, read_png
from static_asset_suite import glb_census, pixels_pass, validate_import, validate_resources
from tools.benchmark_suite import player_counters, sha256_file
from tools.perf_bench import assert_backend

BACKENDS = ("d3d11", "d3d12", "vulkan", "gl")
LABELS = ("empty", "base20", "base50", "repeat50")


def validate_inventory(entries):
    if len(entries) != 4 or {entry["backend"] for entry in entries} != set(BACKENDS) or \
            len({entry["executable_sha256"] for entry in entries}) != 1 or \
            len({entry["asset_sha256"] for entry in entries}) != 1:
        raise ValueError("one complete four-backend matrix with one binary/asset required")


def validate_pair(before, after, before_scene, after_scene, before_image, after_image):
    if before["backend"] != after["backend"] or before["asset_sha256"] != after["asset_sha256"] or \
            before["census"] != after["census"] or before_scene != after_scene:
        raise ValueError("before/after asset, topology and framing must match")
    delta = full_pixel_difference(before_image, after_image, threshold=5)
    if delta >= .0001:
        raise ValueError("before/after static pictures differ beyond unchanged controls")
    return delta


def revalidate(path, exe, asset, packer):
    data = json.loads(path.read_text(encoding="utf-8"))
    if data["status"] != "STATIC_ASSET_LIVENESS_PASS" or data["rgb_threshold"] != 5 or \
            set(data["captures"]) != set(LABELS) or data["census"] != glb_census(asset.read_bytes()):
        raise ValueError("completed original-asset controls required")
    for key, artifact in (("executable", exe), ("asset", asset), ("packer", packer),
                          ("pak", path.parent / "fixture.pak")):
        if sha256_file(artifact) != data[key + "_sha256"]:
            raise ValueError("source artifact changed: " + key)
    images, hashes, peaks = {}, {}, []
    for label in LABELS:
        target, entry = path.parent / label, data["captures"][label]
        paths = {"image": target / "frame.png", "scene": target / "scene.json",
                 "log": target / "visual.log"}
        hashes[label] = {name: sha256_file(artifact) for name, artifact in paths.items()}
        if any(hashes[label][name] != entry[name + "_sha256"] for name in paths):
            raise ValueError("capture artifact changed: " + label)
        log = paths["log"].read_text(encoding="utf-8")
        stdout = (target / "visual_process/stdout.log").read_text(encoding="utf-8")
        if assert_backend(stdout + "\n" + log, data["backend"]) != entry["process"]["backend_actual"]:
            raise ValueError("actual renderer differs")
        topology = [list(row) for row in validate_import(log, data["census"], label != "empty")]
        resources_path = target / "engine_counters.jsonl"
        resources = player_counters(resources_path.read_text(encoding="utf-8"))
        validate_resources(resources, label != "empty")
        if resources != entry["resources"] or topology != entry["topology"]:
            raise ValueError("raw observations differ from retained summary")
        process_path = target / "visual_process/result.json"
        samples_path = target / "visual_process/samples.json"
        process = json.loads(process_path.read_text(encoding="utf-8"))
        samples = json.loads(samples_path.read_text(encoding="utf-8"))
        if process != entry["process"]["process"] or samples != entry["process"]["process_samples"] or \
                process["returncode"] or process["timed_out"] or not process["sampling_available"] or \
                not samples or process["peak_rss_bytes"] is None or process["peak_rss_bytes"] <= 0:
            raise ValueError("completed raw process sampling required")
        hashes[label].update(resources=sha256_file(resources_path), process=sha256_file(process_path),
                             samples=sha256_file(samples_path))
        if label != "empty":
            peaks.append(process["peak_rss_bytes"])
        images[label] = read_png(paths["image"])
    deltas = {"coverage": full_pixel_difference(images["empty"], images["base50"], threshold=5),
              "frame20_50": full_pixel_difference(images["base20"], images["base50"], threshold=5),
              "repeat": full_pixel_difference(images["base50"], images["repeat50"], threshold=5)}
    if deltas != data["deltas"] or not pixels_pass(deltas):
        raise ValueError("raw image controls disagree")
    entry = {key: data[key] for key in ("backend", "executable_sha256", "asset_sha256", "census")}
    entry.update(suite=str(path), suite_sha256=sha256_file(path), hashes=hashes, deltas=deltas,
                 peak_rss_mib=max(peaks) / 1048576,
                 model_estimate_mib=int(data["captures"]["base50"]["topology"][-1][9]) / 1048576)
    return entry, json.loads((path.parent / "base50/scene.json").read_text()), images["base50"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("exe", "asset", "packer", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--suite", type=Path, nargs=4, required=True)
    parser.add_argument("--reference", type=Path, nargs=4)
    parser.add_argument("--reference-exe", type=Path)
    args = parser.parse_args()
    if bool(args.reference) != bool(args.reference_exe):
        parser.error("reference matrix and its archived executable are required together")
    checked = [revalidate(path, args.exe, args.asset, args.packer) for path in args.suite]
    entries = [item[0] for item in checked]
    validate_inventory(entries)
    before = [revalidate(path, args.reference_exe, args.asset, args.packer)
              for path in args.reference] if args.reference else []
    if before:
        validate_inventory([item[0] for item in before])
        for entry, scene, picture in checked:
            old, old_scene, old_picture = next(item for item in before if item[0]["backend"] == entry["backend"])
            entry["before_after_delta"] = validate_pair(old, entry, old_scene, scene, old_picture, picture)
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    args.out.mkdir(parents=True, exist_ok=False)
    figure, axes = plt.subplots(2, 2, figsize=(12, 7))
    for axis, backend in zip(axes.flat, BACKENDS):
        row = next(item[0] for item in checked if item[0]["backend"] == backend)
        path = Path(row["suite"]).parent / "base50/frame.png"
        axis.imshow(plt.imread(path))
        axis.set_title(backend.upper() + ": original static GLB, fixture frame 50")
        axis.axis("off")
    figure.tight_layout()
    figure.savefig(args.out / "static-captures.png", dpi=150)
    plt.close(figure)
    figure, axes = plt.subplots(1, 2, figsize=(12, 4))
    for offset, label, rows in ((-.18, "Reference", [item[0] for item in before]),
                                (.18 if before else 0, "Candidate", entries)):
        if not rows: continue
        ordered = [next(row for row in rows if row["backend"] == backend) for backend in BACKENDS]
        for axis, key in zip(axes, ("model_estimate_mib", "peak_rss_mib")):
            axis.bar([i + offset for i in range(4)], [row[key] for row in ordered], width=.36, label=label)
    for axis in axes:
        axis.set_xticks(range(4), labels=BACKENDS)
        axis.set_ylabel("MiB")
        axis.legend()
    axes[0].set_title("Model residency estimate (not physical VRAM)")
    axes[1].set_title("Max observed process peak RSS across captures")
    figure.suptitle("Correctness runs; concurrent build load; no timing/speedup claim")
    figure.tight_layout()
    figure.savefig(args.out / "static-memory.png", dpi=150)
    figure.savefig(args.out / "static-memory.svg")
    plt.close(figure)
    manifest = {"scope": "static liveness and diagnostic memory; no timing or legacy acceptance",
                "entries": entries, "reference": [item[0] for item in before]}
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    lines = ["# Complex static asset: independently revalidated captures", "",
             "All four actual backends use the unchanged original asset and one candidate executable.", "",
             "![Actual captures](static-captures.png)", "", "![Diagnostic memory](static-memory.png)", "",
             "| Backend | Coverage delta | Temporal/repeat delta | Model estimate MiB | Process peak MiB |",
             "| --- | ---: | ---: | ---: | ---: |"]
    for row in entries:
        lines.append(f"| {row['backend']} | {row['deltas']['coverage']:.8f} | "
                     f"{row['deltas']['frame20_50']:.8f}/{row['deltas']['repeat']:.8f} | "
                     f"{row['model_estimate_mib']:.3f} | {row['peak_rss_mib']:.3f} |")
    lines += ["", "Resident texture estimates use RGBA8-equivalent dimensions; mesh strides are representative.",
              "Mip-chain overhead, driver staging and exact physical GPU allocation are not measured by that counter.",
              "RSS covers entire capture processes, including startup/decode/upload, on a modern host with concurrent build work.",
              "Neither these memory observations nor async-ready captures establish throughput, legacy-device acceptance or optional glTF extension fidelity."]
    if before:
        lines += ["", "Paired images use identical source asset, topology, camera and scene. Before/after changed-pixel fractions:"]
        lines += [f"- {row['backend']}: {row['before_after_delta']:.8f}" for row in entries]
    (args.out / "REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"report": str(args.out), "status": "STATIC_REPORT_REVALIDATED"}))


if __name__ == "__main__":
    main()
