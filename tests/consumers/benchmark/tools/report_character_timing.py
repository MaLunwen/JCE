#!/usr/bin/env python3
"""Imported normal-play character frames; no independent GPU deformation oracle."""
import argparse
import json
from pathlib import Path
import statistics
from report_component_timing import rehash, measured
from particle_timing_suite import validate_process
from time_asset import witness_input, timing_environment
from check_asset_pixels import (read_png, full_pixel_difference, chromatic_fraction,
    cell_pixel_changes, cells_pass, validate_markers, validate_population, validate_still_controls)
from tools.benchmark_suite import player_counters
from tools.jce_determinism import assert_backend
from gbuffer_asset_suite import sha256_file, write


def validate_identity(data, expected_count):
    if data.get("status") != "PASS" or data.get("count") != expected_count or len(data.get("runs", [])) != 7:
        raise ValueError("complete seven-repeat matching character count required")


def load(path, exe, asset, packer, count):
    data = json.loads(path.read_text(encoding="utf-8"))
    validate_identity(data, count)
    witness_path = Path(data["witness"])
    rehash(witness_path, data["witness_sha256"])
    witness, original, actual_count = witness_input(witness_path, exe)
    if actual_count != count or original.get("JCE_BENCH_ASSET_PLAY") != "1":
        raise ValueError("normal-play/count witness mismatch")
    rehash(asset, witness["asset_sha256"]); rehash(packer, witness["packer_sha256"])
    images, hashes = {}, {}
    labels = {"empty", "frozen20", "frozen50", "moving20", "moving50", "repeat50"}
    if set(witness["results"]) != labels:
        raise ValueError("missing witness control")
    topology = witness["results"]["moving50"]["live_samples"][0]
    backend = original["JCE_BACKEND"]
    for label, sample in witness["results"].items():
        target = Path(sample["image"]).parent
        log_path = target / "visual.log"
        log = log_path.read_text(encoding="utf-8")
        assert_backend(log, backend); validate_still_controls(log)
        moving = label.startswith("moving") or label == "repeat50"
        if label != "empty":
            rows = validate_markers(log, count, topology[3], topology[5], topology[6], moving)
            population = validate_population(log, count, tuple(witness["receiver_layers"]), moving)
            if [list(r) for r in rows] != sample["live_samples"] or [list(r) for r in population] != sample["population_samples"]:
                raise ValueError("witness raw population mismatch")
        images[label] = read_png(sample["image"])
        hashes[label] = {"log_sha256_reobserved": sha256_file(log_path),
                         "image_sha256": sample["image_sha256"], "scene_sha256": sample["scene_sha256"]}
    pairs = {"asset_coverage": ("frozen20", "empty"), "frozen_clock_difference": ("frozen20", "frozen50"),
             "moving_clock_difference": ("moving20", "moving50"), "repeat_noise": ("moving50", "repeat50")}
    differences = {key: full_pixel_difference(images[a], images[b]) for key, (a,b) in pairs.items()}
    differences["chromatic_coverage"] = chromatic_fraction(images["moving50"])
    if differences != witness["differences"]:
        raise ValueError("witness pixels disagree")
    for key, pair in {"coverage": ("frozen20", "empty"), "motion": ("moving20", "moving50")}.items():
        changes = cell_pixel_changes(images[pair[0]], images[pair[1]], count)
        if changes != witness["per_cell_changed_pixels"][key] or not cells_pass(changes, count):
            raise ValueError("per-character pixel control mismatch")
    rows = []
    for row in data["runs"]:
        directory = Path(row["directory"])
        if row["status"] != "PASS" or row["durable_flush_interval"] != 0:
            raise ValueError("invalid timing condition")
        intent = json.loads((directory / "intent.json").read_text())
        if (intent["executable_sha256"] != sha256_file(exe) or intent["witness_sha256"] != data["witness_sha256"]
            or intent["warmup_intervals"] != 120 or intent["measured_intervals"] != 600):
            raise ValueError("timing intent mismatch")
        expected_env = timing_environment(original, directory)
        expected_env["JCE_KPI_DURABLE_FLUSH_INTERVAL"] = "0"
        if intent["environment"] != expected_env:
            raise ValueError("timing environment differs from normal play")
        rehash(directory / "frames.csv", row["raw_sha256"]); rehash(directory / "engine.log", row["log_sha256"])
        validate_process(row["process"], backend)
        log = (directory / "engine.log").read_text(encoding="utf-8")
        assert_backend(log, backend); validate_still_controls(log)
        if "] ERROR -" in log or "benchmark-player: entered Play" not in log:
            raise ValueError("runtime simulation failed")
        validate_markers(log, count, topology[3], topology[5], topology[6], True)
        validate_population(log, count, tuple(witness["receiver_layers"]), True)
        resources = player_counters((directory / "engine_counters.jsonl").read_text())
        if resources != row["resources"] or resources[-1]["engine"]["entity_count"] != count+2:
            raise ValueError("resource/population mismatch")
        frames, summary = measured(directory / "frames.csv", 721)
        if summary != row["frame"]:
            raise ValueError("timing summary disagrees")
        rows.append({"index": row["index"], "summary": summary, "frame_ms": list(frames["frame_ms"]),
            "peak_rss_bytes": row["process"]["process"]["peak_rss_bytes"],
            "raw_sha256": row["raw_sha256"], "log_sha256": row["log_sha256"],
            "intent_sha256_reobserved": sha256_file(directory / "intent.json")})
    if {r["index"] for r in rows} != set(range(7)):
        raise ValueError("repeat identity duplicate")
    return {"count": count, "source": data["source"], "backend": backend, "executable_sha256": sha256_file(exe),
            "asset_sha256": witness["asset_sha256"], "suite_sha256": sha256_file(path),
            "witness_sha256": data["witness_sha256"], "witness_hashes": hashes, "runs": rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("exe", "asset", "packer", "out", "fox100", "fox500"):
        parser.add_argument("--"+key, type=Path, required=True)
    args = parser.parse_args()
    matrix = [load(args.fox100, args.exe, args.asset, args.packer, 100),
              load(args.fox500, args.exe, args.asset, args.packer, 500)]
    if len({r["backend"] for r in matrix}) != 1 or len({r["asset_sha256"] for r in matrix}) != 1:
        raise ValueError("mixed asset/backend")
    args.out.mkdir(parents=True, exist_ok=False)
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(2, 1, figsize=(10, 7))
    for ax, entry in zip(axes, matrix):
        for row in entry["runs"]:
            ax.plot(row["frame_ms"], linewidth=.6, alpha=.6, label="Run %u" % (row["index"]+1))
        ax.set(title="%u Fox characters, normal play" % entry["count"], xlabel="Measured frame", ylabel="Frame time (ms)")
        ax.legend(ncol=4, fontsize=8); ax.grid(alpha=.2)
    fig.tight_layout(); fig.savefig(args.out / "characters.png", dpi=150); plt.close(fig)
    write(args.out / "recomputed.json", matrix)
    text = "# Imported character timing\n\n![All measured frames](characters.png)\n\n| Characters | Median run median ms | Median run p99 ms | Max ms | Peak RSS MiB |\n| --- | ---: | ---: | ---: | ---: |\n"
    for entry in matrix:
        rows = entry["runs"]
        text += f"| {entry['count']} | {statistics.median(r['summary']['frame_median_ms'] for r in rows):.4f} | {statistics.median(r['summary']['frame_p99_ms'] for r in rows):.4f} | {max(r['summary']['frame_max_ms'] for r in rows):.4f} | {max(r['peak_rss_bytes'] for r in rows)/1048576:.2f} |\n"
    text += "\nSeven repeats, 120 warm-up/600 measured frames. Normal Play, imported asset/PAK, per-character clocks and coverage/motion controls were revalidated separately. Whole Player frame includes update/render/post/composition and KPI export; delayed GPU stats are not aligned CPU/GPU pairs. No independent GPU deformation oracle or unprovided skeletal subphase timing is certified. Witness logs/intents lack original stored hashes: their current hashes are recorded as reobserved, not historical hash proof. This is not cross-engine speed comparison or final protocol completion. Binary: `"+matrix[0]["executable_sha256"]+"`.\n"
    (args.out / "REPORT.md").write_bytes(text.encode())


if __name__ == "__main__":
    main()
