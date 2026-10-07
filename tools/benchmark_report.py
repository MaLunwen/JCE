#!/usr/bin/env python3
"""Render attributable benchmark suites and separately labelled public references.

Plots never combine different suites, hosts, workloads or executable identities.
PASS summaries are recalculated from raw CSV before being displayed. Failed and
unavailable measurements remain text without a fabricated zero-valued bar.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import statistics
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.benchmark_suite import read_frames, frame_summary, sha256_file
from tools.benchmark_probe_report import load_probe_suite, probe_section


def load_suite(path):
    path = Path(path).resolve()
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("manifest", {}).get("schema") == 1:
        return load_probe_suite(path)
    if data.get("schema_version") != 2 or not isinstance(data.get("results"), list):
        raise ValueError("unsupported suite schema: %s" % path)
    rows = []
    for entry in data["results"]:
        row = dict(entry)
        row["suite_id"] = data["suite_id"]
        row["host"] = data["host"]
        if entry.get("status") != "PASS":
            row.pop("frame", None)
            rows.append(row)
            continue
        directory = Path(entry["directory"])
        manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
        workload = manifest["workload"]
        if manifest["host"] != data["host"] or manifest["engine"] != data["engine"]:
            raise ValueError("manifest host/engine disagrees with suite")
        if workload["kind"] != entry["kind"] or manifest["variant"] != entry["variant"]:
            raise ValueError("manifest workload/variant disagrees with suite")
        raw = directory / manifest["raw_file"]
        samples = read_frames(raw, warmup=workload["warmup_frames"],
                              sample_count=workload["sample_frames"])
        calculated = frame_summary(samples)
        for metric in ("sample_count", "frame_median_ms", "frame_p99_ms"):
            expected = entry["frame"][metric]
            if isinstance(expected, bool) or not math.isfinite(expected) or not math.isclose(
                    expected, calculated[metric], rel_tol=1e-8, abs_tol=1e-8):
                raise ValueError("raw CSV disagrees with summary: " + metric)
        row.update(frame=calculated, count=workload["count"],
                   executable_sha256=manifest["executable_sha256"],
                   scene_sha256=workload["scene_sha256"],
                   raw_sha256=sha256_file(raw), raw_file=str(raw),
                   manifest_sha256=sha256_file(directory / "manifest.json"))
        rows.append(row)
    return data, rows


def groups(rows):
    collected = defaultdict(list)
    for row in rows:
        if row.get("status") == "PASS":
            key = (row["suite_id"], row["host"], row["kind"], row["count"],
                   row["variant"], row["backend_actual"], row["executable_sha256"],
                   row["scene_sha256"])
            collected[key].append(row)
    return collected


def plot_suite(data, rows, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    selected = groups(rows)
    if not selected:
        return False
    labels, values, low, high = [], [], [], []
    for key, members in selected.items():
        medians = [row["frame"]["frame_median_ms"] for row in members]
        center = statistics.median(medians)
        labels.append("%s %s / %s / %s (n=%d)" %
                      (key[2], format(key[3], ",d"), key[4], key[5], len(members)))
        values.append(center)
        low.append(center - min(medians))
        high.append(max(medians) - center)
    figure, axis = plt.subplots(figsize=(10, max(3, len(labels) * 0.45 + 1.7)))
    axis.barh(labels, values, xerr=[low, high], color="#246fa8", capsize=3)
    axis.invert_yaxis()
    axis.set_xlabel("Wall-clock ms/frame; bar = median of trial medians; whiskers = trial range")
    axis.set_title("%s / %s / %s" % (data["engine"], data["host"], data["profile"]))
    axis.grid(axis="x", alpha=0.25)
    axis.set_axisbelow(True)
    figure.tight_layout()
    figure.savefig(path, dpi=150)
    figure.savefig(path.with_suffix(".svg"))
    plt.close(figure)
    return True


def reference_section(reference):
    if reference.get("schema_version") != 1:
        raise ValueError("unsupported public-reference schema")
    lines = ["## Public engine references", "",
             "Scenario and instrumentation references only. These are not local timings or a speed ranking.", "",
             "| Engine | Reference | Version/scope | Reproducible scenario |", "| --- | --- | --- | --- |"]
    for row in reference["engines"]:
        if row.get("status") != "REFERENCE_ONLY" or row.get("frame_ms") is not None:
            raise ValueError("a public reference must not masquerade as a matched timing")
        lines.append("| %s | [%s](%s) | %s | %s |" %
                     (row["engine"], row["title"], row["url"], row["scope"], row["scenario"]))
        for metric in row.get("instrumentation", []):
            lines.append("\n%s: [%s](%s). %s" %
                         (row["engine"], metric["title"], metric["url"], metric["note"]))
    columns = reference["columns"]
    lines.extend(["", "| Workload | " + " | ".join(columns) + " |",
                  "| " + " | ".join(["---"] * (len(columns) + 1)) + " |"])
    for row in reference.get("mapping", []):
        lines.append("| " + " | ".join(row[key] for key in ("workload", *columns)) + " |")
    lines.extend(["", *reference.get("limitations", [])])
    return lines


def plot_reference(reference, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.colors import ListedColormap
    codes = {"UNAVAILABLE": 0, "REFERENCE": 1, "MEASURED": 2}
    columns = reference["columns"]
    mapping = reference.get("mapping", [])
    if not mapping:
        return
    values = [[codes[row["status"][column]] for column in columns] for row in mapping]
    figure, axis = plt.subplots(figsize=(9, max(3.5, len(mapping) * 0.5 + 1.5)))
    axis.imshow(values, cmap=ListedColormap(["#dddddd", "#ffe3a0", "#a8d4e7"]),
                vmin=0, vmax=2, aspect="auto")
    axis.set_xticks(range(len(columns)), columns)
    axis.set_yticks(range(len(mapping)), [row["workload"] for row in mapping])
    for y, row in enumerate(values):
        for x, code in enumerate(row):
            axis.text(x, y, ("Unavailable", "Reference", "Measured")[code],
                      ha="center", va="center", fontsize=10)
    axis.set_title("Supplied evidence coverage; not a capability score or speed ranking")
    figure.tight_layout()
    figure.savefig(path, dpi=150)
    figure.savefig(path.with_suffix(".svg"))
    plt.close(figure)


def verify_reference_evidence(reference, suites):
    available = {(data["engine"], data["suite_id"], row["kind"])
                 for data, rows in suites for row in rows if row.get("status") == "PASS"}
    for row in reference.get("mapping", []):
        for engine in reference["columns"]:
            if row["status"][engine] == "MEASURED":
                evidence = row.get("evidence", {}).get(engine, [])
                if not evidence or any((engine, item["suite_id"], item["kind"]) not in available
                                       for item in evidence):
                    raise ValueError("coverage claims a measurement without supplied raw evidence")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", type=Path, action="append", required=True)
    parser.add_argument("--references", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    suites = [load_suite(path) for path in args.suite]
    reference = json.loads(args.references.read_text(encoding="utf-8")) if args.references else None
    reference_lines = reference_section(reference) if reference else []
    if reference:
        verify_reference_evidence(reference, suites)
    args.out.mkdir(parents=True, exist_ok=False)
    lines = ["# Benchmark candidate evidence", "",
             "This report describes supplied evidence; it is not full release acceptance.", "",
             "Each suite stays separate. Initial image liveness does not establish continuous visual fidelity.", ""]
    provenance = []
    for index, (data, rows) in enumerate(suites):
        image = args.out / ("suite-%02d.png" % index)
        if data.get("measurement_domain") == "CPU workload":
            lines.extend(probe_section(data, rows, image) + [""])
            provenance.extend(rows)
            continue
        lines.extend(["## " + data["suite_id"], "",
                      "Engine: %s. Host: %s. Profile: %s. Status: %s." %
                      (data["engine"], data["host"], data["profile"], data["status"]), ""])
        if plot_suite(data, rows, image):
            lines.extend(["![Measured trial medians](%s)" % image.name, ""])
        lines.extend(["| Run | Workload/count | Backend | Status | Median ms | P99 ms |",
                      "| --- | --- | --- | --- | ---: | ---: |"])
        for row in rows:
            frame = row.get("frame")
            metrics = ("%.4f | %.4f" % (frame["frame_median_ms"], frame["frame_p99_ms"])) if frame else "- | -"
            lines.append("| %s | %s/%s | %s | %s | %s |" %
                         (row["run_id"], row["kind"], row.get("count", "unavailable"),
                          row.get("backend_actual", "unavailable"), row["status"], metrics))
        provenance.extend(rows)
        lines.append("")
    if reference:
        plot_reference(reference, args.out / "reference-coverage.png")
        lines.extend(["![Evidence coverage](reference-coverage.png)", ""])
    lines.extend(reference_lines)
    (args.out / "REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    (args.out / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    print(args.out.resolve() / "REPORT.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
