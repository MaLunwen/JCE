"""Revalidate retained native playback and muted device clocks before plotting."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import statistics

from media_suite import validate
from culling_suite import sha256_file


def plot_resume(groups, seconds, out, plt):
    figure, axes = plt.subplots(1, len(groups), figsize=(5 * len(groups), 4), squeeze=False)
    lines = ["", "## Playback after exact seek", "",
             "A fresh muted device voice starts after current-generation picture and PCM readiness.",
             "These 50 ms samples measure %u seconds after the seek; they exclude seek readiness time." % seconds,
             "All trials are retained. PCM consumption is not acoustic output latency.", "",
             "| Asset | Trials | Max picture / transport lag (ms) | Max consumed PCM / transport lag (ms) | Max wall / transport lag (ms) |",
             "|---|---:|---:|---:|---:|"]
    for index, (name, entries) in enumerate(groups.items()):
        maxima = [1000 * max(observed[key] for _, observed in entries) for key in
                  ("resume_max_picture_lag_s", "resume_max_audio_lag_s", "resume_max_wall_lag_s")]
        lines.append("| %s | %u | %.2f | %.2f | %.2f |" % (name, len(entries), *maxima))
        axis = axes[0][index]
        for trial, (_, observed) in enumerate(entries):
            samples = observed["resume_timeline"]
            for key, color in (("picture", "#2878a0"), ("audio", "#e39d2c")):
                axis.plot([sample["wall"] for sample in samples],
                          [(sample[key] - sample["media"]) * 1000 for sample in samples],
                          color=color, alpha=0.6, label=key if trial == 0 else None)
        axis.set_title(name, fontsize=9)
        axis.set_xlabel("Wall time after resumed voice (s)")
        axis.set_ylabel("Clock minus transport (ms)")
        axis.grid(alpha=0.25)
        axis.legend()
    figure.tight_layout()
    for extension in ("png", "svg"):
        figure.savefig(out / ("resume." + extension), dpi=160)
    plt.close(figure)
    return lines + ["", "![All post-seek resumed device trials](resume.png)", ""]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    data = json.loads(args.suite.read_text(encoding="utf-8"))
    if data["status"] != "PASS" or not data["runs"]:
        raise ValueError("completed media evidence required")
    groups, provenance = defaultdict(list), []
    for row in data["runs"]:
        source, output = Path(row["file"]), Path(row["process"]["stdout_path"])
        text = output.read_text(encoding="utf-8", errors="replace")
        if row["status"] != "PASS" or row["process"]["returncode"] != 0 or row["process"]["timed_out"] or \
                sha256_file(source) != row["asset_sha256"] or text != row["process"]["stdout"]:
            raise ValueError("asset/probe output identity changed or failed")
        observed = validate(text, data["seconds"], data["tick_ms"], data["audio_device"], data.get("resume_seconds", 0))
        groups[source.name].append((row, observed))
        provenance.append({"input": str(source), "input_sha256": row["asset_sha256"],
                           "stdout": str(output), "stdout_sha256": sha256_file(output),
                           "observations": observed})
    args.out.mkdir(parents=True, exist_ok=False)
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    figure, axes = plt.subplots(1, len(groups) + 1, figsize=(5 * (len(groups) + 1), 4))
    lines = ["# Native video / muted-device cadence evidence", "", data["scope"], "",
             "Requested initial preview max dimension: %s." %
             (str(data["preview"]) if data["preview"] else "native resolution"),
             "Preview probes restore native resolution before resumed playback; peak RSS includes restoration.",
             "Raw probe logs and input asset hashes were rechecked. No external-engine ranking is claimed.",
             "This does not measure GPU upload, editor preview or acoustic speaker latency.",
             "Exact seek blocks clock advancement until current-generation picture and PCM are ready.",
             "Seek-picture error must fit one nominal frame period plus 2 ms; VFR needs a per-frame oracle.", "",
             "| Asset | Trials | Max picture / transport lag (ms) | Max wall / transport lag (ms) | Max consumed PCM / transport lag (ms) | Seek readiness median / max (ms) | Peak RSS max (MiB) |",
             "|---|---:|---:|---:|---:|---:|---:|"]
    for index, (name, entries) in enumerate(groups.items()):
        times = [observed["seek_ready_ms"] for _, observed in entries]
        picture = max(observed["max_picture_lag_s"] for _, observed in entries) * 1000
        wall = max(observed["max_wall_lag_s"] for _, observed in entries) * 1000
        audio_values = [observed["max_audio_consumption_lag_s"] for _, observed in entries]
        audio = max(audio_values) * 1000 if all(value is not None for value in audio_values) else None
        peak = max(row["process"]["peak_rss_bytes"] for row, _ in entries) / 2**20
        lines.append("| %s | %u | %.2f | %.2f | %s | %.1f / %u | %.2f |" %
                     (name, len(entries), picture, wall, "%.2f" % audio if audio is not None else "unavailable",
                      statistics.median(times), max(times), peak))
        for trial, (_, observed) in enumerate(entries):
            samples = [sample for sample in observed["timeline"] if sample["wall"] < 2 * observed["seek_target_s"] - 0.25]
            for key, color in (("picture", "#2878a0"), ("audio", "#e39d2c")):
                if key == "audio" and not data["audio_device"]:
                    continue
                axes[index].plot([sample["wall"] for sample in samples],
                                 [(sample[key] - sample["media"]) * 1000 for sample in samples],
                                 color=color, alpha=0.6, label=key if trial == 0 else None)
        axes[index].set_title(name, fontsize=9)
        axes[index].set_xlabel("Playback wall time (s)")
        axes[index].set_ylabel("Clock minus transport (ms)")
        axes[index].grid(alpha=0.25)
        axes[index].legend()
        axes[-1].scatter([index] * len(times), times, color="#2878a0")
    axes[-1].set_title("Exact seek readiness: all trials")
    axes[-1].set_ylabel("Milliseconds")
    axes[-1].set_xticks(range(len(groups)), [name.split(".")[0] for name in groups], rotation=15, fontsize=8)
    axes[-1].set_ylim(bottom=0)
    axes[-1].grid(axis="y", alpha=0.25)
    figure.tight_layout()
    for extension in ("png", "svg"):
        figure.savefig(args.out / ("media." + extension), dpi=160)
    plt.close(figure)
    lines.extend(["", "![All native playback and seek trials](media.png)", "",
                  "Clock plots end before EOF; an ended clip holding its last picture is not a slowdown.",
                  "All seek samples are shown. Millisecond clocks and one-second logging cadence limit precision."])
    if data.get("resume_seconds", 0):
        lines.extend(plot_resume(groups, data["resume_seconds"], args.out, plt))
    lines.extend(["",
                  "Source suite: [%s](%s), SHA-256 `%s`." %
                  (args.suite.name, args.suite.resolve().as_posix(), sha256_file(args.suite)),
                  "Executable SHA-256: `%s`." % data["executable_sha256"], ""])
    lines.extend("- " + value for value in data["limitations"])
    (args.out / "REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    (args.out / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
