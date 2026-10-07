#!/usr/bin/env python3
"""Recompute bounded light/particle timings; never infer cross-workload ratios."""
import argparse
import json
from pathlib import Path
import statistics
from gbuffer_asset_suite import sha256_file, write
from particle_timing_suite import correctness, timing_records, validate_process
from particle_suite import particle_asset, scene
from light_timing_suite import timing_scene, validate_inventory
from tools.benchmark_suite import read_frames, frame_summary
from tools.jce_determinism import assert_backend


def rehash(path, expected):
    if sha256_file(path) != expected:
        raise ValueError("changed input: " + str(path))


def measured(path, total):
    full = read_frames(path, warmup=0, sample_count=None)
    if len(full["frame_ms"]) != total:
        raise ValueError("incomplete frame CSV")
    frames = read_frames(path, warmup=120, sample_count=600)
    return frames, frame_summary(frames)


def load_light(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("status") != "LIGHT_COMPONENT_TIMING_OBSERVED" or data.get("repeats") != 7:
        raise ValueError("incomplete light publication")
    rehash(Path(data["exe"]), data["exe_sha256"])
    expected = {f"{kind}-{count}-{r}" for kind in ("point", "spot") for count in (0, 10, 100) for r in range(7)}
    if set(data["cases"]) != expected or data["warmup"] != 120 or data["measured_frames"] != 600:
        raise ValueError("missing light conditions")
    result = []
    for key, case in data["cases"].items():
        kind, count, repeat = key.split("-")
        directory = path.parent / key
        for name, digest in case["raw_sha256"].items():
            target = (directory / name).resolve()
            if not target.is_relative_to(directory.resolve()):
                raise ValueError("escaped artifact")
            rehash(target, digest)
        for name in ("engine.log", "scene.json", "frames.csv"):
            if name not in case["raw_sha256"]:
                raise ValueError("missing required hash")
        rehash(directory / "scene.json", case["fixture_sha256"])
        if json.loads((directory / "scene.json").read_text()) != timing_scene(kind, int(count)):
            raise ValueError("light fixture changed")
        validate_process(case["process"], data["backend"])
        log = (directory / "engine.log").read_text(encoding="utf-8")
        assert_backend(log, data["backend"])
        inventory = validate_inventory(log, kind, int(count))
        if case["inventory"] != inventory:
            raise ValueError("stored inventory disagrees")
        frames, summary = measured(directory / "frames.csv", 720)
        if summary != case["frame"]:
            raise ValueError("stored light summary disagrees")
        result.append({"condition": f"{kind}-{count}", "repeat": int(repeat), "summary": summary,
            "frame_ms": list(frames["frame_ms"]), "peak_rss_bytes": case["process"]["process"].get("peak_rss_bytes"),
            "peak_commit_bytes": case["process"]["process"].get("peak_commit_bytes")})
    return data, result


def load_particle(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("status") != "BOUNDED_PARTICLE_TIMING_PASS" or len(data.get("runs", [])) != 7:
        raise ValueError("incomplete particle publication")
    rehash(Path(data["executable"]), data["executable_sha256"])
    prerequisite = Path(data["correctness"]["path"])
    rehash(prerequisite, data["correctness"]["sha256"])
    result = []
    for index, case in enumerate(data["runs"]):
        for record in case["files"].values():
            rehash(Path(record["path"]), record["sha256"])
        asset = Path(case["files"]["fixture.particles.json"]["path"])
        generated = particle_asset(data["emitters"], mode="burst", particles=data["count"])
        actual = json.loads(asset.read_text())
        generated["sizeStart"] = generated["sizeEnd"] = actual["sizeStart"]
        if actual != generated:
            raise ValueError("particle fixture changed")
        correctness(prerequisite, data["executable_sha256"], data["backend"], data["path"],
                    data["count"], actual["sizeStart"], data["emitters"])
        if json.loads(Path(case["files"]["scene.json"]["path"]).read_text()) != scene(asset, data["emitters"], data["path"] != "cpu"):
            raise ValueError("particle scene changed")
        env = case["environment"]
        if env.get("JCE_BENCH_PARTICLE_STATS_UNTIL_FRAME") != "64" or any("SHOT" in key for key in env):
            raise ValueError("diagnostics/capture polluted timing")
        validate_process(case["process"], data["backend"])
        log = Path(case["files"]["engine.log"]["path"]).read_text(encoding="utf-8")
        assert_backend(log, data["backend"])
        witness = timing_records(log, data["emitters"], data["path"] == "cpu", data["count"])
        if witness != case["witness"]:
            raise ValueError("stored witness disagrees")
        frames, summary = measured(Path(case["files"]["frames.csv"]["path"]), 721)
        if summary != case["summary"]:
            raise ValueError("stored particle summary disagrees")
        result.append({"repeat": index, "summary": summary, "frame_ms": list(frames["frame_ms"]),
            "witness": witness, "peak_rss_bytes": case["process"]["process"].get("peak_rss_bytes"),
            "peak_commit_bytes": case["process"]["process"].get("peak_commit_bytes")})
    return data, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lights", type=Path, required=True)
    parser.add_argument("--particles", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    lights, lr = load_light(args.lights.resolve())
    particles, pr = load_particle(args.particles.resolve())
    if lights["exe_sha256"] != particles["executable_sha256"] or lights["backend"] != particles["backend"]:
        raise ValueError("mixed artifact/backend")
    args.out.mkdir(parents=True, exist_ok=False)
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    conditions = [f"{kind}-{count}" for kind in ("point", "spot") for count in (0, 10, 100)]
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.boxplot([[r["summary"]["frame_median_ms"] for r in lr if r["condition"] == c] for c in conditions], tick_labels=conditions)
    ax.set(ylabel="Per-run median frame time (ms)", title="Lights: seven independent runs per condition")
    ax.grid(axis="y", alpha=.2)
    fig.tight_layout(); fig.savefig(args.out / "lights.png", dpi=150); plt.close(fig)
    fig, axes = plt.subplots(2, 1, figsize=(10, 7))
    for run in pr:
        axes[0].plot(range(600), run["frame_ms"], alpha=.55, linewidth=.6, label="Run %u" % (run["repeat"]+1))
        ordered = sorted(run["frame_ms"])
        axes[1].plot(ordered, [(i+1)/600 for i in range(600)], linewidth=1, label="Run %u" % (run["repeat"]+1))
    axes[0].set(ylabel="Frame time (ms)", xlabel="Measured frame", title="Million CPU particles: every measured frame, spikes retained")
    axes[1].set(xlabel="Frame time (ms)", ylabel="Empirical cumulative fraction")
    for ax in axes:
        ax.grid(alpha=.2)
        ax.legend(loc="upper right", ncol=4, fontsize=8)
    fig.tight_layout(); fig.savefig(args.out / "particles.png", dpi=150); plt.close(fig)
    write(args.out / "recomputed.json", {"executable_sha256": lights["exe_sha256"], "light_source": lights["source"],
        "particle_source": particles["source"], "inputs": {str(p.resolve()): sha256_file(p) for p in (args.lights, args.particles)},
        "lights": lr, "particles": pr})
    text = "# Independently recomputed bounded component timing\n\nBinary SHA-256: `"+lights["exe_sha256"]+"`. Actual backend: "+lights["backend"]+".\n\n"
    text += "120 warm-up / 600 measured frames; seven independent runs. Original CSVs, logs, binary, fixture hashes, light inventories and particle typed counts/cutoff were revalidated. Source identity claims remain the original suite manifests.\n\n![Light run distributions](lights.png)\n\n| Condition | Median run median ms | Median run p99 ms | Max frame ms |\n| --- | ---: | ---: | ---: |\n"
    for c in conditions:
        rows = [r for r in lr if r["condition"] == c]
        text += f"| {c} | {statistics.median(r['summary']['frame_median_ms'] for r in rows):.4f} | {statistics.median(r['summary']['frame_p99_ms'] for r in rows):.4f} | {max(r['summary']['frame_max_ms'] for r in rows):.4f} |\n"
    text += "\n![Particle frame distribution](particles.png)\n\n| Particle repeat | Median ms | p99 ms | Max ms | >16.67ms frames | Peak RSS MiB | Peak commit MiB |\n| --- | ---: | ---: | ---: | ---: | ---: | ---: |\n"
    for r in pr:
        v = r["summary"]
        text += f"| {r['repeat']+1} | {v['frame_median_ms']:.4f} | {v['frame_p99_ms']:.4f} | {v['frame_max_ms']:.4f} | {v['over_16_67_ms']} | {r['peak_rss_bytes']/1048576:.2f} | {r['peak_commit_bytes']/1048576:.2f} |\n"
    text += "\nComponent inventories are not shader evaluation counters. Particle diagnostics stop before measured frames; snapshots/captures belong to separate correctness processes. Peaks and long frames are retained. Million-particle RSS does not qualify a 512-MiB device. Frame wall time includes KPI export and host overhead; asynchronous GPU counters are not exact same-frame GPU timings. Neither chart is a cross-workload or cross-engine speed comparison. This bounded burst lifetime is not 1800-second endurance; final protocol completion is not asserted.\n"
    (args.out / "REPORT.md").write_bytes(text.encode())


if __name__ == "__main__":
    main()
