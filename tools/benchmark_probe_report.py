"""Raw CPU-workload evidence adapter for the shared benchmark report.

No scenario names, counts, engine defaults or user-project paths live here.
Integer clocks, completed work and manifest identities are revalidated before
scoped workload timings become plots. They never become frame timings.
"""
from collections import defaultdict
import csv
import json
import math
from pathlib import Path
import statistics

from tools.benchmark_suite import sha256_file


def load_probe_suite(path):
    path = Path(path).resolve()
    data = json.loads(path.read_text(encoding="utf-8"))
    manifest = data.get("manifest", {})
    if manifest.get("schema") != 1 or not isinstance(data.get("results"), list):
        raise ValueError("unsupported CPU-workload suite schema")
    normalized = dict(data, suite_id=manifest["suite_id"], host=manifest["host"],
                      engine=manifest.get("engine", "unavailable"), profile="CPU workload",
                      measurement_domain="CPU workload")
    rows = []
    for entry in data["results"]:
        row = dict(entry, suite_id=manifest["suite_id"], host=manifest["host"],
                   kind=entry["probe"], run_id=Path(entry["directory"]).name)
        row.pop("samples", None)
        if entry.get("status") != "PASS":
            row.pop("median_ms", None)
            row.pop("p99_ms", None)
            rows.append(row)
            continue
        raw = Path(entry["directory"]) / "samples.csv"
        source_manifest = raw.parent.parent / "manifest.json"
        if json.loads(source_manifest.read_text(encoding="utf-8")) != manifest:
            raise ValueError("CPU source manifest disagrees with suite")
        if manifest["rounds"] < 1 or isinstance(manifest["rounds"], bool):
            raise ValueError("CPU sample count must be a positive integer")
        values, frequencies, indices = [], set(), []
        with raw.open(encoding="utf-8", newline="") as stream:
            for sample in csv.DictReader(stream):
                for key in ("count", "concurrency", "depth"):
                    if int(sample[key]) != entry[key]:
                        raise ValueError("CPU workload differs from requested settings")
                if (sample["probe"] != entry["probe"] or int(sample["correct"]) != 1 or
                        int(sample["completed"]) != entry["count"]):
                    raise ValueError("CPU workload did not complete correctly")
                ticks, frequency = int(sample["ticks"]), int(sample["frequency"])
                if ticks <= 0 or frequency <= 0:
                    raise ValueError("CPU workload lacks a valid monotonic interval")
                elapsed = ticks * 1000 / frequency
                reported = float(sample["ms"])
                if not math.isfinite(reported) or not math.isclose(reported, elapsed, rel_tol=1e-8):
                    raise ValueError("CPU elapsed time disagrees with integer clocks")
                values.append(elapsed)
                frequencies.add(frequency)
                indices.append(int(sample["round"]))
        if indices != list(range(manifest["rounds"])) or len(frequencies) != 1:
            raise ValueError("CPU samples have missing/reordered rounds or changed clocks")
        med = statistics.median(values)
        p99 = sorted(values)[math.ceil(len(values) * 0.99) - 1]
        for key, calculated in (("median_ms", med), ("p99_ms", p99)):
            if isinstance(entry[key], bool) or not math.isfinite(entry[key]) or not math.isclose(
                    entry[key], calculated, abs_tol=1e-8, rel_tol=1e-8):
                raise ValueError("CPU raw CSV disagrees with " + key)
        row.update(median_ms=med, p99_ms=p99, raw_sha256=sha256_file(raw), raw_file=str(raw),
                   executable_sha256=manifest["executable_sha256"],
                   manifest_sha256=sha256_file(source_manifest),
                   timing_scope=entry.get("timing_scope", manifest["timing_scope"]))
        rows.append(row)
    return normalized, rows


def plot_probes(data, rows, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    grouped = defaultdict(list)
    for row in rows:
        if row["status"] == "PASS":
            key = (row["kind"], row["count"], row["concurrency"], row["depth"],
                   row["executable_sha256"], row["timing_scope"])
            grouped[key].append(row["median_ms"])
    if not grouped:
        return False
    labels, medians, low, high = [], [], [], []
    for key, values in grouped.items():
        median = statistics.median(values)
        labels.append("%s / %s / c%d d%d (n=%d)" %
                      (key[0], format(key[1], ",d"), key[2], key[3], len(values)))
        medians.append(median)
        low.append(median - min(values))
        high.append(max(values) - median)
    fig, axis = plt.subplots(figsize=(11, max(3, len(labels) * 0.45 + 1.7)))
    axis.barh(labels, medians, xerr=[low, high], color="#648536", capsize=3)
    axis.invert_yaxis()
    axis.set_xlabel("Wall-clock ms per workload batch; median of trials, whiskers = trial range")
    axis.set_title("%s / %s: separate CPU workloads, not frame timing" % (data["engine"], data["host"]))
    axis.grid(axis="x", alpha=0.25)
    axis.set_axisbelow(True)
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    fig.savefig(path.with_suffix(".svg"))
    plt.close(fig)
    return True


def probe_section(data, rows, image):
    lines = ["## " + data["suite_id"], "",
             "%s / %s / CPU workload. Status: %s." % (data["engine"], data["host"], data["status"]), ""]
    if plot_probes(data, rows, image):
        lines.extend(["![Scoped CPU workloads](%s)" % image.name, ""])
    lines.extend(["| Run | Workload/count | Status | Median ms | P99 ms | Scope |",
                  "| --- | --- | --- | ---: | ---: | --- |"])
    for row in rows:
        metrics = "%.4f | %.4f" % (row["median_ms"], row["p99_ms"]) if row["status"] == "PASS" else "- | -"
        lines.append("| %s | %s/%d | %s | %s | %s |" %
                     (row["run_id"], row["kind"], row["count"], row["status"], metrics,
                      row.get("timing_scope", "unavailable")))
    return lines
