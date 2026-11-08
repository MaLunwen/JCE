"""Actual source-runner input/dispatch controls; no wrapper/CMake execution."""
from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("jce_lint_stock_input", ROOT / "tools/lint/run_all.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)
ENTRY = (RUNNER.STOCK_WRAPPER_ENTRY, [])


class LintStockWrapperInputTests(unittest.TestCase):
    def subject(self, root):
        path = Path(root) / "native tools with spaces" / "jce_bin2obj.exe"
        path.parent.mkdir()
        path.write_bytes(b"protocol-presence-only; never executed")
        return path

    def test_actual_main_resolves_and_dispatches_only_explicit_native_subject(self):
        with tempfile.TemporaryDirectory() as root:
            wrapper = self.subject(root)
            benchmark = ("tools/tests/test_editor_benchmark_log.py", [])
            out = io.StringIO()
            result = subprocess.CompletedProcess([], 0, "protocol child returned\n", "")
            with mock.patch.object(RUNNER, "LINTS", [ENTRY, benchmark]), \
                 mock.patch.object(RUNNER.subprocess, "run", return_value=result) as spawn, \
                 mock.patch.object(RUNNER, "DEFAULT_LOG", Path(root) / "old.log"), redirect_stdout(out):
                code = RUNNER.main(["--stock-wrapper-exe", str(wrapper)])
            self.assertEqual(code, 0)
            self.assertEqual(spawn.call_count, 2)
            self.assertEqual(spawn.call_args_list[0].args[0],
                [RUNNER.sys.executable, str(ROOT / ENTRY[0]), "--exe", str(wrapper.resolve())])
            self.assertEqual(spawn.call_args_list[1].args[0],
                [RUNNER.sys.executable, str(ROOT / benchmark[0])])
            self.assertIn("[PASS] " + ENTRY[0], out.getvalue())

    def test_missing_input_is_explicit_not_measured_without_child_invocation(self):
        out = io.StringIO()
        with tempfile.TemporaryDirectory() as root, \
             mock.patch.object(RUNNER, "LINTS", [ENTRY]), \
             mock.patch.object(RUNNER.subprocess, "run") as spawn, \
             mock.patch.object(RUNNER, "DEFAULT_LOG", Path(root) / "old.log"), redirect_stdout(out):
            code = RUNNER.main([])
        self.assertEqual(code, 0)  # Aggregate may finish; this subject is still SKIP.
        spawn.assert_not_called()
        self.assertIn("[SKIP] " + ENTRY[0], out.getvalue())
        self.assertIn("NOT_MEASURED: explicit native stock wrapper not supplied", out.getvalue())
        self.assertNotIn("usage:", out.getvalue())
        self.assertNotIn("[PASS]", out.getvalue())

    def test_bad_path_missing_value_and_unknown_option_reject_before_dispatch(self):
        with tempfile.TemporaryDirectory() as root:
            cases = (["--stock-wrapper-exe", str(Path(root) / "missing.exe")],
                     ["--stock-wrapper-exe", root], ["--stock-wrapper-exe"],
                     ["--unknown-stock-wrapper", root])
            for argv in cases:
                with self.subTest(argv=argv), mock.patch.object(RUNNER.subprocess, "run") as spawn, \
                     redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
                    with self.assertRaises(SystemExit) as caught:
                        RUNNER.main(argv)
                    self.assertEqual(caught.exception.code, 2)
                    spawn.assert_not_called()

    def test_explicit_child_exit_two_is_failure_and_cannot_become_skip(self):
        with tempfile.TemporaryDirectory() as root:
            wrapper = self.subject(root)
            result = subprocess.CompletedProcess([], 2, "", "usage: rejected explicit subject\n")
            with mock.patch.object(RUNNER.subprocess, "run", return_value=result) as spawn:
                label, code, raw = RUNNER.run_one(ENTRY, stock_wrapper_exe=wrapper)
            self.assertEqual(spawn.call_count, 1)
            self.assertEqual(code, 1)
            self.assertIn("NOT a SKIP", raw)
            self.assertIn("usage: rejected explicit subject", raw)
            self.assertIn(str(wrapper), label)

    def test_absent_private_suites_are_reported_by_cli_only(self):
        spec = importlib.util.spec_from_file_location("jce_lint_public", SPEC.origin)
        runner = importlib.util.module_from_spec(spec)
        actual_is_file = Path.is_file
        private_dir = ROOT / "private/tools/lint"

        def public_is_file(path):
            return path.parent != private_dir and actual_is_file(path)

        import_stdout, import_stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(Path, "is_file", public_is_file), \
             redirect_stdout(import_stdout), redirect_stderr(import_stderr):
            spec.loader.exec_module(runner)
        self.assertEqual(import_stdout.getvalue(), "")
        self.assertEqual(import_stderr.getvalue(), "")
        self.assertFalse(any(isinstance(entry, tuple) and entry[0].startswith("private/")
                             for entry in runner.LINTS))

        with tempfile.TemporaryDirectory() as temporary:
            out = io.StringIO()
            with mock.patch.object(runner, "LINTS", []), \
                 mock.patch.object(runner, "DEFAULT_LOG", Path(temporary) / "failure.log"), \
                 redirect_stdout(out):
                self.assertEqual(runner.main([]), 0)
        notices = [line for line in out.getvalue().splitlines()
                   if line.startswith("Private suite unavailable")]
        self.assertEqual(notices, ["Private suite unavailable (not a PASS): " + name
                                   for name in runner._PRIVATE_CHECKS])

    def test_real_child_imports_repo_outside_cwd_and_preserves_caller_environment(self):
        # Import the actual runner by filename in an isolated Python process.
        # Neither its cwd nor its initial sys.path supplies the repository root.
        host = """import importlib.util, json, os, sys
spec = importlib.util.spec_from_file_location('lint_runner_subject', sys.argv[1])
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
before = dict(os.environ)
label, code, output = runner.run_one((sys.argv[2], []))
print(json.dumps(dict(code=code, output=output, inherited_pythonpath=before.get("PYTHONPATH"),
    environment_unchanged=dict(os.environ) == before)))
"""
        child = """import importlib, json, os, sys
from pathlib import Path
from tools.build import cache_lock
caller = os.environ.get('JCE_LINT_CALLER_MODULE')
print(json.dumps(dict(module=cache_lock.__file__, cwd=os.getcwd(),
    pythonpath=os.environ.get('PYTHONPATH'), path=os.environ.get('PATH'),
    cwd_importable=any(Path(path).resolve() == Path.cwd().resolve() for path in sys.path),
    sentinel=os.environ['JCE_LINT_SENTINEL'], bytecode_disabled=sys.dont_write_bytecode,
    caller=importlib.import_module(caller).VALUE if caller else None)))
"""
        with tempfile.TemporaryDirectory() as temporary:
            outside = Path(temporary)
            probe = outside / 'scripts' / 'gate child.py'
            probe.parent.mkdir()
            probe.write_text(child, encoding='utf-8')
            caller_dir = outside / 'caller modules with spaces'
            caller_dir.mkdir()
            (caller_dir / 'jce_lint_caller.py').write_text("VALUE = 'caller-import-retained'\n", encoding='utf-8')
            for inherited in (None, '', str(caller_dir) + os.pathsep + str(outside / 'unused path')):
                env = dict(os.environ)
                env.pop('PYTHONPATH', None)
                env.pop('JCE_LINT_CALLER_MODULE', None)
                env['JCE_LINT_SENTINEL'] = 'caller-environment-retained'
                env['PYTHONSAFEPATH'] = '1'
                if inherited is not None:
                    env['PYTHONPATH'] = inherited
                if inherited:
                    env['JCE_LINT_CALLER_MODULE'] = 'jce_lint_caller'
                with self.subTest(inherited_pythonpath=inherited):
                    proc = subprocess.run([sys.executable, '-B', '-c', host, str(SPEC.origin), str(probe)],
                        cwd=outside, env=env, capture_output=True, text=True, encoding='utf-8', timeout=20)
                    self.assertEqual(proc.returncode, 0, proc.stderr)
                    result = json.loads(proc.stdout)
                    self.assertEqual(result['code'], 0, result['output'])
                    self.assertTrue(result['environment_unchanged'])
                    self.assertEqual(result['inherited_pythonpath'], inherited)
                    observed = json.loads(result['output'])
                    self.assertEqual(Path(observed['module']).resolve(), ROOT / 'tools/build/cache_lock.py')
                    self.assertEqual(Path(observed['cwd']), outside)
                    self.assertFalse(observed['cwd_importable'], observed['pythonpath'])
                    expected = str(ROOT) + (os.pathsep + inherited if inherited else '')
                    self.assertEqual(observed['pythonpath'], expected)
                    self.assertEqual(observed['path'], env.get('PATH'))
                    self.assertEqual(observed['sentinel'], env['JCE_LINT_SENTINEL'])
                    self.assertTrue(observed['bytecode_disabled'])
                    self.assertEqual(observed['caller'], 'caller-import-retained' if inherited else None)

    def test_resolver_preserves_registered_arguments_and_default_other_entry(self):
        with tempfile.TemporaryDirectory() as root:
            wrapper = self.subject(root)
            _, arguments, _ = RUNNER.resolve(ENTRY, stock_wrapper_exe=wrapper)
            self.assertEqual(arguments, ["--exe", str(wrapper)])
            self.assertEqual(ENTRY[1], [])
            other = ("tools/tests/test_editor_benchmark_log.py", ["--sentinel"])
            _, arguments, _ = RUNNER.resolve(other, stock_wrapper_exe=wrapper)
            self.assertEqual(arguments, ["--sentinel"])
            self.assertEqual(other[1], ["--sentinel"])
            self.assertEqual(RUNNER.resolve("check_release_version.py")[1], [])


if __name__ == "__main__":
    unittest.main()
