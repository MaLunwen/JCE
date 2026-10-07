"""Rehash startup evidence and recalculate steady frames before plotting."""
import argparse
import configparser
import json
import math
from pathlib import Path
import statistics

from report_frames import load_trials, sha256_file


def startup_rows(data, suite):
    config = suite.parent / "profile.ini"
    if sha256_file(config) != data["profile_sha256"]:
        raise ValueError("startup profile input changed")
    rows = []
    for entry in data["startup"]:
        directory = Path(entry["directory"])
        if sha256_file(directory / "frame.png") != entry["image_sha256"] or \
                sha256_file(directory / "frames.csv") != entry["raw_sha256"] or \
                sha256_file(directory / "profile.ini") != entry["profile_after_sha256"]:
            raise ValueError("startup capture, raw intervals or effective profile changed")
        intent = json.loads((directory / "intent.json").read_text(encoding="utf-8"))
        if intent["executable_sha256"] != data["executable_sha256"]:
            raise ValueError("startup executable identity differs")
        process = entry["process"]["process"]
        effective = configparser.ConfigParser()
        effective.read(directory / "profile.ini", encoding="utf-8")
        vsync = effective.getboolean("renderer", "vsync")
        if data.get("vsync", "on") != ("on" if vsync else "off"):
            raise ValueError("effective vsync disagrees with the measured profile")
        markers = process["live_markers_s"]
        ready, readback = markers["application_ready"], markers["first_gpu_readback"]
        interval = process["marker_sampling_interval_s"]
        if not process["sampling_available"] or process["returncode"] != 0 or process["timed_out"] or not all(
                isinstance(value, (int, float)) and not isinstance(value, bool) and
                math.isfinite(value) and value > 0 for value in (ready, readback, interval)) or \
                not ready <= readback <= process["elapsed_s"]:
            raise ValueError("live startup markers are unavailable or invalid")
        if not all(isinstance(process[key], (int, float)) and math.isfinite(process[key]) and
                   process[key] > 0 for key in ("peak_rss_bytes", "peak_commit_bytes")):
            raise ValueError("startup process memory is unavailable")
        rows.append({"ready_s": ready, "readback_s": readback,
                     "peak_rss_mib": process["peak_rss_bytes"] / 2**20,
                     "peak_commit_mib": process["peak_commit_bytes"] / 2**20,
                     "poll_interval_s": interval, "directory": str(directory)})
    if not rows:
        raise ValueError("startup trials missing")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    suites = []
    for path in args.suite:
        data, steady = load_trials(path)
        startup = startup_rows(data, path)
        suites.append((path, data, steady, startup))
    args.out.mkdir(parents=True, exist_ok=False)
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    figure, axes = plt.subplots(1, 3, figsize=(14, 4))
    lines = ["# Empty SDK Player startup evidence", "",
             "Live readiness and completed GPU readback are sampled wall-clock markers.",
             "Cache state is uncontrolled; readback is not physical onscreen presentation.",
             "Forced low policy on a modern GPU is not old-device compatibility proof.",
             "Peak process commit is distinct from resident working set and excludes GPU residency.",
             "Steady frames exclude readback. Compare throughput only with matching vsync settings.", "",
             "| Profile | Vsync | Startup / steady trials | Ready median (s) | Readback median (s) | Peak RSS median / max (MiB) | Peak commit median / max (MiB) | Steady median (ms) |",
             "|---|---|---:|---:|---:|---:|---:|---:|"]
    provenance, labels = [], []
    for index, (path, data, steady, startup) in enumerate(suites):
        # Earlier isolated runs used the unchanged engine default vsync=true.
        vsync = data.get("vsync", "on (effective INI)")
        label = data["profile"] + " / " + vsync
        labels.append(label)
        median = lambda key: statistics.median(row[key] for row in startup)
        frame = statistics.median(row["frame"]["frame_median_ms"] for row in steady)
        lines.append("| %s | %s | %u / %u | %.4f | %.4f | %.2f / %.2f | %.2f / %.2f | %.5f |" %
            (data["profile"], vsync, len(startup), len(steady), median("ready_s"), median("readback_s"),
             median("peak_rss_mib"), max(row["peak_rss_mib"] for row in startup),
             median("peak_commit_mib"), max(row["peak_commit_mib"] for row in startup), frame))
        for offset, key, color in ((-0.15, "ready_s", "#2878a0"), (0.15, "readback_s", "#e39d2c")):
            axes[0].scatter([index + offset] * len(startup), [row[key] for row in startup],
                            color=color, s=15, label=key if index == 0 else None)
        for offset, key, color in ((-0.15, "peak_rss_mib", "#2878a0"), (0.15, "peak_commit_mib", "#e39d2c")):
            axes[1].scatter([index + offset] * len(startup), [row[key] for row in startup],
                            color=color, s=15, label=key if index == 0 else None)
        axes[2].scatter([index] * len(steady), [row["frame"]["frame_median_ms"] for row in steady], color="#2878a0")
        provenance.append({"suite": str(path.resolve()), "sha256": sha256_file(path),
                           "binary_sha256": data["executable_sha256"], "startup": startup,
                           "steady_raw": [{"path": row["raw_file"], "sha256": row["raw_sha256"]} for row in steady]})
    for axis, title, unit in zip(axes, ("Live startup markers", "Peak OS process memory", "Steady trial medians"), ("Seconds", "MiB", "Milliseconds")):
        axis.set_title(title)
        axis.set_ylabel(unit)
        axis.set_xticks(range(len(labels)), labels, rotation=15)
        axis.grid(axis="y", alpha=0.25)
        axis.set_ylim(bottom=0)
    axes[0].legend()
    axes[1].legend()
    figure.tight_layout()
    for extension in ("png", "svg"):
        figure.savefig(args.out / ("startup." + extension), dpi=160)
    plt.close(figure)
    lines.extend(["", "![All trials, including outliers](startup.png)", "",
                  "All trial medians are shown; no outlier was removed. Marker polling resolution is retained.", ""])
    for path, data, _, _ in suites:
        lines.append("- [%s](%s), executable `%s`, %u bytes." %
                     (data["profile"], path.resolve().as_posix(), data["executable_sha256"], data["package_bytes"]))
    (args.out / "REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    (args.out / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
