#!/usr/bin/env python3
"""Bounded review of pinned PresentMon v2.6.0 CSV observations, not GPU utilization."""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
import math
import statistics
from pathlib import Path

TOOL_SHA256 = "b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af"
METRICS = ("MsBetweenPresents", "MsBetweenDisplayChange", "MsInPresentAPI",
           "MsCPUBusy", "MsCPUWait", "MsGPULatency", "MsGPUTime", "MsGPUBusy",
           "MsGPUWait", "MsUntilDisplayed")
REQUIRED = ("Application", "ProcessID", "SwapChainAddress", "PresentRuntime",
            "SyncInterval", "AllowsTearing", "PresentMode", "CPUStartQPCTimeInMs") + METRICS

# Native v2 column names stay distinct from legacy Present() metrics. The pinned
# CsvOutput.cpp emits CPUStartQPCTime in milliseconds only under --qpc_time_ms.
V2_METRICS = ("FrameTime", "CPUBusy", "CPUWait", "GPULatency", "GPUTime",
              "GPUBusy", "GPUWait", "DisplayLatency", "DisplayedTime")
V2_REQUIRED = REQUIRED[:7] + ("PresentFlags", "CPUStartQPCTime") + V2_METRICS
V2_DEFINITIONS = {
    "FrameTime": "CPU work start to the next frame's CPU work start",
    "CPUBusy": "CPU work start to this frame's Present() call",
    "CPUWait": "CPU waiting before starting the next frame",
    "GPULatency": "Event-derived estimate of CPU work start to the GPU starting this frame's work",
    "GPUTime": "Event-derived estimate of the GPU work interval, including GPU busy and wait",
    "GPUBusy": "Event-derived estimate of time at least one target-process GPU engine executes work",
    "GPUWait": "Event-derived estimate of GPU idle time within this frame's GPU work interval",
    "DisplayLatency": "CPU work start to this frame becoming displayed",
    "DisplayedTime": "Time this frame is displayed; NA for an undisplayed frame",
}
# Explicit opt-in only: v2.6.0 native --v1_metrics emits QPCTime in
# absolute QPC seconds under --qpc_time_ms (CsvOutput.cpp), unlike v2.
NATIVE_V1_SCHEMA = "presentmon-2.6-native-v1"
NATIVE_V1_METRICS = ("msInPresentAPI", "msBetweenPresents", "msUntilRenderComplete",
    "msUntilDisplayed", "msBetweenDisplayChange", "msFlipDelay",
    "msUntilRenderStart", "msGPUActive", "msSinceInput")
NATIVE_V1_REQUIRED = ("Application", "ProcessID", "SwapChainAddress", "Runtime",
    "SyncInterval", "PresentFlags", "Dropped", "TimeInSeconds", "msInPresentAPI",
    "msBetweenPresents", "AllowsTearing", "PresentMode", "msUntilRenderComplete",
    "msUntilDisplayed", "msBetweenDisplayChange", "msFlipDelay", "msUntilRenderStart",
    "msGPUActive", "msSinceInput", "QPCTime")
NATIVE_V1_SIGNED = ("msUntilRenderStart", "msUntilRenderComplete")
NATIVE_V1_DEFINITIONS = {
    "msInPresentAPI": "Event-derived elapsed time inside this Present() API call; not a scheduler wait attribution",
    "msBetweenPresents": "Event-derived interval between this and the preceding Present() start",
    "msUntilRenderComplete": "Signed event-derived render-ready offset from Present() start, which may be negative; not a wait duration or hardware fence observation",
    "msUntilDisplayed": "Event-derived Present() start to displayed estimate; dropped rows may contain numeric zero",
    "msBetweenDisplayChange": "Event-derived interval from the preceding display change; not optical display cadence",
    "msFlipDelay": "Upstream NVIDIA flip-delay metric, emitted without reinterpretation",
    "msUntilRenderStart": "Signed event-derived render-start offset from Present() start, which may be negative; not a wait duration",
    "msGPUActive": "Upstream msGPUDuration estimate emitted as msGPUActive; not v2 GPUTime or hardware utilization",
    "msSinceInput": "Upstream input-event age metric; not independently measured input-to-photon latency",
}
NATIVE_V1_SOURCES = (
    "https://github.com/GameTechDev/PresentMon/blob/v2.6.0/PresentMon/CsvOutput.cpp",
    "https://github.com/GameTechDev/PresentMon/blob/v2.6.0/PresentMon/OutputThread.cpp",
    "https://github.com/GameTechDev/PresentMon/blob/v2.6.0/IntelPresentMon/CommonUtilities/mc/MetricsCalculatorAnimation.cpp",
    "https://github.com/GameTechDev/PresentMon/blob/v2.6.0/IntelPresentMon/CommonUtilities/mc/MetricsCalculatorDisplay.cpp",
    "https://github.com/GameTechDev/PresentMon/blob/v2.6.0/PresentData/PresentMonTraceSession.cpp",
    "https://github.com/GameTechDev/PresentMon/blob/v2.6.0/PresentMon/CommandLine.cpp",
)
HWS_PRECISION_SOURCE = (
    "https://github.com/GameTechDev/PresentMon/blob/v2.6.0/README.md"
    "#tracking-gpu-work-with-hardware-accelerated-gpu-scheduling-enabled"
)
SOURCE_DEFINITIONS = (
    "https://github.com/GameTechDev/PresentMon/blob/v2.6.0/PresentMon/CsvOutput.cpp",
    "https://github.com/GameTechDev/PresentMon/blob/v2.6.0/README-ConsoleApplication.md",
)


def _gpu_timing_qualification(metric_columns):
    # CSV timing values, including valid zero, do not witness HWS state. Keep
    # the upstream estimates unchanged; no hardware query or correction belongs
    # to an offline CSV review.
    return {"measurement_kind": "EVENT_DERIVED_ESTIMATES",
        "source_metric_columns": [key for key in metric_columns if "GPU" in key],
        "hws_state": "unknown",
        "hws_state_scope": "This CSV review does not independently certify hardware-accelerated GPU scheduling state",
        "precision_limit": "When HWS is enabled, GPU execution timing may be delayed or inflated; the amount depends on workload and GPU",
        "correction_applied": False,
        "hardware_gpu_busy_certified": False,
        "source": HWS_PRECISION_SOURCE}


def _schema(metrics_mode):
    if metrics_mode == "legacy":
        return REQUIRED, METRICS, "CPUStartQPCTimeInMs"
    if metrics_mode == "v2":
        return V2_REQUIRED, V2_METRICS, "CPUStartQPCTime"
    if metrics_mode == "native-v1":
        return NATIVE_V1_REQUIRED, NATIVE_V1_METRICS, "QPCTime"
    raise ValueError("unknown CSV metrics mode")


def _csv_schema(metrics_mode, track_input):
    required, metrics, qpc = _schema(metrics_mode)
    if not track_input:
        required = tuple(key for key in required if key != "msSinceInput")
        metrics = tuple(key for key in metrics if key != "msSinceInput")
    return required, metrics, qpc


def _receipt_metrics_mode(receipt, command):
    v1, v2 = command.count("--v1_metrics"), command.count("--v2_metrics")
    if v1 > 1 or v2 > 1 or (v1 and v2):
        raise ValueError("conflicting or duplicate metrics mode")
    mode = "v2" if v2 else "legacy"
    if "csv_schema" in receipt:
        if receipt["csv_schema"] != NATIVE_V1_SCHEMA or v1 != 1 or v2:
            raise ValueError("unknown CSV schema or native-v1 command disagrees")
        if receipt.get("qpc_unit") != "seconds":
            raise ValueError("native-v1 requires explicit QPC seconds")
        mode = "native-v1"
    # Existing receipts need no new field: their explicit command is authority.
    # A caller's optional additional declaration must agree, never override it.
    if "metrics_mode" in receipt and receipt["metrics_mode"] != mode:
        raise ValueError("receipt metrics mode disagrees with observer command")
    return mode


def _native_v1_tracks_input(command):
    # PresentMon v2.6.0 parses these options with a following argv value.
    value_options = {"process_name", "exclude", "process_id", "etl_file", "output_file",
                     "hotkey", "delay", "timed", "session_name", "set_circular_buffer_size"}
    flag = "--no_track_input"
    indices = [index for index, token in enumerate(command[1:], 1)
               if isinstance(token, str) and token.lower().lstrip("-/") == "no_track_input"]
    if not indices:
        return True
    if len(indices) != 1 or command[indices[0]] != flag:
        raise ValueError("duplicate or noncanonical native-v1 input tracking flag")
    prior = command[indices[0] - 1]
    if indices[0] > 1 and isinstance(prior, str) and prior.lower().lstrip("-/") in value_options:
        raise ValueError("native-v1 input tracking flag is an option value")
    return False


def file_hash(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def number(value, label, *, signed=False):
    try:
        result = float(value)
    except (TypeError, ValueError) as error:
        raise ValueError("invalid " + label) from error
    if not math.isfinite(result) or (not signed and result < 0):
        raise ValueError("invalid " + label)
    return result


def review_rows(rows, pid, name, *, max_rows=250000, max_chains=8, metrics_mode="legacy",
                track_input=True):
    if type(pid) is not int or pid <= 0 or not name or max_rows < 2 or max_chains < 1:
        raise ValueError("invalid capture limits or target")
    native_v1 = metrics_mode == "native-v1"
    if type(track_input) is not bool or (not track_input and not native_v1):
        raise ValueError("input-disabled rows require native-v1")
    required, metric_columns, qpc_column = _csv_schema(metrics_mode, track_input)
    groups, count = {}, 0
    for row in rows:
        count += 1
        if count > max_rows:
            raise ValueError("capture exceeds bounded row limit; no prefix certificate")
        if None in row or any(key not in row or row[key] is None for key in required):
            raise ValueError("missing field or malformed CSV row")
        if not track_input and any(isinstance(key, str) and key.lower() == "mssinceinput" for key in row):
            raise ValueError("input column disagrees with disabled input tracking")
        if native_v1 and any(type(row[key]) is not str for key in required):
            raise ValueError("native-v1 CSV fields require strings, not coercion")
        if row["ProcessID"] != str(pid) or row["Application"] != name:
            raise ValueError("capture contains a different process")
        chain = row["SwapChainAddress"]
        if not chain or chain in ("0x0", "NA"):
            raise ValueError("missing swapchain identity")
        if chain not in groups:
            if len(groups) >= max_chains:
                raise ValueError("capture exceeds bounded swapchain limit")
            groups[chain] = {"rows": 0, "first_qpc_stamp": None, "last_qpc_stamp": None,
                "classifications": {}, "metrics": {key: [] for key in metric_columns}}
        group = groups[chain]
        stamp = number(row[qpc_column], "QPC seconds" if native_v1 else "QPC milliseconds")
        if native_v1:
            number(row["TimeInSeconds"], "session-relative Present seconds")
            if row["Dropped"] not in ("0", "1") or not row["PresentFlags"].isascii() or not row["PresentFlags"].isdigit():
                raise ValueError("invalid native-v1 dropped state or present flags")
            group["dropped_rows"] = group.get("dropped_rows", 0) + int(row["Dropped"])
        if group["last_qpc_stamp"] is not None and stamp < group["last_qpc_stamp"]:
            raise ValueError("QPC stream is reversed within swapchain")
        if group["first_qpc_stamp"] is None:
            group["first_qpc_stamp"] = stamp
        group["last_qpc_stamp"] = stamp
        group["rows"] += 1
        runtime_column = "Runtime" if native_v1 else "PresentRuntime"
        classification = tuple(row[key] for key in (runtime_column, "SyncInterval", "AllowsTearing", "PresentMode"))
        if classification[0] not in ("DXGI", "D3D9") or not classification[1].isdigit() or classification[2] not in ("0", "1") or not classification[3]:
            raise ValueError("invalid presentation classification")
        group["classifications"][classification] = group["classifications"].get(classification, 0) + 1
        if metrics_mode == "v2" and not row["PresentFlags"].isdigit():
            raise ValueError("invalid present flags")
        for key in metric_columns:
            if row[key] != "NA":
                group["metrics"][key].append(number(row[key], key, signed=native_v1 and key in NATIVE_V1_SIGNED))
    if not count:
        raise ValueError("empty capture")
    result = []
    for chain, group in groups.items():
        elapsed = (group["last_qpc_stamp"] - group["first_qpc_stamp"]) / (1 if native_v1 else 1000)
        if group["rows"] < 2 or elapsed <= 0:
            raise ValueError("no positive swapchain observation window")
        metrics = {}
        for key, values in group["metrics"].items():
            metrics[key] = {"status": "OBSERVED" if values else "UNAVAILABLE",
                "observed_rows": len(values), "unavailable_rows": group["rows"] - len(values),
                "min_ms": min(values) if values else None,
                "mean_ms": statistics.mean(values) if values else None,
                "p50_ms": percentile(values, .5) if values else None,
                "p95_ms": percentile(values, .95) if values else None,
                "p99_ms": percentile(values, .99) if values else None,
                "p999_ms": percentile(values, .999) if values else None,
                "max_ms": max(values) if values else None,
                "source_column": key, "unit": "milliseconds"}
            if metrics_mode == "v2":
                metrics[key]["definition"] = V2_DEFINITIONS[key]
            if native_v1:
                metrics[key]["definition"] = NATIVE_V1_DEFINITIONS[key]
                metrics[key]["numeric_zero_rows"] = values.count(0)
        if metrics_mode == "v2":
            # Absent legacy metrics are not aliases, estimates, or zeros. In
            # particular FrameTime is not MsBetweenPresents, and CPUWait is
            # not a substitute for either MsInPresentAPI or GPUWait.
            for key in METRICS:
                metrics[key] = {"status": "UNAVAILABLE", "reason": "NOT_EMITTED_BY_V2",
                    "observed_rows": 0, "unavailable_rows": group["rows"],
                    "min_ms": None, "mean_ms": None, "p50_ms": None, "p95_ms": None,
                    "p99_ms": None, "p999_ms": None, "max_ms": None,
                    "source_column": None, "unit": "milliseconds"}
        if native_v1:
            result.append({"swapchain": chain, "rows": group["rows"],
                "qpc_begin_s": group["first_qpc_stamp"], "qpc_end_s": group["last_qpc_stamp"],
                "observed_window_s": elapsed, "dropped_rows": group["dropped_rows"],
                "not_dropped_rows": group["rows"] - group["dropped_rows"],
                "observed_present_start_cadence_hz": (group["rows"] - 1) / elapsed,
                "cadence_source_column": qpc_column,
                "cadence_scope": "Present() start timestamp observations, including dropped rows; not CPU work start or optical display cadence",
                "classifications": [{"runtime": item[0], "sync_interval": int(item[1]),
                    "allows_tearing": item[2] == "1", "mode": item[3], "rows": total}
                    for item, total in group["classifications"].items()], "metrics": metrics})
            continue
        result.append({"swapchain": chain, "rows": group["rows"], "qpc_begin_ms": group["first_qpc_stamp"],
            "qpc_end_ms": group["last_qpc_stamp"], "observed_window_s": elapsed,
            "observed_present_cadence_hz": (group["rows"] - 1) / elapsed if metrics_mode == "legacy" else None,
            "observed_cpu_start_cadence_hz": (group["rows"] - 1) / elapsed,
            "cadence_source_column": qpc_column,
            "cadence_scope": "CPU start timestamp observations, not optical display cadence",
            "classifications": [{"runtime": item[0], "sync_interval": int(item[1]),
                "allows_tearing": item[2] == "1", "mode": item[3], "rows": total}
                for item, total in group["classifications"].items()], "metrics": metrics})
    if native_v1:
        return {"status": "PRESENTATION_ROWS_OBSERVED", "pid": pid, "application": name,
            "rows": count, "swapchains": result, "metrics_mode": metrics_mode,
            "csv_schema": NATIVE_V1_SCHEMA, "source_metric_columns": list(metric_columns),
            "qpc_source_column": qpc_column, "qpc_unit": "seconds",
            "qpc_timebase": "Absolute QPC scaled by trace frequency; Present() start, not session-relative TimeInSeconds",
            "source_definitions": list(NATIVE_V1_SOURCES),
            "gpu_timing_qualification": _gpu_timing_qualification(metric_columns),
            "display_metadata": {"status": "UNAVAILABLE", "reason": "V1_IGNORES_WRITE_DISPLAY_METADATA"},
            "unavailable_metrics": ["CPU work start", "CPU busy", "CPU wait", "GPU wait", "optical cadence"],
            "scope": "Native-v1 event-derived Present() observations only. Signed render-start and render-ready offsets are retained, never converted to wait durations. Numeric zeros are retained upstream output, including dropped/missing-event fallbacks; they do not certify zero latency, render readiness, display or GPU completion. No v2 aliases, hardware utilization, ETW loss absence, whole-lifecycle attribution or DWM causality. TimeInSeconds is session-relative and unused for the observation window."}
    return {"status": "PRESENTATION_ROWS_OBSERVED", "pid": pid, "application": name,
        "rows": count, "swapchains": result, "metrics_mode": metrics_mode,
        "source_metric_columns": list(metric_columns),
        "qpc_source_column": qpc_column, "qpc_unit": "milliseconds",
        "source_definitions": list(SOURCE_DEFINITIONS),
        "gpu_timing_qualification": _gpu_timing_qualification(metric_columns),
        "scope": "PresentMon event observations, not optical refresh, hardware utilization, exact input latency, original parity, ETW loss absence or whole-lifecycle/GPU pass attribution. GPU and Present waits are separate costs; NA stays unavailable. Only the mode-specific CPUStart QPC-ms column supplies the observation window; legacy TimeInSeconds is unused. CPU-start cadence is not Present() or display cadence; the legacy cadence key is retained for compatibility, not a hardware/display certificate."}


def _file_identity(path):
    item = Path(path).stat()
    return item.st_dev, item.st_ino, item.st_size, item.st_mtime_ns


def _review_process_binding(receipt):
    """Opt-in retired sampler metadata + retained bytes, not an OS image join."""
    if "process_binding" not in receipt:
        return None
    binding = receipt["process_binding"]
    fields = {"schema", "receipt", "sha256", "creation_time_unix_s"}
    if (type(binding) is not dict or set(binding) != fields or
            type(binding["schema"]) is not int or binding["schema"] != 1):
        raise ValueError("unknown process binding schema or fields")
    birth = binding["creation_time_unix_s"]
    if (type(birth) not in (int, float) or not 0 < birth <= (1 << 53) or
            not math.isfinite(birth)):
        raise ValueError("process binding requires an exact positive recorded birth")
    expected = binding["sha256"]
    if (type(expected) is not str or len(expected) != 64 or
            any(char not in "0123456789abcdef" for char in expected)):
        raise ValueError("invalid native process receipt digest")
    if type(binding["receipt"]) is not str or not Path(binding["receipt"]).is_absolute():
        raise ValueError("native process receipt requires an absolute path")
    native_path = Path(binding["receipt"])
    native_before = _file_identity(native_path)
    with native_path.open("rb") as stream:
        payload = stream.read(131073)
    if len(payload) > 131072 or hashlib.sha256(payload).hexdigest() != expected:
        raise ValueError("native process receipt exceeds bound or digest disagrees")
    native = json.loads(payload.decode("utf-8"))
    identity = native.get("process_identity") if type(native) is dict else None
    if (type(identity) is not dict or type(identity.get("pid")) is not int or
            identity["pid"] != receipt["pid"] or type(identity.get("creation_time_unix_s")) not in (int, float) or
            identity["creation_time_unix_s"] != birth or
            identity.get("creation_time_source") != "psutil.create_time" or
            type(native.get("returncode")) is not int or native["returncode"] != 0 or
            native.get("timed_out") is not False):
        raise ValueError("native process receipt lacks matching retired PID/birth/normal exit")
    if type(receipt["exe"]) is not str or not Path(receipt["exe"]).is_absolute():
        raise ValueError("bound executable requires an absolute retained path")
    executable = Path(receipt["exe"])
    artifact_before = _file_identity(executable)
    actual_hash = file_hash(executable)
    if actual_hash != receipt["exe_sha256"]:
        raise ValueError("actual retained executable bytes disagree")
    result = dict(schema=1, status="RETIRED_SAMPLER_BIRTH_AND_ARTIFACT_BOUND",
        pid=identity["pid"], creation_time_unix_s=birth,
        creation_time_source=identity["creation_time_source"],
        native_receipt=str(native_path.resolve()), native_receipt_sha256=expected,
        executable=str(executable.resolve()), executable_sha256=actual_hash,
        os_image_to_pid="UNAVAILABLE",
        scope="Retired sampler PID/birth metadata and current retained artifact bytes agree with the caller receipt; not live PID identity, kernel process-start coverage or independent OS image-to-PID proof")
    return result, [(native_path, native_before, expected),
                    (executable, artifact_before, actual_hash)]


def review_capture(csv_path, receipt_path, *, max_rows=250000):
    csv_path, receipt_path = Path(csv_path), Path(receipt_path)
    receipt_before = _file_identity(receipt_path)
    receipt_bytes = receipt_path.read_bytes()
    receipt = json.loads(receipt_bytes.decode("utf-8"))
    exit_codes = (receipt.get("app_returncode"), receipt.get("observer_returncode"))
    executable_hash = receipt.get("exe_sha256")
    if (receipt.get("observer_sha256") != TOOL_SHA256 or
            any(type(code) is not int or code != 0 for code in exit_codes) or
            not isinstance(executable_hash, str) or len(executable_hash) != 64 or
            any(char not in "0123456789abcdef" for char in executable_hash)):
        raise ValueError("pinned tool, executable identity or normal process exit not witnessed")
    command = receipt.get("observer_command")
    if not isinstance(command, list) or command.count("--qpc_time_ms") != 1 or "--process_id" not in command:
        raise ValueError("explicit QPC-ms and process selection required")
    if any(flag in command for flag in ("--qpc_time", "--date_time")):
        raise ValueError("conflicting timestamp units")
    metrics_mode = _receipt_metrics_mode(receipt, command)
    track_input = metrics_mode != "native-v1" or _native_v1_tracks_input(command)
    required, _, _ = _csv_schema(metrics_mode, track_input)
    try:
        if command[command.index("--process_id") + 1] != str(receipt["pid"]):
            raise ValueError("observer process selection disagrees")
        selected = Path(command[command.index("--output_file") + 1]).resolve()
    except (IndexError, KeyError) as error:
        raise ValueError("invalid observer command") from error
    if selected != csv_path.resolve():
        raise ValueError("observer output path disagrees")
    tool_path = Path(command[0])
    if file_hash(tool_path) != TOOL_SHA256:
        raise ValueError("actual observer tool bytes disagree")
    binding = _review_process_binding(receipt)
    if binding is not None:
        binding[1].append((receipt_path, receipt_before, hashlib.sha256(receipt_bytes).hexdigest()))
    before = csv_path.stat()
    digest = file_hash(csv_path)
    with csv_path.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        headers = reader.fieldnames
        if not headers or len(set(headers)) != len(headers) or any(key not in headers for key in required):
            raise ValueError("duplicate or missing CSV headers")
        if metrics_mode == "v2" and any(key in headers for key in
                ("CPUStartQPC", "CPUStartTime", "CPUStartDateTime", "CPUStartQPCTimeInMs")):
            raise ValueError("v2 CSV carries a different or ambiguous timestamp unit")
        if metrics_mode == "native-v1" and any(key in headers for key in
                ("PresentRuntime", "CPUStartQPC", "CPUStartTime", "CPUStartDateTime",
                 "CPUStartQPCTime", "CPUStartQPCTimeInMs")):
            raise ValueError("native-v1 CSV carries a different or ambiguous timestamp schema")
        report = review_rows(reader, receipt["pid"], Path(receipt["exe"]).name,
                             max_rows=max_rows, metrics_mode=metrics_mode, track_input=track_input)
    if not track_input:
        report["input_tracking"] = "DISABLED_BY_COLLECTOR"
    after = csv_path.stat()
    if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino) or file_hash(csv_path) != digest:
        raise ValueError("capture changed during review")
    if binding is not None:
        for path, identity, expected in binding[1]:
            if _file_identity(path) != identity or file_hash(path) != expected:
                raise ValueError("process binding artifact or receipt changed during review")
        report["process_binding"] = binding[0]
        report["wait_attribution"] = dict(status="UNAVAILABLE", gpu_completion_certified=False,
            missing_joins=["OS image-to-PID", "engine/observer/ETW QPC bridge",
                "Present token/swapchain lifetime", "scheduler wait/call stack", "DWM composition/display path"],
            scope="Presentation timing and retired process metadata do not attribute engine CPU waits, DWM work, cross-adapter copies or GPU completion")
    report.update(csv=str(csv_path.resolve()), csv_sha256=digest,
        receipt=str(receipt_path.resolve()), receipt_sha256=file_hash(receipt_path),
        tool_sha256=TOOL_SHA256, executable_sha256=executable_hash,
        launcher_identity_scope="external caller receipt; CSV confirms PID/name but not creation time")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--max-rows", type=int, default=250000)
    args = parser.parse_args()
    if args.out.exists():
        parser.error("fresh output required")
    try:
        report = review_capture(args.csv, args.receipt, max_rows=args.max_rows)
    except (ValueError, OSError, KeyError, TypeError, csv.Error) as error:
        report = {"status": "INVALID_OR_UNAVAILABLE_CAPTURE", "error": str(error)}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(report["status"])
    return 0 if report["status"] == "PRESENTATION_ROWS_OBSERVED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
