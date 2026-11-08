"""Guarded telemetry controls; application launches are mocked, Python controls run."""
import inspect
import csv
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools import perf_bench


HEADER = "frame_index,phase,ms\n"
BODY = "".join(f"{i},frame_wall,{i + 1}\n{i},ecs_move,0.1\n" for i in range(3))
BACKEND = "jce_renderer: renderer: Direct3D 11 (graphics tier stable)\n"
WORKLOAD = "benchmark-spawn: kind=3 requested=100 spawned=100\n"
PERF = ("engine: perf: 1.5 ms avg (667 FPS) | cpu 1.0 / gpu 0.5 ms | 10 draws | "
        "gpu-mem 12 MB (tex 10 + rt 2) | rss 20 MB | commit 30\n")


class RawCsvAdmissionControls(unittest.TestCase):
    def setUp(self):
        directory = perf_bench.ROOT / "build/tests/perf-bench-log-policy"
        directory.mkdir(parents=True, exist_ok=True)
        holder = tempfile.TemporaryDirectory(dir=directory)
        self.addCleanup(holder.cleanup)
        self.root = Path(holder.name)
        self.log = self.root / "engine.log"
        self.csv = self.root / "frames.csv"
        self.env = {"JCE_BACKEND": "d3d11", "JCE_PERF_LOG": "0", "JCE_MULTI_INSTANCE": "1"}
        self.observed = {}

    def writer(self, *, accepted=3, written=3, payload=None, accepted_bytes=None,
               written_bytes=None):
        length = len((BODY if payload is None else payload).encode("utf-8"))
        return ("engine: frame KPI capture complete: accepted=%d written=%d "
                "accepted_bytes=%d written_bytes=%d peak_bytes=1024 peak_records=3\n" %
                (accepted, written, length if accepted_bytes is None else accepted_bytes,
                 length if written_bytes is None else written_bytes))

    def invoke(self, *, require=False, csv=True, body=BODY, header=HEADER,
               log=None, process=None, write_log=True, write_csv=True, expected=None,
               extra_env=None, error=None, run_options=None):
        environment = dict(self.env)
        environment.update(extra_env or {})
        text = BACKEND + WORKLOAD + self.writer(payload=body) if log is None else log
        def child(command, *, env, **kwargs):
            self.observed.update(env=env, command=command, options=kwargs)
            if error is not None:
                raise error
            if write_log:
                self.log.write_text(text, encoding="utf-8", newline="\n")
            if csv and write_csv:
                self.csv.write_text(header + body, encoding="utf-8", newline="\n")
            result = {"timed_out": False, "returncode": 0, "elapsed_s": 2.0,
                      "stdout": "", "stderr": "", "samples": []}
            result.update(process or {})
            return result
        options = {}
        # The BEFORE owner must reach its real missing-perf rejection rather
        # than stop at an unknown keyword. AFTER exercises the explicit policy.
        parameters = inspect.signature(perf_bench.run).parameters
        if not require and "require_perf_log" in parameters:
            options["require_perf_log"] = False
        if expected is not None and "expected_frame_count" in parameters:
            options["expected_frame_count"] = expected
        options.update(run_options or {})
        with mock.patch.object(perf_bench, "run_process", side_effect=child):
            return perf_bench.run(environment, self.log, frames=3,
                exe=self.root / "owned_fixture.exe", samples_path=self.csv if csv else None,
                expected_workload=(3, 100), **options)

    def test_log_binding_accepts_exact_and_cwd_relative_target(self):
        for binding in (str(self.log), self.log.name):
            with self.subTest(binding=binding):
                self.invoke(extra_env={"JCE_LOG_FILE":binding}, run_options={"cwd":self.root})
                self.assertEqual(self.observed["env"]["JCE_LOG_FILE"], binding)

    def test_conflicting_log_target_preserves_evidence_before_launch(self):
        self.log.write_bytes(b"prior-log"); self.csv.write_bytes(b"prior-csv")
        with self.assertRaisesRegex(ValueError,"JCE_LOG_FILE"):
            self.invoke(extra_env={"JCE_LOG_FILE":str(self.root/"other.log")})
        self.assertEqual(self.log.read_bytes(),b"prior-log")
        self.assertEqual(self.csv.read_bytes(),b"prior-csv")
        self.assertFalse(self.observed)

    def test_invalid_log_target_is_refused_before_launch(self):
        for binding in ("", None, True, 0, []):
            with self.subTest(binding=binding):
                with self.assertRaisesRegex(ValueError,"JCE_LOG_FILE"):
                    self.invoke(extra_env={"JCE_LOG_FILE":binding})
                self.assertFalse(self.observed)

    def test_terminal_request_precedes_launch_and_survives_failure(self):
        nonce = "8" * 32
        expected = self.root / "engine_process" / f"terminal-ack-request-{nonce}.json"
        exe = self.root / "owned_fixture.exe"
        trace = self.root / "trace.json"
        def child(command, *, env, **options):
            request = json.loads(expected.read_text(encoding="utf-8"))
            self.assertEqual(request["nonce"], env["JCE_TERMINAL_ACK_TOKEN"])
            self.assertEqual(request["exe_path"], command[0])
            self.assertEqual(request["launch_cwd"], options["cwd"])
            self.assertEqual(Path(request["expected_stderr_path"]),
                             options["output_dir"] / "stderr.log")
            self.assertEqual(Path(request["trace_path"]), trace)
            return {"timed_out": False, "returncode": 5, "stderr": "controlled failure"}
        with mock.patch.object(perf_bench.secrets, "token_hex", return_value=nonce), \
                mock.patch.object(perf_bench, "run_process", side_effect=child) as launched:
            with self.assertRaisesRegex(RuntimeError, "exited 5"):
                perf_bench.run({"JCE_MULTI_INSTANCE": "1", "JCE_TRACE_STREAM": str(trace)},
                    self.log, exe=exe, cwd=self.root, require_terminal_ack=True)
        launched.assert_called_once()
        self.assertEqual(json.loads(expected.read_text(encoding="utf-8"))["nonce"], nonce)

    def test_legacy_run_does_not_write_terminal_request(self):
        self.invoke()
        self.assertEqual(list(self.root.glob("*_process/terminal-ack-request-*.json")), [])
        self.assertNotIn("JCE_TERMINAL_ACK_TOKEN", self.observed["env"])

    def test_gpu_options_forward_to_existing_process_owner_and_keep_receipt(self):
        peers = (77, 88)
        receipt = {"gpu_sampling_requested": True, "gpu_output_limit": 2,
                   "gpu_peer_pids": list(peers), "gpu_samples_observed": 3,
                   "gpu_samples_path": str(self.root / "gpu_samples.json"),
                   "gpu_close_status": 0}
        requested = {"sample_gpu": True, "gpu_limit": 2, "gpu_peer_pids": peers}
        # A pre-forwarding owner must fail the observation, not its signature.
        parameters = inspect.signature(perf_bench.run).parameters
        supported = {key: value for key, value in requested.items() if key in parameters}
        result = self.invoke(process=receipt, run_options=supported)
        observed = self.observed["options"]
        self.assertIs(observed.get("sample_gpu", False), True,
                      "GPU request must reach existing process sampler")
        self.assertEqual(observed.get("gpu_limit"), 2)
        self.assertIs(observed.get("gpu_peer_pids"), peers)
        for key, value in receipt.items():
            self.assertEqual(result["process"][key], value)
        self.assertNotIn("gpu", result["process"])
        self.assertIsNone(result["gpu"])
        self.assertIsNone(result["fps"])

    def test_gpu_defaults_keep_existing_unsampled_result_shape(self):
        result = self.invoke()
        observed = self.observed["options"]
        self.assertIs(observed.get("sample_gpu", False), False)
        self.assertEqual(observed.get("gpu_limit", 256), 256)
        self.assertEqual(observed.get("gpu_peer_pids", ()), ())
        self.assertFalse(any(key.startswith("gpu_") for key in result["process"]))
        self.assertIsNone(result["gpu"])

    def test_invalid_gpu_options_preserve_prior_evidence_before_launch(self):
        self.log.write_bytes(b"old independent log")
        self.csv.write_bytes(b"old independent raw")
        cases = ({"sample_gpu": 1}, {"sample_gpu": None},
                 {"gpu_limit": 0}, {"gpu_limit": True}, {"gpu_limit": 1025},
                 {"gpu_peer_pids": (77,)},
                 {"sample_gpu": True, "gpu_peer_pids": (77, 77)},
                 {"sample_gpu": True, "gpu_peer_pids": (0,)},
                 {"sample_gpu": True, "gpu_peer_pids": (True,)},
                 {"sample_gpu": True, "gpu_peer_pids": (0x100000000,)},
                 {"sample_gpu": True, "gpu_peer_pids": tuple(range(1, 18))},
                 {"sample_gpu": True, "gpu_peer_pids": {77}})
        with mock.patch.object(perf_bench, "run_process") as child:
            for options in cases:
                with self.subTest(options=options):
                    with self.assertRaises(ValueError):
                        perf_bench.run(self.env, self.log, frames=3,
                            exe=self.root / "owned_fixture.exe", samples_path=self.csv,
                            require_perf_log=False, **options)
                    self.assertEqual(self.log.read_bytes(), b"old independent log")
                    self.assertEqual(self.csv.read_bytes(), b"old independent raw")
            child.assert_not_called()

    def test_same_owner_missing_gpu_forwarder_fails_ordinary_assertion(self):
        source = Path(perf_bench.__file__).read_text(encoding="utf-8")
        before = ("interval=sample_interval, sample_gpu=sample_gpu,\n"
                  "                         gpu_limit=gpu_limit, gpu_peer_pids=gpu_peer_pids)")
        self.assertEqual(source.count(before), 1)
        owner = self.root / "missing_gpu_forwarder_owner.py"
        owner.write_text(source.replace(before, "interval=sample_interval)", 1),
                         encoding="utf-8", newline="\n")
        script = ("import importlib.util, sys, unittest\n"
                  "from tools.tests import test_perf_bench as controls\n"
                  "spec=importlib.util.spec_from_file_location('missing_gpu_forwarder_owner',sys.argv[1])\n"
                  "owner=importlib.util.module_from_spec(spec)\n"
                  "spec.loader.exec_module(owner)\n"
                  "controls.perf_bench=owner\n"
                  "suite=unittest.TestSuite([controls.RawCsvAdmissionControls("
                  "'test_gpu_options_forward_to_existing_process_owner_and_keep_receipt')])\n"
                  "result=unittest.TextTestRunner(verbosity=2).run(suite)\n"
                  "raise SystemExit(0 if result.wasSuccessful() else 1)\n")
        result = subprocess.run([sys.executable, "-B", "-c", script, str(owner)],
            cwd=ROOT, capture_output=True, text=True, timeout=30)
        output = result.stdout + result.stderr
        (self.root / "missing_gpu_forwarder.log").write_text(output, encoding="utf-8", newline="\n")
        self.assertEqual(result.returncode, 1, output)
        self.assertIn("FAILED (failures=1)", output)
        self.assertNotIn("ERROR:", output)
        self.assertIn("GPU request must reach existing process sampler", output)

    def test_default_remains_strict_when_raw_csv_and_writer_are_complete(self):
        with self.assertRaisesRegex(RuntimeError, "no perf line"):
            self.invoke(require=True)

    def test_default_retains_actual_periodic_metrics(self):
        result = self.invoke(require=True, log=BACKEND + WORKLOAD + PERF,
                             extra_env={"JCE_PERF_LOG": "1"})
        self.assertEqual(result["frame"], 1.5)
        self.assertEqual(result["draws"], 10)

    def test_unknown_memory_does_not_discard_valid_periodic_timing(self):
        line = PeriodicMemoryControls.line(gpu="N/A", tex="0", rt="N/A",
                                          rss="N/A", commit="0")
        result = self.invoke(require=True, log=BACKEND + WORKLOAD + line,
                             extra_env={"JCE_PERF_LOG": "1"})
        self.assertTrue(result["perf_log_available"])
        self.assertEqual(result["frame"], 1.5)
        self.assertEqual(result["draws"], 10)
        self.assertIsNone(result["gpu_mem"])
        self.assertIsNone(result["rt"])
        self.assertIsNone(result["rss"])
        self.assertEqual(result["tex"], 0)
        self.assertEqual(result["commit"], 0)
        self.assertEqual(result["memory_observation"]["memory_unit"], "MiB")
        self.assertEqual(result["memory_observation"]["memory_raw"]["gpu_mem_mb"], "N/A")

    def test_explicit_log_off_accepts_complete_cpu_csv_without_gpu_rows(self):
        result = self.invoke()
        self.assertFalse(result["perf_log_available"])
        self.assertEqual(result["phases"], {})
        for key in ("frame", "fps", "cpu", "gpu", "draws", "gpu_mem", "tex", "rt", "rss", "commit"):
            self.assertIsNone(result[key])
        self.assertEqual(result["raw_frame_count"], 3)
        self.assertEqual(result["kpi_writer"]["accepted"], 3)
        self.assertEqual(result["backend_actual"], "Direct3D 11")
        self.assertEqual(self.observed["env"]["JCE_PERF_LOG"], "0")

    def test_complete_raw_capture_accepts_and_counts_zero_wall_intervals(self):
        body = "0,frame_wall,0\n0,ecs_move,0\n1,frame_wall,1\n2,frame_wall,0\n"
        result = self.invoke(body=body)
        self.assertEqual(result["raw_frame_count"], 3)
        self.assertEqual(result["kpi_writer"]["accepted"], 3)
        self.assertEqual(result["kpi_writer"]["written_bytes"], len(body.encode("utf-8")))

    @staticmethod
    def extended_capture():
        packet = {"v": 1, "window_present": True, "flags_begin": 512, "flags_end": 512,
                  "point_size": [1280, 720], "drawable_size": [2560, 1440],
                  "native_size_begin": [2560, 1440], "native_size_end": [2560, 1440],
                  "backend_frame_begin": 0, "backend_frame_end": 1}
        stream = io.StringIO(newline="")
        writer = csv.writer(stream, lineterminator="\n")
        for index in range(3):
            writer.writerow([index, "frame_wall", index + 1, json.dumps(packet)])
            writer.writerow([index, "ecs_move", 0.1, ""])
        return "frame_index,phase,ms,context_json\n", stream.getvalue()

    def test_extended_header_uses_actual_payload_accounting(self):
        header, body = self.extended_capture()
        try:
            result = self.invoke(header=header, body=body,
                                 extra_env={"JCE_FRAME_LOG_CONTEXT": "1"})
        except (ValueError, RuntimeError) as error:
            self.fail("extended capture must use its validated header: " + str(error))
        self.assertEqual(result["frame_context_schema"], "window_v1")
        self.assertEqual(result["kpi_writer"]["written_bytes"], len(body.encode("utf-8")))
        self.assertEqual(result["raw_frame_count"], 3)

    def test_extended_header_rejects_legacy_payload_size_and_nonrectangular_rows(self):
        header, body = self.extended_capture()
        wrong = len(body.encode("utf-8")) + len(header) - len(HEADER)
        with self.assertRaisesRegex(RuntimeError, "payload bytes"):
            self.invoke(header=header, body=body,
                extra_env={"JCE_FRAME_LOG_CONTEXT": "1"},
                log=BACKEND + WORKLOAD + self.writer(accepted_bytes=wrong, written_bytes=wrong))
        with self.assertRaisesRegex(ValueError, "field count"):
            self.invoke(header=header, body=body.replace("ecs_move,0.1,\n", "ecs_move,0.1\n"))

    def test_context_request_must_match_actual_schema(self):
        header, body = self.extended_capture()
        for options in ({"header": header, "body": body},
                        {"extra_env": {"JCE_FRAME_LOG_CONTEXT": "1"}}):
            with self.subTest(options=options), self.assertRaisesRegex(RuntimeError, "context schema"):
                self.invoke(**options)
        self.log.write_bytes(b"prior log")
        self.csv.write_bytes(b"prior CSV")
        with mock.patch.object(perf_bench, "run_process") as child:
            with self.assertRaisesRegex(ValueError, "CONTEXT"):
                self.invoke(extra_env={"JCE_FRAME_LOG_CONTEXT": "yes"})
            child.assert_not_called()
        self.assertEqual(self.log.read_bytes(), b"prior log")
        self.assertEqual(self.csv.read_bytes(), b"prior CSV")

    def viewport_capture(self, *, viewport_updates=None):
        header, body = self.extended_capture()
        viewport = {"source_id": 1, "flags": 0xf0, "cpu_outcome": 0,
                    "refusal_reason": 0, "requested_size": None, "allocated_size": None,
                    "display_size": None, "texture_index": 0, "image_count": 0,
                    "queued_commands": 0, "main_submits": 0, "recording_submits": 0}
        viewport.update(viewport_updates or {})
        stream = io.StringIO(newline="")
        writer = csv.writer(stream, lineterminator="\n")
        for row in csv.reader(io.StringIO(body)):
            if row[1] == "frame_wall":
                packet = json.loads(row[3])
                packet.update(v=2, viewport_reported=True, viewports=[viewport])
                row[3] = json.dumps(packet)
            writer.writerow(row)
        return header, stream.getvalue()

    def test_context_v2_explicit_selector_uses_shared_parser_and_payload_accounting(self):
        header, body = self.viewport_capture()
        try:
            result = self.invoke(header=header, body=body,
                                 extra_env={"JCE_FRAME_LOG_CONTEXT": "2"})
        except (ValueError, RuntimeError) as error:
            self.fail("v2 capture must honor explicit selector 2: " + str(error))
        self.assertEqual(result["frame_context_schema"], "window_viewports_v2")
        self.assertEqual(self.observed["env"]["JCE_FRAME_LOG_CONTEXT"], "2")
        self.assertEqual(result["raw_frame_count"], 3)
        self.assertEqual(result["kpi_writer"]["written_bytes"], len(body.encode("utf-8")))
        wrong = len(body.encode("utf-8")) + len(header) - len(HEADER)
        with self.assertRaisesRegex(RuntimeError, "payload bytes"):
            self.invoke(header=header, body=body, extra_env={"JCE_FRAME_LOG_CONTEXT": "2"},
                        log=BACKEND + WORKLOAD + self.writer(accepted_bytes=wrong, written_bytes=wrong))

    def clock_capture(self, *, clock_updates=None):
        header, body = self.viewport_capture()
        stream = io.StringIO(newline="")
        writer = csv.writer(stream, lineterminator="\n")
        for row in csv.reader(io.StringIO(body)):
            if row[1] == "frame_wall":
                index = int(row[0])
                clock = {"domain": "jce-performance-counter", "frequency_hz": "1000000",
                         "begin": str(index * 10000), "end": str(index * 10000 + (index + 1) * 1000)}
                clock.update(clock_updates or {})
                packet = json.loads(row[3])
                packet.update(v=3, clock=clock)
                row[3] = json.dumps(packet)
            writer.writerow(row)
        return header, stream.getvalue()

    def test_context_v3_explicit_selector_uses_shared_parser_and_payload_accounting(self):
        header, body = self.clock_capture()
        result = self.invoke(header=header, body=body, extra_env={"JCE_FRAME_LOG_CONTEXT": "3"})
        self.assertEqual(result["frame_context_schema"], "window_viewports_clock_v3")
        self.assertEqual(self.observed["env"]["JCE_FRAME_LOG_CONTEXT"], "3")
        self.assertEqual(result["raw_frame_count"], 3)
        self.assertEqual(result["kpi_writer"]["written_bytes"], len(body.encode("utf-8")))
        wrong = len(body.encode("utf-8")) + len(header) - len(HEADER)
        with self.assertRaisesRegex(RuntimeError, "payload bytes"):
            self.invoke(header=header, body=body, extra_env={"JCE_FRAME_LOG_CONTEXT": "3"},
                        log=BACKEND + WORKLOAD + self.writer(accepted_bytes=wrong, written_bytes=wrong))

    def test_context_v3_selector_requires_its_exact_schema(self):
        v3_header, v3_body = self.clock_capture()
        for header, body in (self.extended_capture(), self.viewport_capture(), (HEADER, BODY)):
            with self.subTest(body=body[:120]), self.assertRaisesRegex(RuntimeError, "context schema"):
                self.invoke(header=header, body=body, extra_env={"JCE_FRAME_LOG_CONTEXT": "3"})
        for selector in ("", "0", "1", "2"):
            with self.subTest(selector=selector), self.assertRaisesRegex(RuntimeError, "context schema"):
                self.invoke(header=v3_header, body=v3_body, extra_env={"JCE_FRAME_LOG_CONTEXT": selector})

    def test_context_v3_admission_reuses_full_clock_validation_and_raw_zero_policy(self):
        for updates in ({"frequency_hz": "0"}, {"begin": "01"}, {"end": "18446744073709551616"},
                        {"domain": "unix"}, {"end": "1"}):
            header, body = self.clock_capture(clock_updates=updates)
            with self.subTest(updates=updates), self.assertRaises(ValueError):
                self.invoke(header=header, body=body, extra_env={"JCE_FRAME_LOG_CONTEXT": "3"})
        header, body = self.clock_capture()
        rows = list(csv.reader(io.StringIO(body)))
        packet = json.loads(rows[-2][3])
        packet["clock"].update(frequency_hz="2000000", end="26000")
        rows[-2][3] = json.dumps(packet)
        stream = io.StringIO(newline="")
        csv.writer(stream, lineterminator="\n").writerows(rows)
        with self.assertRaisesRegex(ValueError, "frequency changed"):
            self.invoke(header=header, body=stream.getvalue(), extra_env={"JCE_FRAME_LOG_CONTEXT": "3"})
        rows = list(csv.reader(io.StringIO(body)))
        packet = json.loads(rows[0][3])
        packet["clock"]["end"] = "0"
        rows[0][2:4] = ["0", json.dumps(packet)]
        stream = io.StringIO(newline="")
        csv.writer(stream, lineterminator="\n").writerows(rows)
        result = self.invoke(header=header, body=stream.getvalue(), extra_env={"JCE_FRAME_LOG_CONTEXT": "3"})
        self.assertEqual(result["raw_frame_count"], 3)
        self.assertEqual(result["frame_context_schema"], "window_viewports_clock_v3")

    def test_context_selector_requires_exact_v1_or_v2_schema(self):
        v1_header, v1_body = self.extended_capture()
        v2_header, v2_body = self.viewport_capture()
        for selector, header, body in (("1", v2_header, v2_body), ("2", v1_header, v1_body),
                                       ("2", HEADER, BODY), ("0", v2_header, v2_body),
                                       ("", v2_header, v2_body)):
            with self.subTest(selector=selector, body_version=body[:120]), \
                    self.assertRaisesRegex(RuntimeError, "context schema"):
                self.invoke(header=header, body=body, extra_env={"JCE_FRAME_LOG_CONTEXT": selector})
        for selector in ("", "0"):
            result = self.invoke(extra_env={"JCE_FRAME_LOG_CONTEXT": selector})
            self.assertIsNone(result["frame_context_schema"])
        result = self.invoke(header=v1_header, body=v1_body, extra_env={"JCE_FRAME_LOG_CONTEXT": "1"})
        self.assertEqual(result["frame_context_schema"], "window_v1")

    def test_context_v2_reuses_strict_viewport_and_full_stream_validation(self):
        for fields in ({"source_id": 0}, {"flags": 256}, {"image_count": None},
                       {"main_submits": True}, {"texture_index": 65535}):
            header, body = self.viewport_capture(viewport_updates=fields)
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                self.invoke(header=header, body=body, extra_env={"JCE_FRAME_LOG_CONTEXT": "2"})
        header, body = self.viewport_capture()
        rows = list(csv.reader(io.StringIO(body)))
        packet = json.loads(rows[-2][3])
        packet["v"] = 1
        del packet["viewport_reported"], packet["viewports"]
        rows[-2][3] = json.dumps(packet)
        stream = io.StringIO(newline="")
        csv.writer(stream, lineterminator="\n").writerows(rows)
        with self.assertRaisesRegex(ValueError, "mixed frame context"):
            self.invoke(header=header, body=stream.getvalue(), extra_env={"JCE_FRAME_LOG_CONTEXT": "2"})

    def test_context_invalid_selector_preserves_prior_evidence_before_launch(self):
        self.log.write_bytes(b"prior log")
        self.csv.write_bytes(b"prior CSV")
        with mock.patch.object(perf_bench, "run_process") as child:
            for selector in ("4", "03", "3.0", " 3", "02", "2.0", " 2", 2, 3, True):
                with self.subTest(selector=selector), self.assertRaisesRegex(ValueError, "CONTEXT"):
                    self.invoke(extra_env={"JCE_FRAME_LOG_CONTEXT": selector})
                self.assertEqual(self.log.read_bytes(), b"prior log")
                self.assertEqual(self.csv.read_bytes(), b"prior CSV")
            child.assert_not_called()

    def test_same_owner_old_context_presence_control_fails_ordinary_assertion(self):
        source = Path(perf_bench.__file__).read_text(encoding="utf-8")
        before = 'if samples["context_schema"] != expected_schema:'
        self.assertEqual(source.count(before), 1)
        owner = self.root / "old_context_owner.py"
        owner.write_text(source.replace(before,
            'if (samples["context_schema"] is not None) != (selector == "1"):', 1),
            encoding="utf-8", newline="\n")
        script = ("import importlib.util, sys, unittest\n"
                  "from tools.tests import test_perf_bench as controls\n"
                  "spec=importlib.util.spec_from_file_location('old_context_owner',sys.argv[1])\n"
                  "owner=importlib.util.module_from_spec(spec)\n"
                  "spec.loader.exec_module(owner)\n"
                  "controls.perf_bench=owner\n"
                  "suite=unittest.TestSuite([controls.RawCsvAdmissionControls("
                  "'test_context_selector_requires_exact_v1_or_v2_schema')])\n"
                  "result=unittest.TextTestRunner(verbosity=2).run(suite)\n"
                  "raise SystemExit(0 if result.wasSuccessful() else 1)\n")
        result = subprocess.run([sys.executable, "-B", "-c", script, str(owner)],
                                cwd=ROOT, capture_output=True, text=True, timeout=30)
        output = result.stdout + result.stderr
        (self.root / "old_context.log").write_text(output, encoding="utf-8", newline="\n")
        self.assertEqual(result.returncode, 1, output)
        self.assertIn("FAILED (failures=2)", output)
        self.assertNotIn("ERROR:", output)
        self.assertIn("RuntimeError not raised", output)

    def test_same_owner_old_header_control_fails_ordinary_assertion(self):
        source = Path(perf_bench.__file__).read_text(encoding="utf-8")
        before = 'samples_path.stat().st_size - samples["csv_header_bytes"]'
        self.assertEqual(source.count(before), 1)
        owner = self.root / "old_header_owner.py"
        owner.write_text(source.replace(before,
            'samples_path.stat().st_size - len(b"frame_index,phase,ms\\n")', 1),
            encoding="utf-8", newline="\n")
        script = ("import importlib.util, sys, unittest\n"
                  "from tools.tests import test_perf_bench as controls\n"
                  "spec=importlib.util.spec_from_file_location('old_header_owner',sys.argv[1])\n"
                  "owner=importlib.util.module_from_spec(spec)\n"
                  "spec.loader.exec_module(owner)\n"
                  "controls.perf_bench=owner\n"
                  "suite=unittest.TestSuite([controls.RawCsvAdmissionControls("
                  "'test_extended_header_uses_actual_payload_accounting')])\n"
                  "result=unittest.TextTestRunner(verbosity=2).run(suite)\n"
                  "raise SystemExit(0 if result.wasSuccessful() else 1)\n")
        result = subprocess.run([sys.executable, "-B", "-c", script, str(owner)],
            cwd=ROOT, capture_output=True, text=True, timeout=30)
        output = result.stdout + result.stderr
        (self.root / "old_header.log").write_text(output, encoding="utf-8", newline="\n")
        self.assertEqual(result.returncode, 1, output)
        self.assertIn("FAILED (failures=1)", output)
        self.assertNotIn("ERROR:", output)
        self.assertIn("extended capture must use its validated header", output)

    def test_effective_frame_budget_and_explicit_expectation_are_honored(self):
        result = self.invoke(extra_env={"JCE_MAX_FRAMES": "3"})
        self.assertEqual(result["raw_frame_count"], 3)
        with self.assertRaisesRegex((RuntimeError, ValueError), "frame"):
            self.invoke(extra_env={"JCE_MAX_FRAMES": "4"})
        result = self.invoke(extra_env={"JCE_MAX_FRAMES": "4"}, expected=3)
        self.assertEqual(result["raw_frame_count"], 3)

    def test_timed_run_requires_actual_limit_marker_and_keeps_duration(self):
        environment = {"JCE_MAX_SECONDS": "1", "JCE_MAX_FRAMES": "2000000000"}
        result = self.invoke(extra_env=environment,
            log=BACKEND + WORKLOAD + self.writer() + "JCE_MAX_SECONDS reached\n")
        self.assertEqual(result["max_seconds_requested"], 1.0)
        self.assertEqual(result["capture_scope"], "declared_writer_window")
        self.assertFalse(result["timed_horizon_checked"])
        with self.assertRaisesRegex((RuntimeError, ValueError), "timed|MAX_SECONDS"):
            self.invoke(extra_env=environment)

    def test_opt_out_requires_deliberate_log_off_and_csv(self):
        for environment, csv in (({"JCE_PERF_LOG": "1"}, True), ({}, False)):
            with self.subTest(environment=environment, csv=csv):
                with self.assertRaises(ValueError):
                    self.invoke(extra_env=environment, csv=csv)

    def test_invalid_opt_out_never_launches_or_erases_prior_evidence(self):
        self.log.write_bytes(b"old independent log")
        self.csv.write_bytes(b"old independent raw")
        cases = (({"JCE_PERF_LOG": "1"}, self.csv, None),
                 ({"JCE_PERF_LOG": "0"}, None, None),
                 ({"JCE_PERF_LOG": "false"}, self.csv, None),
                 ({"JCE_PERF_LOG": "0"}, self.csv, True),
                 ({"JCE_PERF_LOG": "0"}, self.csv, 0),
                 ({"JCE_PERF_LOG": "0"}, self.csv, 3.5))
        with mock.patch.object(perf_bench, "run_process") as child:
            for environment, path, expected in cases:
                with self.subTest(environment=environment, expected=expected):
                    with self.assertRaises(ValueError):
                        perf_bench.run(environment, self.log, frames=3,
                            exe=self.root / "owned_fixture.exe", samples_path=path,
                            require_perf_log=False, expected_frame_count=expected)
                    self.assertEqual(self.log.read_bytes(), b"old independent log")
                    self.assertEqual(self.csv.read_bytes(), b"old independent raw")
            child.assert_not_called()

    def test_stale_files_are_removed_and_missing_fresh_outputs_rejected(self):
        self.log.write_text(BACKEND + WORKLOAD + self.writer(), encoding="utf-8")
        self.csv.write_text(HEADER + BODY, encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "no log"):
            self.invoke(write_log=False, write_csv=False)
        self.assertFalse(self.log.exists())
        self.assertFalse(self.csv.exists())
        with self.assertRaisesRegex(RuntimeError, "raw samples"):
            self.invoke(write_csv=False)

    def test_shadowed_invalid_duration_preserves_prior_evidence(self):
        for duration in (float("nan"), 0, 86401):
            with self.subTest(duration=duration):
                self.log.write_bytes(b"old independent log")
                self.csv.write_bytes(b"old independent raw")
                environment = dict(self.env, JCE_MAX_SECONDS="1")
                with mock.patch.object(perf_bench, "run_process") as child:
                    with self.assertRaises(ValueError):
                        perf_bench.run(environment, self.log, frames=3,
                            exe=self.root / "owned_fixture.exe", samples_path=self.csv,
                            require_perf_log=False, max_seconds=duration)
                    child.assert_not_called()
                self.assertEqual(self.log.read_bytes(), b"old independent log")
                self.assertEqual(self.csv.read_bytes(), b"old independent raw")

    def test_malformed_short_duplicate_and_nonfinite_csv_fail(self):
        bodies = ("", "0,frame_wall,1\n", "1,frame_wall,1\n2,frame_wall,2\n3,frame_wall,3\n",
                  "0,frame_wall,1\n2,frame_wall,2\n", "0,frame_wall,1\n0,frame_wall,1\n",
                  "0,frame_wall,-1\n", "0,frame_wall,nan\n", "0,frame_wall,1\n0,ecs_move,inf\n")
        for body in bodies:
            with self.subTest(body=body):
                with self.assertRaises((RuntimeError, ValueError)):
                    self.invoke(body=body)
        with self.assertRaises((RuntimeError, ValueError)):
            self.invoke(header="wrong,header\n")

    def test_missing_duplicate_incomplete_or_mismatched_writer_fails(self):
        invalid = ("", self.writer() * 2, self.writer(accepted=2),
                   self.writer(written=2), self.writer(accepted_bytes=1),
                   self.writer(written_bytes=1), "frame KPI capture incomplete: failure\n")
        for writer in invalid:
            with self.subTest(writer=writer):
                with self.assertRaises((RuntimeError, ValueError)):
                    self.invoke(log=BACKEND + WORKLOAD + writer)

    def test_writer_summary_in_mirrored_capture_is_counted_once(self):
        text = BACKEND + WORKLOAD + self.writer()
        result = self.invoke(log=text, process={"stderr": text})
        self.assertEqual(result["kpi_writer"]["written"], 3)

    def test_native_exit_timeout_backend_workload_error_and_lock_stay_strict(self):
        for process in ({"returncode": 7}, {"timed_out": True}):
            with self.subTest(process=process):
                with self.assertRaises(RuntimeError):
                    self.invoke(process=process)
        logs = (WORKLOAD + self.writer(),
                BACKEND.replace("Direct3D 11", "OpenGL") + WORKLOAD + self.writer(),
                BACKEND + self.writer(), BACKEND + WORKLOAD * 2 + self.writer(),
                BACKEND + WORKLOAD + self.writer() + "] ERROR - failure\n",
                BACKEND + WORKLOAD + self.writer() + "single-instance lock already held\n")
        for text in logs:
            with self.subTest(text=text):
                with self.assertRaises(RuntimeError):
                    self.invoke(log=text)

    def test_arbitrary_runtime_error_is_never_swallowed(self):
        error = RuntimeError("distinct child failure")
        for require in (True, False):
            with self.subTest(require=require):
                with self.assertRaises(RuntimeError) as caught:
                    self.invoke(require=require, error=error)
                self.assertIs(caught.exception, error)


class PeriodicMemoryControls(unittest.TestCase):
    FIELDS = ("gpu_mem_mb", "tex_mb", "rt_mb", "rss_mb", "commit_mb")

    @staticmethod
    def line(*, gpu="12", tex="10", rt="2", rss="20", commit="30"):
        return ("engine: perf: 1.5 ms avg (667 FPS) | cpu 1.0 / gpu 0.5 ms | 10 draws | "
                f"gpu-mem {gpu} MiB (tex {tex} + rt {rt}) | rss {rss} MiB | commit {commit} MiB"
                " | worst 2.0 ms / 120 | cpu_scope=bgfx_frame_interval gpu_scope=latched_backend"
                " gpu_mem_scope=backend_reported_usage_not_tex_rt_sum"
                " process_mem_scope=mimalloc_process_provider at jce_engine.c:2300\n")

    def test_legacy_positive_values_keep_binary_units_and_raw_label(self):
        sample = perf_bench.parse_perf(PERF)
        self.assertEqual([sample[key] for key in self.FIELDS], [12, 10, 2, 20, 30])
        self.assertEqual(sample["memory_unit"], "MiB")
        self.assertEqual(sample["memory_log_unit"], "MB")
        self.assertEqual(set(sample["memory_availability"].values()), {"AVAILABLE"})
        self.assertEqual(sample["memory_scope"],
                         {"gpu_mem_scope": None, "process_mem_scope": None})

    def test_unknown_memory_values_remain_unavailable(self):
        sample = perf_bench.parse_perf(self.line(gpu="N/A", tex="N/A", rt="N/A",
                                                rss="N/A", commit="N/A"))
        self.assertEqual(sample["frame_ms"], 1.5)
        for key in self.FIELDS:
            self.assertIsNone(sample[key], "unknown memory must remain unavailable")
            self.assertEqual(sample["memory_raw"][key], "N/A")
            self.assertEqual(sample["memory_availability"][key], "UNAVAILABLE")

    def test_modern_zero_is_known_independently_for_every_memory_field(self):
        sample = perf_bench.parse_perf(self.line(gpu="0", tex="0", rt="0", rss="0", commit="0"))
        for key in self.FIELDS:
            self.assertEqual(sample[key], 0, "known zero must remain available")
            self.assertEqual(sample["memory_availability"][key], "AVAILABLE")
        self.assertEqual(sample["memory_log_unit"], "MiB")
        self.assertEqual(sample["memory_scope"]["process_mem_scope"], "mimalloc_process_provider")

    def test_legacy_zero_cannot_certify_known_memory(self):
        line = PERF.replace("12 MB", "0 MB").replace("tex 10", "tex 0")
        line = line.replace("rt 2", "rt 0").replace("rss 20", "rss 0").replace("commit 30", "commit 0")
        sample = perf_bench.parse_perf(line)
        for key in self.FIELDS:
            self.assertIsNone(sample[key], "legacy zero availability is ambiguous")
            self.assertEqual(sample["memory_raw"][key], "0")
            self.assertEqual(sample["memory_availability"][key], "LEGACY_ZERO_AMBIGUOUS")

    def test_last_sample_keeps_independent_mixed_availability(self):
        sample = perf_bench.parse_perf(PERF + self.line(gpu="N/A", tex="0", rt="3", rss="N/A"))
        self.assertEqual(sample["samples"], 2)
        self.assertIsNone(sample["gpu_mem_mb"])
        self.assertEqual(sample["tex_mb"], 0)
        self.assertEqual(sample["rt_mb"], 3)
        self.assertIsNone(sample["rss_mb"])
        self.assertEqual(sample["commit_mb"], 30)

    def test_invalid_tokens_units_and_incomplete_samples_are_not_observations(self):
        invalid = (self.line(gpu="-1"), self.line(rss="nan"), self.line(tex="unknown"),
                   self.line().replace("rss 20 MiB", "rss 20 MB"),
                   self.line().replace("commit 30 MiB", "commit 30"),
                   self.line().replace("1.5 ms avg", "1.5.6 ms avg"),
                   self.line().replace("gpu-mem 12 MiB", "gpu-mem 12 GB"),
                   "engine: perf: 1.5 ms/frame avg (667 FPS), worst 2.0 ms, over 120 frames\n")
        for line in invalid:
            with self.subTest(line=line):
                self.assertIsNone(perf_bench.parse_perf(line))

    def test_ordinary_same_owner_controls_reject_fabricated_availability(self):
        owner = Path(inspect.getfile(perf_bench.parse_perf))
        source = owner.read_text(encoding="utf-8")
        controls = (
            ("unknown-zero", 'return None, "UNAVAILABLE"', 'return 0, "AVAILABLE"',
             "test_unknown_memory_values_remain_unavailable", "unknown memory must remain unavailable"),
            ("legacy-zero", 'return None, "LEGACY_ZERO_AMBIGUOUS"', 'return 0, "AVAILABLE"',
             "test_legacy_zero_cannot_certify_known_memory", "legacy zero availability is ambiguous"),
            ("erase-zero", 'return value, "AVAILABLE"', 'return (None if value == 0 else value), "AVAILABLE"',
             "test_modern_zero_is_known_independently_for_every_memory_field", "known zero must remain available"))
        scratch = perf_bench.ROOT / "build/tests" / ("perf-memory-controls-" + uuid.uuid4().hex[:12])
        scratch.mkdir(parents=True)
        outcomes = []
        for name, before, after, method, marker in controls:
            self.assertEqual(source.count(before), 1)
            mutated = scratch / (name + ".py")
            mutated.write_text(source.replace(before, after, 1), encoding="utf-8", newline="\n")
            script = ("import importlib.util, sys, unittest\n"
                      "from tools.tests import test_perf_bench as controls\n"
                      "spec = importlib.util.spec_from_file_location('memory_mutant', sys.argv[1])\n"
                      "owner = importlib.util.module_from_spec(spec)\n"
                      "spec.loader.exec_module(owner)\n"
                      "controls.perf_bench.parse_perf = owner.parse_perf\n"
                      "suite = unittest.TestSuite([controls.PeriodicMemoryControls(sys.argv[2])])\n"
                      "result = unittest.TextTestRunner(verbosity=2).run(suite)\n"
                      "raise SystemExit(0 if result.wasSuccessful() else 1)\n")
            result = subprocess.run([sys.executable, "-B", "-c", script, str(mutated), method],
                                    cwd=perf_bench.ROOT, capture_output=True, text=True, timeout=30)
            output = result.stdout + result.stderr
            (scratch / (name + ".log")).write_text(output, encoding="utf-8", newline="\n")
            outcomes.append({"name": name, "returncode": result.returncode, "method": method})
            self.assertEqual(result.returncode, 1, output)
            self.assertIn("FAILED (failures=", output)
            self.assertNotIn("ERROR:", output)
            self.assertIn(marker, output)
        (scratch / "controls.json").write_text(json.dumps(outcomes, indent=2) + "\n", encoding="utf-8")
        print("memory parser controls: 3 ordinary assertion failures; evidence=" + str(scratch))


if __name__ == "__main__":
    unittest.main()
