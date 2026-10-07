"""Raw-derived report for this consumer's character and culling frame trials."""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import statistics
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[4]))
from tools.benchmark_suite import frame_summary, read_frames, sha256_file


def load_trials(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("status") != "PASS" or not data.get("runs"):
        raise ValueError("a completed timing suite is required")
    rows = []
    for entry in data["runs"]:
        directory = Path(entry["directory"])
        intent = json.loads((directory / "intent.json").read_text(encoding="utf-8"))
        raw = directory / "frames.csv"
        if sha256_file(raw) != entry["raw_sha256"]:
            raise ValueError("raw frame CSV changed")
        count = intent.get("measured_intervals", intent.get("samples"))
        warmup = intent.get("warmup_intervals", intent.get("warmup"))
        samples = read_frames(raw, warmup=warmup, sample_count=count)
        calculated = frame_summary(samples)
        for name in ("sample_count", "frame_median_ms", "frame_p99_ms", "frame_max_ms"):
            value = entry["frame"][name]
            if isinstance(value, bool) or not math.isfinite(value) or not math.isclose(
                    value, calculated[name], rel_tol=1e-8, abs_tol=1e-8):
                raise ValueError("reported frame summary disagrees with raw CSV")
        rows.append(dict(entry, frame=calculated, raw_file=str(raw), raw_samples=samples))
    return data, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    suites = [(path, *load_trials(path)) for path in args.suite]
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    args.out.mkdir(parents=True, exist_ok=False)
    lines = ["# Character / culling candidate evidence", "",
             "Raw frame summaries were recalculated. This supplement is not full release acceptance.", "",
             "No external-engine timing ranking is claimed. Import/startup and 120 warm-up intervals are excluded.", ""]
    provenance = []
    for index, (path, data, rows) in enumerate(suites):
        lines.extend(["## " + path.parent.name, "", str(data["scope"]).strip(), "",
                      "Count: %u. [Source suite](%s). SHA-256: `%s`." %
                      (data["count"], path.resolve().as_posix(), sha256_file(path)), ""])
        grouped = defaultdict(list)
        for row in rows:
            mode = data.get("comparison_name", "frustum") + " " + row["mode"] if "mode" in row else \
                "durable flush interval %u" % row.get("durable_flush_interval", 60)
            grouped[mode].append(row)
        figure, axes = plt.subplots(1, 2, figsize=(12, 4))
        names = list(grouped)
        for axis, metric, title in zip(axes, ("frame_median_ms", "frame_p99_ms"),
                                       ("Median frame interval", "P99 frame interval")):
            for column, name in enumerate(names):
                values = [row["frame"][metric] for row in grouped[name]]
                axis.bar(column, statistics.median(values), color=("#246fa8", "#b55f36")[column % 2])
                axis.scatter([column] * len(values), values, color="#222222", s=20, zorder=3)
            axis.set_xticks(range(len(names)), names, rotation=12)
            axis.set_ylabel("ms (raw wall clock)")
            axis.set_title(title + "; dots = all trials")
            axis.grid(axis="y", alpha=0.2)
        figure.suptitle("%u objects; each mode retains its own samples" % data["count"])
        figure.tight_layout()
        output = args.out / ("timings-%02u.png" % index)
        figure.savefig(output, dpi=150)
        figure.savefig(output.with_suffix(".svg"))
        plt.close(figure)
        lines.extend(["![Raw frame intervals](%s)" % output.name, "",
                      "| Mode | Trial | Median ms | P99 ms | Maximum ms | >16.67 ms |",
                      "| --- | ---: | ---: | ---: | ---: | ---: |"])
        for mode, members in grouped.items():
            for row in members:
                frame = row["frame"]
                lines.append("| %s | %u | %.4f | %.4f | %.4f | %u |" %
                    (mode, row["index"], frame["frame_median_ms"], frame["frame_p99_ms"],
                     frame["frame_max_ms"], frame["over_16_67_ms"]))
                provenance.append({key: value for key, value in row.items() if key != "raw_samples"})
        lines.append("")
    (args.out / "REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    (args.out / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n",
                                              encoding="utf-8", newline="\n")
    print(args.out.resolve() / "REPORT.md")


if __name__ == "__main__":
    main()
