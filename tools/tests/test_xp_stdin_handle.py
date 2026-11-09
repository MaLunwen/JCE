"""Regression tests for XP Editor noninteractive subprocess stdin."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tools/build/xp_editor.py"
SPEC = importlib.util.spec_from_file_location("jce_xp_editor_stdin_test", MODULE_PATH)
XP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(XP)


class XpEditorStdinHandleTests(unittest.TestCase):
    def run_case(self, *, input_data=None):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            observed = []

            def fake_runner(command, **kwargs):
                observed.append((command, kwargs))
                return subprocess.CompletedProcess(command, 0, "fixture output", "")

            stages = XP._Stages(directory, {"stages": []}, fake_runner)
            stages.run("dependencies", ["conan", "install", "fixture"], directory, {},
                       input_data=input_data)
            self.assertEqual(len(observed), 1)
            self.assertTrue((directory / "receipt.json").is_file())
            self.assertEqual(stages.receipt["stages"][0]["state"], "SUCCEEDED")
            self.assertEqual(observed[0][1]["input"], input_data)
            return observed[0][1]

    def test_noninteractive_conan_has_valid_stdin_handle(self):
        arguments = self.run_case()
        self.assertEqual(arguments["stdin"], subprocess.DEVNULL)
        self.assertTrue(arguments["capture_output"])
        self.assertEqual(arguments["creationflags"],
                         getattr(subprocess, "CREATE_NO_WINDOW", 0))

    def test_explicit_stdin_input_retains_subprocess_pipe_behavior(self):
        arguments = self.run_case(input_data="")
        self.assertIsNone(arguments["stdin"])
        self.assertEqual(arguments["input"], "")

    def test_real_child_has_valid_fd_zero(self):
        # Checks the actual spawned child, not merely mocked kwargs. On Windows
        # the XP producer uses CREATE_NO_WINDOW; without a supplied STDIN
        # handle that mode may leave fd 0 invalid for MSYS2 Libtool.
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            stages = XP._Stages(directory, {"stages": []})
            result = stages.run("stdin-probe", [sys.executable, "-c",
                "import os; os.fstat(0); print('FD0_VALID')"], directory, {})
            self.assertEqual(result.returncode, 0)
            self.assertIn("FD0_VALID", result.stdout)


if __name__ == "__main__":
    unittest.main()
