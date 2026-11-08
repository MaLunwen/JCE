import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from particle_process import particle_backend, run_particle_process, validate_particle_execution


class ParticleProcessControls(unittest.TestCase):
    BACKEND = "benchmark-particle-backend: reported=Direct3D 11 at particle_witness.c:56\n"

    def check_case(self, process=None, log=None, rows=None, write_log=True, write_csv=True):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            engine, csv = directory / "engine.log", directory / "frames.csv"
            if write_log:
                engine.write_text(self.BACKEND if log is None else log, encoding="utf-8")
            if write_csv:
                csv.write_text("frame_index,phase,ms\n" + ("".join(
                    "%u,frame_wall,1.0\n" % frame for frame in range(200)) if rows is None else rows), encoding="utf-8")
            return validate_particle_execution({"returncode": 0, "timed_out": False} if process is None else process,
                                               engine, csv, 200, "d3d11")

    def test_complete_csv_without_stock_perf_summary(self):
        self.assertEqual(self.check_case(), "Direct3D 11")

    def test_process_exit_timeout_and_missing_completion_rejected(self):
        for process in ({"returncode": 1, "timed_out": False},
                        {"returncode": 0, "timed_out": True},
                        {"returncode": False, "timed_out": False},
                        {"returncode": 0}, {"timed_out": False}, {}):
            with self.subTest(process=process), self.assertRaises(ValueError):
                self.check_case(process=process)

    def test_missing_runtime_records_and_failure_markers_rejected(self):
        for marker in ("] ERROR - failed", "single-instance lock already held",
                       "CPU particle upload unavailable", "CPU instance upload unavailable"):
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                self.check_case(log=self.BACKEND + marker)
        for record in ((False, True), (True, False)):
            with self.subTest(record=record), self.assertRaises(ValueError):
                self.check_case(write_log=record[0], write_csv=record[1])

    def test_actual_backend_missing_duplicate_malformed_or_conflicting_rejected(self):
        for log in ("", self.BACKEND * 2, self.BACKEND.replace("Direct3D 11", "Vulkan"),
                    self.BACKEND.replace("Direct3D 11", "Direct3D 11 garbage"),
                    self.BACKEND + "jce_renderer: renderer: Vulkan"):
            with self.subTest(log=log), self.assertRaises((ValueError, RuntimeError)):
                self.check_case(log=log)
        with self.assertRaises(ValueError):
            particle_backend(self.BACKEND + "jce_renderer: renderer: Vulkan", "auto")

    def test_csv_truncation_gaps_duplicates_nonfinite_and_wrong_origin_rejected(self):
        cases = ["".join("%u,frame_wall,1\n" % i for i in range(61)),
                 "".join("%u,frame_wall,1\n" % i for i in range(1, 201)),
                 "0,frame_wall,1\n2,frame_wall,1\n",
                 "0,frame_wall,1\n0,frame_wall,1\n",
                 "0,frame_wall,nan\n", "0,frame_wall,-1\n", "0,frame_wall,0\n",
                 "0,frame_cpu,1\n", "bad,row\n", ""]
        for rows in cases:
            with self.subTest(rows=rows[:40]), self.assertRaises(ValueError):
                self.check_case(rows=rows)

    def test_stale_output_is_retained_and_never_executed(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            engine, csv = directory / "engine.log", directory / "frames.csv"
            engine.write_text("old evidence", encoding="utf-8")
            with patch("particle_process.run_process") as execute, self.assertRaises(ValueError):
                run_particle_process({}, engine, 200, exe=directory / "app.exe", samples_path=csv)
            execute.assert_not_called()
            self.assertEqual(engine.read_text(), "old evidence")

    def test_shared_sampler_receives_clean_hidden_environment(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            engine, csv = directory / "engine.log", directory / "frames.csv"
            def execute(command, **kwargs):
                env = kwargs["env"]
                self.assertNotIn("JCE_UNRELATED_STALE_SETTING", env)
                self.assertEqual(env["JCE_WINDOW_HIDDEN"], "1")
                self.assertEqual(env["JCE_KPI_FRAME_COUNT"], "200")
                self.assertEqual(env["JCE_SCENE"], "explicit.scene")
                engine.write_text(self.BACKEND, encoding="utf-8")
                csv.write_text("frame_index,phase,ms\n" + "".join(
                    "%u,frame_wall,1\n" % i for i in range(200)), encoding="utf-8")
                return {"returncode": 0, "timed_out": False, "samples": [], "stdout": "", "stderr": ""}
            with patch.dict(os.environ, {"JCE_UNRELATED_STALE_SETTING": "1"}), \
                    patch("particle_process.run_process", side_effect=execute):
                result = run_particle_process({"JCE_BACKEND": "d3d11", "JCE_SCENE": "explicit.scene"},
                    engine, 200, exe=directory / "app.exe", samples_path=csv)
            self.assertEqual(result["backend_actual"], "Direct3D 11")
            self.assertEqual(result["process"]["returncode"], 0)
            self.assertEqual(result["raw_frame_count"], 200)
            self.assertNotIn("frame", result)


if __name__ == "__main__":
    unittest.main()
