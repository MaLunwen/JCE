import json
from pathlib import Path
import sys
import unittest
import tempfile
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.benchmark_report import groups, load_suite, reference_section, verify_reference_evidence
from tools.benchmark_probe_report import load_probe_suite


class BenchmarkReportTest(unittest.TestCase):
    def documents(self, median=2.0):
        return [
            {"schema_version": 2, "suite_id": "test", "host": "player", "engine": "test",
             "results": [{"status": "PASS", "directory": "unused", "kind": "work",
                          "variant": "A", "frame": {"sample_count": 3,
                                                     "frame_median_ms": median, "frame_p99_ms": 4.0}}]},
            {"engine": "test", "host": "player", "variant": "A", "raw_file": "samples.csv",
             "executable_sha256": "exe", "workload": {"kind": "work", "count": 1000,
             "warmup_frames": 1, "sample_frames": 3, "scene_sha256": "scene"}}]

    @mock.patch("tools.benchmark_report.sha256_file", return_value="digest")
    @mock.patch("tools.benchmark_report.read_frames", return_value={"frame_ms": [1.0, 2.0, 4.0],
                 "cpu_frame_ms": [], "gpu_frame_ms": [], "phase_ms": {}})
    def test_raw_samples_must_agree_with_claimed_summary(self, reader, digest):
        for median in (2.0, 5.0, True):
            docs = iter(self.documents(median))
            with mock.patch.object(Path, "read_text", side_effect=lambda **kwargs: json.dumps(next(docs))):
                if median == 2.0:
                    _, rows = load_suite("input.json")
                    self.assertEqual(rows[0]["raw_sha256"], "digest")
                else:
                    with self.assertRaises(ValueError):
                        load_suite("input.json")

    def test_failed_results_do_not_get_numeric_bars_or_read_old_artifacts(self):
        data = self.documents()[0]
        data["results"][0]["status"] = "FAIL"
        with mock.patch.object(Path, "read_text", return_value=json.dumps(data)) as reader:
            _, rows = load_suite("input.json")
        self.assertEqual(reader.call_count, 1)
        self.assertNotIn("frame", rows[0])
        self.assertFalse(groups(rows))

    def test_different_binary_scene_host_or_suite_cannot_be_pooled(self):
        original = {"status": "PASS", "suite_id": "s", "host": "p", "kind": "k", "count": 2,
                    "variant": "A", "backend_actual": "b", "executable_sha256": "e", "scene_sha256": "f"}
        for key in ("suite_id", "host", "kind", "variant", "backend_actual", "executable_sha256", "scene_sha256"):
            self.assertEqual(len(groups([original, dict(original, **{key: "other"})])), 2)

    def test_public_references_cannot_impersonate_matched_measurements(self):
        row = {"status": "REFERENCE_ONLY", "engine": "external", "title": "Official",
               "url": "https://example.org", "scope": "scenario", "scenario": "boxes"}
        source = {"schema_version": 1, "columns": ["external"], "engines": [row]}
        self.assertIn("not local timings", "\n".join(reference_section(source)))
        for changed in (dict(row, status="PASS"), dict(row, frame_ms=0)):
            with self.assertRaises(ValueError):
                reference_section(dict(source, engines=[changed]))

    def test_coverage_chart_requires_attributable_supplied_measurements(self):
        source = {"columns": ["test"], "mapping": [{"status": {"test": "MEASURED"},
                  "evidence": {"test": [{"suite_id": "s", "kind": "work"}]}}]}
        with self.assertRaises(ValueError):
            verify_reference_evidence(source, [])
        verify_reference_evidence(source, [({"engine": "test", "suite_id": "s"},
                                           [{"status": "PASS", "kind": "work"}])])

    def test_cpu_reports_check_integer_clocks_and_completed_population(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run = root / "trial"
            run.mkdir()
            manifest = {"schema": 1, "suite_id": "cpu", "host": "player", "engine": "test",
                        "rounds": 1, "executable_sha256": "binary", "timing_scope": "one step"}
            entry = {"status": "PASS", "probe": "work", "count": 10, "concurrency": 1,
                     "depth": 1, "directory": str(run), "median_ms": 0.5, "p99_ms": 0.5}
            suite = root / "suite.json"
            suite.write_text(json.dumps({"manifest": manifest, "results": [entry]}), encoding="utf-8")
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            heading = "probe,round,count,completed,concurrency,depth,ticks,frequency,correct,ms\n"
            for completed, elapsed, index in ((10, 0.5, 0), (9, 0.5, 0), (10, 9, 0), (10, 0.5, 1)):
                (run / "samples.csv").write_text(heading +
                    "work,%d,10,%d,1,1,500,1000000,1,%s\n" % (index, completed, elapsed), encoding="utf-8")
                if (completed, elapsed, index) == (10, 0.5, 0):
                    data, rows = load_probe_suite(suite)
                    self.assertEqual(data["measurement_domain"], "CPU workload")
                    self.assertEqual(rows[0]["median_ms"], 0.5)
                    self.assertNotIn("frame", rows[0])
                else:
                    with self.assertRaises(ValueError):
                        load_probe_suite(suite)


if __name__ == "__main__":
    unittest.main()
