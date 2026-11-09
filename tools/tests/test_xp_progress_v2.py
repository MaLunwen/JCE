"""XP jce.py and embedded scripting closure, compiler-free unit controls."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]

def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

XP = load("jce_xp_live_stage", "tools/build/xp_editor.py")
DRIVER = load("jce_xp_unified_driver", "tools/build/jce.py")

class XpLiveProgressTests(unittest.TestCase):
    def test_windows_shim_routes_to_jce_py(self):
        text = (ROOT / "scripts/windows/build-editor.bat").read_text(encoding="utf-8").lower()
        self.assertIn('"%~dp0..\\jce.py" editor %*', text)
        self.assertNotIn("xp_editor.py", text)

    def test_unified_xp_jobs_and_phase(self):
        parser = DRIVER.build_parser()
        args = parser.parse_args(["editor", "--xp", "--arch", "x64", "--jobs", "2", "--phase", "dependencies"])
        with tempfile.TemporaryDirectory() as tmp:
            tc = Path(tmp)
            (tc / "bin").mkdir()
            xp = SimpleNamespace(resolve_toolchain_root=lambda root, supplied: tc)
            with patch.object(DRIVER, "HOST", "windows"), patch.object(DRIVER, "DRY_RUN", True), \
                 patch.object(DRIVER, "_xp_tool", return_value=xp), \
                 patch.object(DRIVER, "host_tool", side_effect=lambda name: tc / (name + ".exe")), \
                 patch.object(DRIVER, "cmd_xp_editor") as forward, \
                 patch.dict(os.environ, {"JCE_SHADERC_EXECUTABLE":"", "BGFX_SHADERC":""}):
                args.func(args)
        kw = forward.call_args.args[0]
        self.assertEqual((kw.arch, kw.jobs, kw.phase), ("x64", 2, "dependencies"))

    def test_no_xp_options_for_normal_editor(self):
        parser = DRIVER.build_parser()
        args = parser.parse_args(["editor", "--jobs", "2"])
        with patch.object(DRIVER, "die", side_effect=ValueError("XP only")), \
             patch.object(DRIVER, "cmd_editor") as native:
            with self.assertRaisesRegex(ValueError, "XP only"):
                args.func(args)
            native.assert_not_called()

    def test_real_stream_visible_and_full_receipts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stages = XP._Stages(root, {"stages": []}, heartbeat_seconds=0.02)
            child = "import sys,time;print('[1/2] compile',flush=True);time.sleep(.10);" \
                    "print('[2/2] link',flush=True);print('diagnostic',file=sys.stderr,flush=True)"
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                result = stages.run("build", [sys.executable, "-u", "-c", child], root, {})
            self.assertIn("[1/2] compile", out.getvalue())
            self.assertIn("[2/2] link", out.getvalue())
            self.assertIn("running", out.getvalue())
            self.assertEqual(result.returncode, 0)
            self.assertEqual(stages.receipt["stages"][0]["state"], "SUCCEEDED")
            self.assertEqual(Path(stages.receipt["stages"][0]["stderr"]["path"]).read_text().strip(), "diagnostic")
            self.assertIn("[1/2] compile", Path(stages.receipt["stages"][0]["stdout"]["path"]).read_text())

    def test_conan_json_not_spammed_but_is_byte_perfect(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stages = XP._Stages(root, {"stages": []}, heartbeat_seconds=2)
            data = '{"graph":{"nodes":[]}}'
            code = "import sys;sys.stdout.write(" + repr(data) + ");sys.stderr.write('conan package built\\n')"
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                r = stages.run("dependencies", [sys.executable, "-u", "-c", code, "--format=json"], root, {})
            self.assertEqual(r.stdout, data)
            self.assertNotIn(data, out.getvalue())
            self.assertIn("conan package built", out.getvalue())

    def test_real_nonzero_failure_prints_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stages = XP._Stages(root, {"stages": []}, heartbeat_seconds=2)
            code = "import sys;print('FAILED: link',flush=True);print('fatal error: symbol', file=sys.stderr,flush=True);sys.exit(9)"
            out = io.StringIO()
            with contextlib.redirect_stdout(out), self.assertRaises(subprocess.CalledProcessError):
                stages.run("build", [sys.executable, "-u", "-c", code], root, {})
            self.assertIn("fatal error", out.getvalue())
            self.assertEqual(stages.receipt["stages"][0]["state"], "FAILED")
            self.assertEqual(stages.receipt["stages"][0]["returncode"], 9)

    def test_injected_runner_keeps_input_data_and_fd0_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            seen = []
            def fake(cmd, **kwargs):
                seen.append(kwargs)
                return subprocess.CompletedProcess(cmd, 0, "test", "")
            stages = XP._Stages(root, {"stages": []}, fake)
            stages.run("dependencies", ["conan", "install"], root, {})
            stages.run("preflight", ["clang", "-E", "-"], root, {}, input_data="")
            self.assertEqual(seen[0]["stdin"], subprocess.DEVNULL)
            self.assertIsNone(seen[1]["stdin"])

    def test_host_tools_disable_external_script_vms(self):
        import inspect
        source = inspect.getsource(DRIVER.ensure_host_tools)
        for key in ("VM_PYTHON", "CSHARP", "JAVA"):
            self.assertIn('"-DJCE_BUILD_SCRIPT_'+key+'=OFF"', source)

if __name__ == "__main__":
    unittest.main()
