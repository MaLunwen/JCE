"""CPU controls for realtime lint progress; no native toolchain required."""
from __future__ import annotations
from contextlib import redirect_stdout
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("jce_lint_progress_v6", ROOT / "tools/lint/run_all.py")
OWNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(OWNER)

class LintRealtimeControls(unittest.TestCase):
    def test_serial_order_immediate_verdict_skip_and_failure_transcript(self):
        with tempfile.TemporaryDirectory() as tmp, io.StringIO() as output:
            labels = [("tools/tests/test_shader_lint.py", []), ("tools/tests/test_cache_lock.py", []),
                      ("tools/tests/test_bundle_command.py", [])]
            answers = [(labels[0][0], 0, "good"), (labels[1][0], 2, "NOT_MEASURED: absent"),
                       (labels[2][0], 1, "FAIL: broken fixture\n")]
            log = Path(tmp) / "lint-failure.txt"
            with mock.patch.object(OWNER, "LINTS", labels), \
                 mock.patch.object(OWNER, "DEFAULT_LOG", log), \
                 mock.patch.object(OWNER, "run_one", side_effect=answers) as run, \
                 redirect_stdout(output):
                code = OWNER.main(["--progress-interval", "0.05"])
            self.assertEqual(code, 1)
            self.assertEqual(run.call_count, 3)
            stdout = output.getvalue()
            self.assertLess(stdout.index("BEGIN " + labels[0][0]), stdout.index("[PASS] " + labels[0][0]))
            self.assertLess(stdout.index("[PASS] " + labels[0][0]), stdout.index("BEGIN " + labels[1][0]))
            self.assertIn("[SKIP] " + labels[1][0], stdout)
            self.assertIn("[FAIL (1)] " + labels[2][0], stdout)
            self.assertIn("DIAGNOSTIC FAIL: broken fixture", stdout)
            self.assertIn("FAIL: broken fixture", log.read_text(encoding="utf-8"))
            self.assertIn("1 lint(s) SKIPPED", stdout)

    def test_heartbeat_during_long_running_gate(self):
        with tempfile.TemporaryDirectory() as tmp, io.StringIO() as output:
            entry = ("tools/tests/test_shader_lint.py", [])
            def slow(*args, **kwargs):
                time.sleep(0.13)
                return entry[0], 0, "pass"
            with mock.patch.object(OWNER, "LINTS", [entry]), \
                 mock.patch.object(OWNER, "DEFAULT_LOG", Path(tmp) / "previous.log"), \
                 mock.patch.object(OWNER, "run_one", side_effect=slow), \
                 redirect_stdout(output):
                self.assertEqual(OWNER.main(["--progress-interval", "0.025"]), 0)
            self.assertIn("RUNNING", output.getvalue())
            self.assertIn("[PASS]", output.getvalue())

    def test_live_child_tee_preserves_raw_output_and_exit(self):
        script = ROOT / "tools/tests/test_shader_lint.py"
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / "lint_probe.py"
            probe.write_text("import sys;print('HELLO STDOUT',flush=True);"
                             "print('HELLO STDERR',file=sys.stderr,flush=True);sys.exit(3)\n",encoding="utf-8")
            out = io.StringIO()
            with mock.patch.object(OWNER, "resolve", return_value=(probe, [], "lint_probe")), redirect_stdout(out):
                label, code, body = OWNER.run_one(("ignored.py", []), live_output=True)
            self.assertEqual((label, code), ("lint_probe", 3))
            self.assertIn("HELLO STDOUT", body)
            self.assertIn("HELLO STDERR", body)
            self.assertIn("[lint stdout][lint_probe] HELLO STDOUT", out.getvalue())
            self.assertIn("[lint stderr][lint_probe] HELLO STDERR", out.getvalue())

    def test_invalid_interval_refuses_before_dispatch(self):
        with mock.patch.object(OWNER, "run_one") as run, redirect_stdout(io.StringIO()), \
             mock.patch("sys.stderr", io.StringIO()):
            for raw in ("0", "-1", "inf", "not-a-number", "3601"):
                with self.subTest(raw=raw), self.assertRaises(SystemExit):
                    OWNER.main(["--progress-interval", raw])
            run.assert_not_called()

    def test_legacy_runner_signature_preserved(self):
        # Existing mocking ports rely on subprocess.run in default mode.
        entry = ("tools/tests/test_shader_lint.py", [])
        value = subprocess.CompletedProcess([], 0, "ok\n", "")
        with mock.patch.object(OWNER.subprocess, "run", return_value=value) as run:
            label, code, out = OWNER.run_one(entry)
        self.assertEqual((code, out), (0, "ok"))
        run.assert_called_once()
        self.assertTrue(run.call_args.kwargs["capture_output"])

if __name__ == "__main__":
    unittest.main()
