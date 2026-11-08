"""Actual bounded PresentMon parser controls; no OS/ETW or GPU claim."""
import copy
import csv
import json
import io
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from presentation_report import (METRICS, REQUIRED, V2_METRICS, V2_REQUIRED, TOOL_SHA256,
    NATIVE_V1_SCHEMA, NATIVE_V1_REQUIRED, NATIVE_V1_METRICS, review_rows, review_capture)


def row(stamp):
    result = {key: "2.0" for key in METRICS}
    result.update(Application="app.exe", ProcessID="42", SwapChainAddress="0x123",
        PresentRuntime="DXGI", SyncInterval="1", AllowsTearing="0",
        PresentMode="Hardware Composed: Independent Flip", CPUStartQPCTimeInMs=str(stamp),
        TimeInSeconds="999999.0")
    return result


class PresentationControls(unittest.TestCase):
    def test_actual_qpc_not_legacy_header_and_percentiles(self):
        rows = [row(1000), row(1008), row(1016)]
        rows[1]["MsGPUBusy"] = "4"; rows[2]["MsGPUBusy"] = "6"
        report = review_rows(rows, 42, "app.exe")
        group = report["swapchains"][0]
        self.assertEqual(group["observed_present_cadence_hz"], 125)
        self.assertEqual(group["observed_window_s"], .016)
        self.assertEqual(group["metrics"]["MsGPUBusy"]["p50_ms"], 4)
        self.assertAlmostEqual(group["metrics"]["MsGPUBusy"]["p95_ms"], 5.8)
        self.assertEqual(group["metrics"]["MsGPUBusy"]["mean_ms"], 4)
        self.assertAlmostEqual(group["metrics"]["MsGPUBusy"]["p99_ms"], 5.96)
        self.assertAlmostEqual(group["metrics"]["MsGPUBusy"]["p999_ms"], 5.996)
        self.assertIn("not optical", report["scope"])

    def test_unavailable_is_never_zero(self):
        rows = [row(1000), row(1008)]
        for item in rows: item["MsUntilDisplayed"] = "NA"
        metric = review_rows(rows, 42, "app.exe")["swapchains"][0]["metrics"]["MsUntilDisplayed"]
        self.assertEqual(metric["status"], "UNAVAILABLE")
        self.assertIsNone(metric["p50_ms"])
        for field in ("mean_ms", "p99_ms", "p999_ms"):
            self.assertIsNone(metric[field])
        self.assertEqual(metric["unavailable_rows"], 2)

    def test_other_process_name_or_pid_refused(self):
        for key, value in (("Application", "other.exe"), ("ProcessID", "43")):
            rows = [row(1000), row(1008)]; rows[1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): review_rows(rows, 42, "app.exe")

    def test_nonfinite_negative_missing_and_wrong_width_refused(self):
        for value in ("nan", "inf", "-1", "", None):
            rows = [row(1000), row(1008)]; rows[1]["MsGPUBusy"] = value
            with self.subTest(value=value), self.assertRaises(ValueError): review_rows(rows, 42, "app.exe")
        for mutation in (lambda item: item.pop("MsCPUBusy"), lambda item: item.update({None: ["extra"]})):
            rows = [row(1000), row(1008)]; mutation(rows[1])
            with self.assertRaises(ValueError): review_rows(rows, 42, "app.exe")

    def test_reversed_and_empty_window_refused(self):
        for rows in ([], [row(1000)], [row(1000), row(999)], [row(1000), row(1000)]):
            with self.subTest(rows=len(rows)), self.assertRaises(ValueError): review_rows(rows, 42, "app.exe")

    def test_multiple_swapchains_are_kept_separate(self):
        rows = [row(1000), row(1004), row(1008), row(1016)]
        rows[1]["SwapChainAddress"] = rows[3]["SwapChainAddress"] = "0x456"
        groups = review_rows(rows, 42, "app.exe")["swapchains"]
        self.assertEqual([group["rows"] for group in groups], [2, 2])
        self.assertEqual([group["observed_window_s"] for group in groups], [.008, .012])
        with self.assertRaises(ValueError): review_rows(rows, 42, "app.exe", max_chains=1)

    def test_limits_refuse_prefix(self):
        with self.assertRaises(ValueError): review_rows([row(1), row(2), row(3)], 42, "app.exe", max_rows=2)
        for pid in (True, 0, -1):
            with self.assertRaises(ValueError): review_rows([row(1), row(2)], pid, "app.exe")

    def fixture(self, directory):
        directory = Path(directory); capture = directory/"presents.csv"; receipt_path=directory/"receipt.json"
        with capture.open("w", newline="", encoding="utf-8") as stream:
            writer=csv.DictWriter(stream, fieldnames=REQUIRED+("TimeInSeconds",))
            writer.writeheader(); writer.writerows([row(1000), row(1008)])
        receipt={"pid":42,"exe":"app.exe","exe_sha256":"a"*64,
            "observer_sha256":TOOL_SHA256,"app_returncode":0,"observer_returncode":0,
            "observer_command":[str(directory/"tool.exe"),"--process_id","42","--qpc_time_ms","--output_file",str(capture)]}
        receipt_path.write_text(json.dumps(receipt),encoding="utf-8")
        return capture, receipt_path, receipt

    def test_actual_file_parser_and_pinned_tool_port(self):
        with tempfile.TemporaryDirectory() as directory:
            capture, receipt_path, _=self.fixture(directory)
            from presentation_report import file_hash
            def hash_port(path): return TOOL_SHA256 if Path(path).name=="tool.exe" else file_hash(path)
            with patch("presentation_report.file_hash",side_effect=hash_port):
                report=review_capture(capture, receipt_path)
            self.assertEqual(report["rows"],2)
            self.assertEqual(len(report["csv_sha256"]),64)

    def test_receipt_admission_negative_controls(self):
        with tempfile.TemporaryDirectory() as directory:
            capture, receipt_path, good=self.fixture(directory)
            mutations=(lambda data: data.update(observer_sha256="bad"),
                lambda data: data.update(app_returncode=1),
                lambda data: data.update(app_returncode=False),
                lambda data: data.update(exe_sha256="bad"),
                lambda data: data.update(observer_returncode=2),
                lambda data: data["observer_command"].remove("--qpc_time_ms"),
                lambda data: data.update(pid=43),
                lambda data: data["observer_command"].__setitem__(-1,str(Path(directory)/"other.csv")))
            for mutate in mutations:
                bad=copy.deepcopy(good);mutate(bad);receipt_path.write_text(json.dumps(bad),encoding="utf-8")
                with self.subTest(receipt=bad), self.assertRaises(ValueError): review_capture(capture,receipt_path)

    def test_actual_tool_bytes_and_duplicate_headers_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            capture, receipt_path, _=self.fixture(directory)
            (Path(directory)/"tool.exe").write_bytes(b"wrong bytes")
            with self.assertRaises(ValueError): review_capture(capture,receipt_path)
            capture.write_text("Application,Application\napp.exe,app.exe\n",encoding="utf-8")
            with patch("presentation_report.file_hash",return_value=TOOL_SHA256),self.assertRaises(ValueError):
                review_capture(capture,receipt_path)



def v2_row(stamp):
    result = {key: "2.0" for key in V2_METRICS}
    result.update(Application="app.exe", ProcessID="42", SwapChainAddress="0x123",
        PresentRuntime="DXGI", SyncInterval="0", PresentFlags="512", AllowsTearing="1",
        PresentMode="Hardware Composed: Independent Flip", CPUStartQPCTime=str(stamp),
        FrameTime="8.0", CPUBusy="0.25", CPUWait="7.75", GPUWait="0.0")
    return result


class GpuTimingQualificationControls(unittest.TestCase):
    def test_both_modes_preserve_estimates_na_units_and_unknown_hws(self):
        source_url = ("https://github.com/GameTechDev/PresentMon/blob/v2.6.0/README.md"
            "#tracking-gpu-work-with-hardware-accelerated-gpu-scheduling-enabled")
        for mode, factory, prefix, columns in (
                ("legacy", row, "Ms", METRICS), ("v2", v2_row, "", V2_METRICS)):
            with self.subTest(mode=mode):
                rows = [factory(1000), factory(1008), factory(1016)]
                busy, latency = prefix+"GPUBusy", prefix+"GPULatency"
                for item, value in zip(rows, ("0", "4", "NA")):
                    item[busy] = value
                    item[latency] = "NA"
                    # Neither a timing value nor an unrelated CSV label is a
                    # checked witness of machine state or a cross-adapter copy.
                    item["HWS"] = "enabled"
                    item["HybridPresent"] = "0"
                original = copy.deepcopy(rows)
                report = review_rows(rows, 42, "app.exe", metrics_mode=mode)
                metrics = report["swapchains"][0]["metrics"]
                self.assertEqual(rows, original)
                self.assertEqual(report["status"], "PRESENTATION_ROWS_OBSERVED")
                self.assertEqual(metrics[busy]["status"], "OBSERVED")
                self.assertEqual(metrics[busy]["source_column"], busy)
                self.assertEqual(metrics[busy]["unit"], "milliseconds")
                self.assertEqual(metrics[busy]["observed_rows"], 2)
                self.assertEqual(metrics[busy]["unavailable_rows"], 1)
                self.assertEqual(metrics[busy]["min_ms"], 0)
                self.assertEqual(metrics[busy]["max_ms"], 4)
                self.assertEqual(metrics[busy]["mean_ms"], 2)
                self.assertEqual(metrics[busy]["p50_ms"], 2)
                self.assertAlmostEqual(metrics[busy]["p95_ms"], 3.8)
                self.assertAlmostEqual(metrics[busy]["p99_ms"], 3.96)
                self.assertAlmostEqual(metrics[busy]["p999_ms"], 3.996)
                self.assertEqual(metrics[latency]["status"], "UNAVAILABLE")
                self.assertEqual(metrics[latency]["unavailable_rows"], 3)
                for field in ("min_ms", "max_ms", "mean_ms", "p50_ms", "p95_ms", "p99_ms", "p999_ms"):
                    self.assertIsNone(metrics[latency][field])
                self.assertEqual(metrics[latency]["unit"], "milliseconds")
                if mode == "v2":
                    for column in ("GPULatency", "GPUTime", "GPUBusy", "GPUWait"):
                        self.assertIn("Event-derived estimate", metrics[column]["definition"])
                qualification = report["gpu_timing_qualification"]
                self.assertEqual(qualification["measurement_kind"], "EVENT_DERIVED_ESTIMATES")
                self.assertEqual(qualification["source_metric_columns"],
                    [key for key in columns if "GPU" in key])
                self.assertEqual(qualification["hws_state"], "unknown")
                self.assertIn("does not independently certify", qualification["hws_state_scope"])
                self.assertIn("delayed or inflated", qualification["precision_limit"])
                self.assertFalse(qualification["correction_applied"])
                self.assertFalse(qualification["hardware_gpu_busy_certified"])
                self.assertEqual(qualification["source"], source_url)
                self.assertNotIn("HybridPresent", report)

    def test_default_report_always_declares_gpu_timing_boundary(self):
        report = review_rows([row(1000), row(1008)], 42, "app.exe")
        qualification = report["gpu_timing_qualification"]
        self.assertEqual(report["metrics_mode"], "legacy")
        self.assertEqual(qualification["hws_state"], "unknown")
        self.assertEqual(qualification["measurement_kind"], "EVENT_DERIVED_ESTIMATES")
        self.assertFalse(qualification["hardware_gpu_busy_certified"])


class ModernPresentationControls(unittest.TestCase):
    def fixture(self, directory):
        directory = Path(directory)
        capture, receipt_path = directory/"v2.csv", directory/"receipt.json"
        with capture.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=V2_REQUIRED)
            writer.writeheader(); writer.writerows([v2_row(1000), v2_row(1008)])
        receipt = {"pid": 42, "exe": "app.exe", "exe_sha256": "a"*64,
            "observer_sha256": TOOL_SHA256, "app_returncode": 0, "observer_returncode": 0,
            "observer_command": [str(directory/"tool.exe"), "--process_id", "42",
                "--qpc_time_ms", "--v2_metrics", "--output_file", str(capture)]}
        receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
        return capture, receipt_path, receipt

    @staticmethod
    def hash_port(owner):
        actual = owner.file_hash
        return lambda path: TOOL_SHA256 if Path(path).name == "tool.exe" else actual(path)

    def test_modern_qpc_ms_source_columns_and_distinct_semantics(self):
        report = review_rows([v2_row(1000), v2_row(1008)], 42, "app.exe", metrics_mode="v2")
        chain = report["swapchains"][0]
        self.assertEqual(report["qpc_source_column"], "CPUStartQPCTime")
        self.assertEqual(report["qpc_unit"], "milliseconds")
        self.assertEqual(report["source_metric_columns"], list(V2_METRICS))
        self.assertEqual(chain["observed_window_s"], .008)
        self.assertEqual(chain["observed_cpu_start_cadence_hz"], 125)
        self.assertIsNone(chain["observed_present_cadence_hz"])
        for column in V2_METRICS:
            self.assertEqual(chain["metrics"][column]["source_column"], column)
            self.assertEqual(chain["metrics"][column]["unit"], "milliseconds")
        self.assertIn("next frame", chain["metrics"]["FrameTime"]["definition"])
        self.assertEqual(chain["metrics"]["CPUWait"]["p50_ms"], 7.75)
        self.assertEqual(chain["metrics"]["GPUWait"]["p50_ms"], 0)

    def test_absent_legacy_metrics_are_unavailable_not_v2_aliases(self):
        metrics = review_rows([v2_row(1000), v2_row(1008)], 42, "app.exe", metrics_mode="v2")["swapchains"][0]["metrics"]
        for column in METRICS:
            self.assertEqual(metrics[column]["status"], "UNAVAILABLE")
            self.assertEqual(metrics[column]["reason"], "NOT_EMITTED_BY_V2")
            self.assertIsNone(metrics[column]["source_column"])
            self.assertIsNone(metrics[column]["p50_ms"])
            for field in ("mean_ms", "p99_ms", "p999_ms"):
                self.assertIsNone(metrics[column][field])
            self.assertEqual(metrics[column]["observed_rows"], 0)
            self.assertEqual(metrics[column]["unavailable_rows"], 2)
        self.assertEqual(metrics["FrameTime"]["p50_ms"], 8)

    def test_modern_na_and_valid_zero_remain_distinct(self):
        rows = [v2_row(1000), v2_row(1008)]
        for item in rows: item["DisplayedTime"] = item["DisplayLatency"] = "NA"
        rows[1]["GPUBusy"] = "0.0"
        metrics = review_rows(rows, 42, "app.exe", metrics_mode="v2")["swapchains"][0]["metrics"]
        self.assertEqual(metrics["DisplayedTime"]["status"], "UNAVAILABLE")
        self.assertIsNone(metrics["DisplayLatency"]["max_ms"])
        self.assertEqual(metrics["GPUBusy"]["min_ms"], 0)
        self.assertEqual(metrics["GPUBusy"]["mean_ms"], 1)
        self.assertAlmostEqual(metrics["GPUBusy"]["p95_ms"], 1.9)
        self.assertAlmostEqual(metrics["GPUBusy"]["p99_ms"], 1.98)
        self.assertAlmostEqual(metrics["GPUBusy"]["p999_ms"], 1.998)
        for field in ("mean_ms", "p99_ms", "p999_ms"):
            self.assertIsNone(metrics["DisplayedTime"][field])
            self.assertEqual(metrics["GPUWait"][field], 0)
        self.assertEqual(metrics["GPUWait"]["observed_rows"], 2)

    def test_modern_missing_nonfinite_negative_and_width_refused(self):
        for key in V2_METRICS:
            for value in (None, "", "-1", "nan", "inf"):
                rows = [v2_row(1000), v2_row(1008)]; rows[1][key] = value
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    review_rows(rows, 42, "app.exe", metrics_mode="v2")
        for mutate in (lambda item: item.pop("CPUBusy"), lambda item: item.update({None: ["extra"]}),
                       lambda item: item.update(PresentFlags="-1")):
            rows = [v2_row(1000), v2_row(1008)]; mutate(rows[1])
            with self.assertRaises(ValueError): review_rows(rows, 42, "app.exe", metrics_mode="v2")

    def test_modern_window_target_and_budget_guards_are_shared(self):
        for mutate in (lambda item: item.update(ProcessID="43"), lambda item: item.update(Application="other.exe"),
                       lambda item: item.update(CPUStartQPCTime="999")):
            rows = [v2_row(1000), v2_row(1008)]; mutate(rows[1])
            with self.assertRaises(ValueError): review_rows(rows, 42, "app.exe", metrics_mode="v2")
        with self.assertRaises(ValueError):
            review_rows([v2_row(1), v2_row(2), v2_row(3)], 42, "app.exe", metrics_mode="v2", max_rows=2)
        rows = [v2_row(1000), v2_row(1004), v2_row(1008), v2_row(1016)]
        rows[1]["SwapChainAddress"] = rows[3]["SwapChainAddress"] = "0x456"
        self.assertEqual([g["rows"] for g in review_rows(rows, 42, "app.exe", metrics_mode="v2")["swapchains"]], [2, 2])
        with self.assertRaises(ValueError): review_rows(rows, 42, "app.exe", metrics_mode="unknown")

    def test_actual_modern_file_receipt_and_pinned_tool_wire(self):
        import presentation_report as owner
        with tempfile.TemporaryDirectory() as directory:
            capture, receipt_path, receipt = self.fixture(directory)
            with patch.object(owner, "file_hash", side_effect=self.hash_port(owner)):
                report = review_capture(capture, receipt_path)
            self.assertEqual(report["metrics_mode"], "v2")
            self.assertEqual(report["rows"], 2)
            self.assertEqual(len(report["csv_sha256"]), 64)
            # Explicit command is sufficient for old receipts. An added mode
            # declaration must independently agree with that same command.
            receipt["metrics_mode"] = "v2"
            receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
            with patch.object(owner, "file_hash", side_effect=self.hash_port(owner)):
                self.assertEqual(review_capture(capture, receipt_path)["rows"], 2)

    def test_modern_mode_and_qpc_command_negative_controls(self):
        import presentation_report as owner
        with tempfile.TemporaryDirectory() as directory:
            capture, receipt_path, good = self.fixture(directory)
            mutations = (lambda data: data["observer_command"].remove("--v2_metrics"),
                lambda data: data["observer_command"].append("--v1_metrics"),
                lambda data: data["observer_command"].append("--v2_metrics"),
                lambda data: data["observer_command"].remove("--qpc_time_ms"),
                lambda data: data["observer_command"].append("--qpc_time"),
                lambda data: data["observer_command"].append("--date_time"),
                lambda data: data.update(metrics_mode="legacy"),
                lambda data: data.update(metrics_mode="unknown"))
            for mutate in mutations:
                bad = copy.deepcopy(good); mutate(bad)
                receipt_path.write_text(json.dumps(bad), encoding="utf-8")
                with self.subTest(receipt=bad), patch.object(owner, "file_hash", side_effect=self.hash_port(owner)), self.assertRaises(ValueError):
                    review_capture(capture, receipt_path)

    def test_modern_bad_or_ambiguous_qpc_headers_refused(self):
        import presentation_report as owner
        with tempfile.TemporaryDirectory() as directory:
            capture, receipt_path, _ = self.fixture(directory)
            good = capture.read_text(encoding="utf-8")
            for key in ("CPUStartTime", "CPUStartQPC", "CPUStartDateTime", "CPUStartQPCTimeInMs"):
                capture.write_text(good.replace("CPUStartQPCTime", key), encoding="utf-8")
                with self.subTest(key=key), patch.object(owner, "file_hash", side_effect=self.hash_port(owner)), self.assertRaises(ValueError):
                    review_capture(capture, receipt_path)
            for bad in (good.replace("GPUBusy,", "UnsupportedColumn,"),
                        good.replace("CPUBusy,", "CPUBusy,CPUBusy,")):
                capture.write_text(bad, encoding="utf-8")
                with patch.object(owner, "file_hash", side_effect=self.hash_port(owner)), self.assertRaises(ValueError):
                    review_capture(capture, receipt_path)

    def test_whole_owner_omitted_header_mode_units_fail_ordinary_assertions(self):
        source = (Path(__file__).resolve().parents[1]/"presentation_report.py").read_text(encoding="utf-8")
        variants = (
            ("header", "len(set(headers)) != len(headers)", "False"),
            ("mode", 'if "metrics_mode" in receipt and receipt["metrics_mode"] != mode:', "if False:"),
            ("qpc_unit", 'if any(flag in command for flag in ("--qpc_time", "--date_time")):', "if False:"),
        )
        for label, old, new in variants:
            self.assertEqual(source.count(old), 1)
            owner = types.ModuleType("actual_presentation_control_"+label)
            exec(compile(source.replace(old, new), str(Path(__file__).resolve().parents[1]/"presentation_report.py"), "exec"), owner.__dict__)
            with tempfile.TemporaryDirectory() as directory:
                capture, receipt_path, receipt = self.fixture(directory)
                if label == "header":
                    # The second duplicate value is valid, so DictReader itself
                    # cannot substitute for the actual duplicate-header gate.
                    with capture.open("w", newline="", encoding="utf-8") as stream:
                        writer = csv.writer(stream); writer.writerow(V2_REQUIRED+("CPUBusy",))
                        for stamp in (1000, 1008):
                            item = v2_row(stamp); writer.writerow([item[k] for k in V2_REQUIRED]+[item["CPUBusy"]])
                elif label == "mode": receipt["metrics_mode"] = "legacy"
                else: receipt["observer_command"].append("--qpc_time")
                receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
                def assert_gate():
                    with patch.object(owner, "file_hash", side_effect=self.hash_port(owner)), self.assertRaises(ValueError):
                        owner.review_capture(capture, receipt_path)
                result = unittest.TestResult()
                unittest.FunctionTestCase(assert_gate).run(result)
                with self.subTest(control=label):
                    self.assertEqual(result.testsRun, 1)
                    self.assertEqual(len(result.failures), 1)
                    self.assertEqual(len(result.errors), 0)
                    self.assertIn("ValueError not raised", result.failures[0][1])


class ProcessBindingControls(unittest.TestCase):
    def fixture(self, directory, modern=False):
        parent = ModernPresentationControls() if modern else PresentationControls()
        capture, receipt_path, receipt = parent.fixture(directory)
        executable = Path(directory)/"app.exe"
        executable.write_bytes(b"retained executable fixture")
        native_path = Path(directory)/"native-result.json"
        native = {"process_identity": {"pid": 42, "creation_time_unix_s": 100.125,
            "creation_time_source": "psutil.create_time"}, "returncode": 0, "timed_out": False}
        receipt.update(exe=str(executable.resolve()), exe_sha256=self.digest(executable),
            process_binding={"schema": 1, "receipt": str(native_path.resolve()),
                             "sha256": "", "creation_time_unix_s": 100.125})
        self.save(receipt_path, receipt, native_path, native)
        return capture, receipt_path, receipt, native_path, native

    @staticmethod
    def digest(path):
        import hashlib
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()

    def save(self, receipt_path, receipt, native_path, native):
        native_path.write_text(json.dumps(native), encoding="utf-8")
        receipt["process_binding"]["sha256"] = self.digest(native_path)
        receipt_path.write_text(json.dumps(receipt), encoding="utf-8")

    def test_both_modes_bind_retired_sampler_birth_and_actual_artifact_only(self):
        import presentation_report as owner
        for modern in (False, True):
            with self.subTest(modern=modern), tempfile.TemporaryDirectory() as directory:
                capture, receipt_path, receipt, _, _ = self.fixture(directory, modern)
                with patch.object(owner, "file_hash", side_effect=ModernPresentationControls.hash_port(owner)):
                    report = owner.review_capture(capture, receipt_path)
                binding = report["process_binding"]
                self.assertEqual(binding["schema"], 1)
                self.assertEqual(binding["status"], "RETIRED_SAMPLER_BIRTH_AND_ARTIFACT_BOUND")
                self.assertEqual(binding["creation_time_unix_s"], 100.125)
                self.assertEqual(binding["executable_sha256"], receipt["exe_sha256"])
                self.assertEqual(binding["os_image_to_pid"], "UNAVAILABLE")
                self.assertEqual(report["wait_attribution"]["status"], "UNAVAILABLE")
                self.assertFalse(report["wait_attribution"]["gpu_completion_certified"])
                receipt.pop("process_binding")
                receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
                with patch.object(owner, "file_hash", side_effect=ModernPresentationControls.hash_port(owner)):
                    legacy = owner.review_capture(capture, receipt_path)
                self.assertNotIn("process_binding", legacy)
                self.assertNotIn("wait_attribution", legacy)

    def test_binding_rejects_mismatch_nonfinite_coercion_and_unretired_receipts(self):
        import presentation_report as owner
        changes = [("schema", True), ("schema", 2), ("creation_time_unix_s", True),
            ("creation_time_unix_s", "100.125"), ("creation_time_unix_s", float("nan")),
            ("creation_time_unix_s", float("inf")), ("creation_time_unix_s", 0),
            ("creation_time_unix_s", 101.125), ("sha256", "x"*64),
            ("receipt", "relative-result.json"), ("unexpected", 1)]
        for key, value in changes:
            with self.subTest(key=key, value=value), tempfile.TemporaryDirectory() as directory:
                capture, receipt_path, receipt, _, _ = self.fixture(directory)
                receipt["process_binding"][key] = value
                receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
                with patch.object(owner, "file_hash", side_effect=ModernPresentationControls.hash_port(owner)), self.assertRaises(ValueError):
                    owner.review_capture(capture, receipt_path)
        for key, value in (("pid", True), ("pid", "42"), ("pid", 42.0), ("pid", 43),
                           ("creation_time_source", "UNAVAILABLE"), ("creation_time_unix_s", 101.125),
                           ("returncode", False), ("returncode", 0.0), ("returncode", 1),
                           ("timed_out", True), ("timed_out", 0), ("timed_out", None)):
            with self.subTest(native=key, value=value), tempfile.TemporaryDirectory() as directory:
                capture, receipt_path, receipt, native_path, native = self.fixture(directory)
                (native if key in ("returncode", "timed_out") else native["process_identity"])[key] = value
                self.save(receipt_path, receipt, native_path, native)
                with patch.object(owner, "file_hash", side_effect=ModernPresentationControls.hash_port(owner)), self.assertRaises(ValueError):
                    owner.review_capture(capture, receipt_path)

    def test_binding_receipt_artifact_tamper_and_midreview_changes_refused(self):
        import presentation_report as owner
        for changed in ("native", "artifact", "main"):
            for during_review in (False, True):
                if changed == "main" and not during_review: continue
                with self.subTest(changed=changed, during=during_review), tempfile.TemporaryDirectory() as directory:
                    capture, receipt_path, receipt, native_path, _ = self.fixture(directory)
                    selected = native_path if changed == "native" else Path(receipt["exe"]) if changed == "artifact" else receipt_path
                    def change():
                        import os
                        stamp = selected.stat(); payload = selected.read_bytes()
                        selected.write_bytes(payload[:-1]+b"!")
                        os.utime(selected, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
                    actual_rows = owner.review_rows
                    def rows_port(*args, **kwargs):
                        result = actual_rows(*args, **kwargs); change(); return result
                    if not during_review: change()
                    with patch.object(owner, "file_hash", side_effect=ModernPresentationControls.hash_port(owner)), patch.object(owner, "review_rows", side_effect=rows_port if during_review else actual_rows), self.assertRaises(ValueError):
                        owner.review_capture(capture, receipt_path)

    def test_whole_owner_disconnected_binding_and_omitted_artifact_gate_fail_ordinary_assertions(self):
        source = (Path(__file__).resolve().parents[1]/"presentation_report.py").read_text(encoding="utf-8")
        for label, old, new in (("wire", "binding = _review_process_binding(receipt)", "binding = None"),
                ("artifact", 'if actual_hash != receipt["exe_sha256"]:', "if False:")):
            self.assertEqual(source.count(old), 1)
            owner = types.ModuleType("actual_process_binding_control_"+label)
            exec(compile(source.replace(old, new), "presentation_report.py", "exec"), owner.__dict__)
            with tempfile.TemporaryDirectory() as directory:
                capture, receipt_path, receipt, _, _ = self.fixture(directory)
                if label == "wire": receipt["process_binding"]["creation_time_unix_s"] += 1
                else: Path(receipt["exe"]).write_bytes(b"substituted artifact")
                receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
                def assert_gate():
                    with patch.object(owner, "file_hash", side_effect=ModernPresentationControls.hash_port(owner)), self.assertRaises(ValueError):
                        owner.review_capture(capture, receipt_path)
                result = unittest.TestResult(); unittest.FunctionTestCase(assert_gate).run(result)
                self.assertEqual((result.testsRun, len(result.failures), len(result.errors)), (1, 1, 0))
                self.assertIn("ValueError not raised", result.failures[0][1])


# Sanitized identity only; pinned 2.6.0 actual native-v1 header and first two
# rows retained verbatim numerically, including dropped zero display metrics.
NATIVE_V1_SAMPLE = """Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags,Dropped,TimeInSeconds,msInPresentAPI,msBetweenPresents,AllowsTearing,PresentMode,msUntilRenderComplete,msUntilDisplayed,msBetweenDisplayChange,msFlipDelay,msUntilRenderStart,msGPUActive,msSinceInput,QPCTime
app.exe,42,0x123,DXGI,0,512,1,6.71963970000000,0.97410000000000,216.99560000000002,0,Composed: Flip,11.71550000000000,0.00000000000000,0.00000000000000,0.00000000000000,0.36080000000000,3.39800000000000,0.00000000000000,17280.74667140000020
app.exe,42,0x123,DXGI,0,512,0,6.72172550000000,0.71500000000000,2.08580000000000,0,Composed: Flip,12.07680000000000,133.18350000000001,327.21850000000001,0.00000000000000,11.82380000000000,0.25300000000000,0.00000000000000,17280.74875719999909
"""


def native_v1_rows():
    return list(csv.DictReader(io.StringIO(NATIVE_V1_SAMPLE)))


class NativeV1PresentationControls(unittest.TestCase):
    def fixture(self, directory):
        capture, receipt_path, receipt = PresentationControls().fixture(directory)
        capture.write_text(NATIVE_V1_SAMPLE, encoding="utf-8")
        receipt.update(csv_schema=NATIVE_V1_SCHEMA, qpc_unit="seconds")
        receipt["observer_command"].append("--v1_metrics")
        receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
        return capture, receipt_path, receipt

    def test_actual_header_rows_seconds_present_start_and_dropped_preserved(self):
        rows = native_v1_rows(); before = copy.deepcopy(rows)
        self.assertEqual(tuple(rows[0]), NATIVE_V1_REQUIRED)
        report = review_rows(rows, 42, "app.exe", metrics_mode="native-v1")
        chain = report["swapchains"][0]
        self.assertEqual(rows, before)
        self.assertEqual(report["qpc_unit"], "seconds")
        self.assertEqual(report["qpc_source_column"], "QPCTime")
        self.assertAlmostEqual(chain["observed_window_s"], .0020858, places=10)
        self.assertAlmostEqual(chain["observed_present_start_cadence_hz"], 1/.0020858, places=4)
        self.assertNotIn("qpc_begin_ms", chain)
        self.assertNotIn("observed_cpu_start_cadence_hz", chain)
        self.assertNotIn("observed_present_cadence_hz", chain)
        self.assertEqual((chain["dropped_rows"], chain["not_dropped_rows"]), (1, 1))
        self.assertEqual(chain["classifications"][0]["mode"], "Composed: Flip")
        self.assertEqual(report["source_metric_columns"], list(NATIVE_V1_METRICS))
        self.assertAlmostEqual(chain["metrics"]["msGPUActive"]["p50_ms"], (3.398+.253)/2)
        self.assertEqual(chain["metrics"]["msUntilDisplayed"]["numeric_zero_rows"], 1)
        self.assertIn("not a scheduler", chain["metrics"]["msInPresentAPI"]["definition"])
        self.assertIn("upstream output", report["scope"])
        self.assertFalse(report["gpu_timing_qualification"]["hardware_gpu_busy_certified"])
        self.assertEqual(report["gpu_timing_qualification"]["hws_state"], "unknown")
        self.assertEqual(report["display_metadata"]["status"], "UNAVAILABLE")
        for key in ("CPUWait", "MsCPUWait", "GPUTime", "MsUntilDisplayed"):
            self.assertNotIn(key, chain["metrics"])
        # Session-relative seconds cannot supply the absolute QPC window.
        rows[0]["TimeInSeconds"] = rows[1]["TimeInSeconds"] = "999999"
        self.assertEqual(review_rows(rows, 42, "app.exe", metrics_mode="native-v1")["swapchains"][0]["observed_window_s"], chain["observed_window_s"])

    def test_native_na_zero_numeric_type_pid_classification_and_budget_guards(self):
        for key in NATIVE_V1_METRICS+("QPCTime", "TimeInSeconds"):
            for value in (True, 0, 1.5, "nan", "inf", "-1", "", None):
                if key in ("msUntilRenderStart", "msUntilRenderComplete") and value == "-1":
                    continue  # Source-backed signed relative offsets, not durations.
                rows = native_v1_rows(); rows[1][key] = value
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    review_rows(rows, 42, "app.exe", metrics_mode="native-v1")
        for key, value in (("Dropped", "2"), ("Dropped", True), ("PresentFlags", "-1"),
                ("PresentFlags", "512.0"), ("Runtime", "Other"), ("ProcessID", "43"),
                ("Application", "other.exe"), ("QPCTime", "17280")):
            rows = native_v1_rows(); rows[1][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                review_rows(rows, 42, "app.exe", metrics_mode="native-v1")
        rows = native_v1_rows(); rows[0]["msGPUActive"] = "NA"
        result = review_rows(rows, 42, "app.exe", metrics_mode="native-v1")
        self.assertEqual(result["swapchains"][0]["metrics"]["msGPUActive"]["unavailable_rows"], 1)
        with self.assertRaises(ValueError):
            review_rows(native_v1_rows()*2, 42, "app.exe", metrics_mode="native-v1", max_rows=2)

    def test_signed_render_offsets_are_preserved_without_clamp_or_wait_alias(self):
        rows = native_v1_rows()
        rows[1].update(msUntilRenderStart="-0.4171", msUntilRenderComplete="-0.0123")
        report = review_rows(rows, 42, "app.exe", metrics_mode="native-v1")
        metrics = report["swapchains"][0]["metrics"]
        self.assertEqual(metrics["msUntilRenderStart"]["min_ms"], -.4171)
        self.assertEqual(metrics["msUntilRenderComplete"]["min_ms"], -.0123)
        self.assertIn("not a wait duration", metrics["msUntilRenderStart"]["definition"])
        self.assertEqual(report["rows"], 2)

    def test_explicit_versioned_receipt_admission_and_no_schema_guess(self):
        import presentation_report as owner
        with tempfile.TemporaryDirectory() as directory:
            capture, receipt_path, receipt = self.fixture(directory)
            with patch.object(owner, "file_hash", side_effect=ModernPresentationControls.hash_port(owner)):
                self.assertEqual(review_capture(capture, receipt_path)["csv_schema"], NATIVE_V1_SCHEMA)
            changes = (lambda r: r.pop("csv_schema"), lambda r: r.update(csv_schema=True),
                lambda r: r.update(csv_schema="native-v1"), lambda r: r.update(csv_schema={}),
                lambda r: r.pop("qpc_unit"), lambda r: r.update(qpc_unit="milliseconds"),
                lambda r: r.update(qpc_unit=True), lambda r: r.update(qpc_unit=float("nan")),
                lambda r: r["observer_command"].remove("--v1_metrics"),
                lambda r: r["observer_command"].append("--v2_metrics"),
                lambda r: r["observer_command"].append("--v1_metrics"))
            for change in changes:
                bad = copy.deepcopy(receipt); change(bad)
                receipt_path.write_text(json.dumps(bad), encoding="utf-8")
                with self.subTest(receipt=bad), patch.object(owner, "file_hash", side_effect=ModernPresentationControls.hash_port(owner)), self.assertRaises(ValueError):
                    review_capture(capture, receipt_path)

    def test_native_header_case_timestamp_ambiguity_and_width_refused(self):
        import presentation_report as owner
        with tempfile.TemporaryDirectory() as directory:
            capture, receipt_path, _ = self.fixture(directory)
            for payload in (NATIVE_V1_SAMPLE.replace("msInPresentAPI", "MsInPresentAPI"),
                    NATIVE_V1_SAMPLE.replace("QPCTime", "CPUStartQPCTime"),
                    NATIVE_V1_SAMPLE.replace("QPCTime\n", "QPCTime,CPUStartQPCTimeInMs\n"),
                    NATIVE_V1_SAMPLE.replace("msGPUActive", "msGPUActive,msGPUActive"),
                    NATIVE_V1_SAMPLE.replace("0.97410000000000,", "0.97410000000000,extra,")):
                capture.write_text(payload, encoding="utf-8")
                with patch.object(owner, "file_hash", side_effect=ModernPresentationControls.hash_port(owner)), self.assertRaises(ValueError):
                    review_capture(capture, receipt_path)

    def test_native_input_disabled_receipt_and_header_agree(self):
        import presentation_report as owner
        with tempfile.TemporaryDirectory() as directory:
            capture, receipt_path, receipt = self.fixture(directory)
            stream = io.StringIO()
            writer = csv.DictWriter(stream, [k for k in NATIVE_V1_REQUIRED if k != "msSinceInput"])
            writer.writeheader()
            for row in native_v1_rows():
                row.pop("msSinceInput")
                writer.writerow(row)
            no_input = stream.getvalue()
            capture.write_text(no_input, encoding="utf-8")
            with patch.object(owner, "file_hash", side_effect=ModernPresentationControls.hash_port(owner)):
                with self.assertRaises(ValueError):
                    review_capture(capture, receipt_path)
                default = receipt["observer_command"]
                receipt["observer_command"] = default + ["--no_track_input"]
                receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
                report = review_capture(capture, receipt_path)
                self.assertEqual(report["input_tracking"], "DISABLED_BY_COLLECTOR")
                self.assertEqual(report["qpc_unit"], "seconds")
                self.assertNotIn("msSinceInput", report["source_metric_columns"])
                self.assertNotIn("msSinceInput", report["swapchains"][0]["metrics"])
                for bad in (NATIVE_V1_SAMPLE, NATIVE_V1_SAMPLE.replace("msSinceInput", "MSSINCEINPUT"),
                            no_input.replace("msGPUActive", "badColumn")):
                    capture.write_text(bad, encoding="utf-8")
                    with self.assertRaises(ValueError):
                        review_capture(capture, receipt_path)
                capture.write_text(no_input, encoding="utf-8")
                for flags in (["--no_track_input", "--no_track_input"], ["/NO_TRACK_INPUT"],
                              ["--session_name", "--no_track_input"]):
                    receipt["observer_command"] = default + flags
                    receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
                    with self.subTest(flags=flags), self.assertRaises(ValueError):
                        review_capture(capture, receipt_path)

    def test_native_reuses_retired_binding_and_qualified_wait_unavailability(self):
        import presentation_report as owner
        with tempfile.TemporaryDirectory() as directory:
            capture, receipt_path, receipt, _, _ = ProcessBindingControls().fixture(directory)
            capture.write_text(NATIVE_V1_SAMPLE, encoding="utf-8")
            receipt.update(csv_schema=NATIVE_V1_SCHEMA, qpc_unit="seconds")
            receipt["observer_command"].append("--v1_metrics")
            receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
            with patch.object(owner, "file_hash", side_effect=ModernPresentationControls.hash_port(owner)):
                report = review_capture(capture, receipt_path)
            self.assertEqual(report["process_binding"]["status"], "RETIRED_SAMPLER_BIRTH_AND_ARTIFACT_BOUND")
            self.assertEqual(report["wait_attribution"]["status"], "UNAVAILABLE")
            self.assertEqual(report["process_binding"]["os_image_to_pid"], "UNAVAILABLE")
            self.assertFalse(report["wait_attribution"]["gpu_completion_certified"])

    def test_whole_owner_missing_schema_unit_and_wrong_scale_fail_ordinary_assertions(self):
        source = (Path(__file__).resolve().parents[1]/"presentation_report.py").read_text(encoding="utf-8")
        variants = (
            ("schema_wire", "metrics_mode = _receipt_metrics_mode(receipt, command)", 'metrics_mode = "native-v1"'),
            ("unit_gate", 'if receipt.get("qpc_unit") != "seconds":', "if False:"),
            ("seconds_scale", "/ (1 if native_v1 else 1000)", "/ 1000"),
            ("signed_offset", "signed=native_v1 and key in NATIVE_V1_SIGNED", "signed=False"))
        for label, old, new in variants:
            self.assertEqual(source.count(old), 1)
            owner = types.ModuleType("actual_native_v1_control_"+label)
            exec(compile(source.replace(old, new), "presentation_report.py", "exec"), owner.__dict__)
            with tempfile.TemporaryDirectory() as directory:
                capture, receipt_path, receipt = self.fixture(directory)
                if label == "schema_wire": receipt.pop("csv_schema")
                if label == "unit_gate": receipt["qpc_unit"] = "milliseconds"
                if label == "signed_offset":
                    capture.write_text(NATIVE_V1_SAMPLE.replace("11.82380000000000", "-0.4171"), encoding="utf-8")
                receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
                def assert_gate():
                    with patch.object(owner, "file_hash", side_effect=ModernPresentationControls.hash_port(owner)):
                        if label == "signed_offset":
                            try: report = owner.review_capture(capture, receipt_path)
                            except ValueError: self.fail("source-backed negative render-start offset was rejected")
                            self.assertEqual(report["swapchains"][0]["metrics"]["msUntilRenderStart"]["min_ms"], -.4171)
                        elif label == "seconds_scale":
                            self.assertAlmostEqual(owner.review_capture(capture, receipt_path)["swapchains"][0]["observed_window_s"], .0020858, places=10)
                        else:
                            with self.assertRaises(ValueError): owner.review_capture(capture, receipt_path)
                result = unittest.TestResult(); unittest.FunctionTestCase(assert_gate).run(result)
                self.assertEqual((result.testsRun, len(result.failures), len(result.errors)), (1, 1, 0))


if __name__=="__main__": unittest.main()
