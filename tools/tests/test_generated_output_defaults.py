"""Prospective output roots and existing runner behavior; no native child runs."""
from contextlib import redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools import benchmark_suite as BENCHMARK


def load_owner(relative_path, name):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative_path)
    owner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(owner)
    return owner


LINT = load_owner("tools/lint/run_all.py", "output_default_lint")
AUDIT = load_owner("tools/audit/run_architecture_audit.py", "output_default_audit")


class GeneratedOutputDefaultsTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / "build" / "tests"
        scratch.mkdir(parents=True, exist_ok=True)
        holder = tempfile.TemporaryDirectory(prefix="output-defaults-", dir=scratch)
        self.addCleanup(holder.cleanup)
        self.root = Path(holder.name)
        self.legacy = self.root / "reports" / "retained-history.txt"
        self.legacy.parent.mkdir()
        self.legacy.write_bytes(b"Retained history\n")

    def assert_default_logs(self, lint=LINT, audit=AUDIT):
        self.assertEqual(lint.DEFAULT_LOG,
                         ROOT / "build" / "reports" / "lint-failure.txt")
        self.assertEqual(audit.DEFAULT_LOG,
                         ROOT / "build" / "reports" / "architecture-audit-failure.txt")

    def test_default_logs_are_under_build(self):
        self.assert_default_logs()

    def test_lint_failure_then_success_keeps_existing_transcript_lifecycle(self):
        target = self.root / LINT.DEFAULT_LOG.relative_to(ROOT)
        output = io.StringIO()
        with mock.patch.object(LINT, "DEFAULT_LOG", target), \
             mock.patch.object(LINT, "LINTS", ["synthetic.py"]), \
             mock.patch.object(LINT, "run_one", return_value=(
                 "synthetic.py", 1, "ordinary finding\nretained detail")), \
             redirect_stdout(output):
            self.assertEqual(LINT.main([]), 1)
            self.assertIn("synthetic.py   exit=1", target.read_text(encoding="utf-8"))
            self.assertIn("retained detail", target.read_text(encoding="utf-8"))
            self.assertIn(str(target), output.getvalue())
            with mock.patch.object(LINT, "run_one",
                                   return_value=("synthetic.py", 0, "clean")):
                self.assertEqual(LINT.main([]), 0)
        self.assertFalse(target.exists())
        self.assertEqual(self.legacy.read_bytes(), b"Retained history\n")

    def run_audit(self, target, *, explicit=None, code=1):
        argv = ["run_architecture_audit.py"]
        if explicit is not None:
            argv.extend(["--log", str(explicit)])
        with mock.patch.object(AUDIT, "DEFAULT_LOG", target), \
             mock.patch.object(AUDIT, "CHECKS", [(Path("synthetic.py"), ["--input"], True)]), \
             mock.patch.object(AUDIT, "run_one", return_value=(code, "audit detail")), \
             mock.patch.object(sys, "argv", argv), redirect_stdout(io.StringIO()):
            return AUDIT.main()

    def test_audit_failure_default_and_explicit_log_keep_real_diagnostics(self):
        target = self.root / AUDIT.DEFAULT_LOG.relative_to(ROOT)
        self.assertEqual(self.run_audit(target), 1)
        transcript = target.read_text(encoding="utf-8")
        self.assertIn("synthetic.py --input", transcript)
        self.assertIn("gating=True  exit=1", transcript)
        self.assertIn("audit detail", transcript)
        for code in (0, 1):
            explicit = self.root / "chosen output" / f"audit-{code}.txt"
            self.assertEqual(self.run_audit(target, explicit=explicit, code=code),
                             int(code != 0))
            self.assertIn(f"gating=True  exit={code}",
                          explicit.read_text(encoding="utf-8"))
            self.assertEqual(target.read_text(encoding="utf-8"), transcript)
        self.assertEqual(self.legacy.read_bytes(), b"Retained history\n")

    def test_audit_success_without_log_creates_no_report(self):
        target = self.root / AUDIT.DEFAULT_LOG.relative_to(ROOT)
        self.assertEqual(self.run_audit(target, code=0), 0)
        self.assertFalse(target.parent.exists())
        self.assertEqual(self.legacy.read_bytes(), b"Retained history\n")

    def test_transcript_io_error_does_not_hide_failed_gate(self):
        # An existing directory cannot be replaced by a transcript file.
        output = io.StringIO()
        with mock.patch.object(LINT, "DEFAULT_LOG", self.root), \
             mock.patch.object(LINT, "LINTS", ["synthetic.py"]), \
             mock.patch.object(LINT, "run_one",
                               return_value=("synthetic.py", 1, "finding")), \
             redirect_stdout(output):
            self.assertEqual(LINT.main([]), 1)
        self.assertIn("could not write transcript", output.getvalue())
        self.assertEqual(self.run_audit(self.root), 1)
        self.assertEqual(self.legacy.read_bytes(), b"Retained history\n")

    def run_benchmark(self, *, owner=BENCHMARK, explicit=None):
        exe = self.root / "presence-only.exe"
        exe.write_bytes(b"Never executed\n")
        argv = ["--exe", str(exe), "--profile", "quick", "--kind", "entity"]
        if explicit is not None:
            argv.extend(["--out", str(explicit)])
        calls = []

        def measured(**kwargs):
            calls.append(kwargs)
            return {"kind": kwargs["kind"], "variant": kwargs["variant"],
                    "run_index": kwargs["index"], "run_id": kwargs["directory"].name,
                    "status": "PASS", "frame": {
                        "frame_median_ms": 10.0, "frame_p99_ms": 12.0}}

        with mock.patch.object(owner, "ROOT", self.root), \
             mock.patch.object(owner, "source_identity", return_value={"status": "UNAVAILABLE"}), \
             mock.patch.object(owner, "machine_manifest", return_value={}), \
             mock.patch.object(owner, "run_control", return_value={"status": "PASS"}) as control, \
             mock.patch.object(owner, "run_one", side_effect=measured), \
             redirect_stdout(io.StringIO()):
            self.assertEqual(owner.main(argv), 0)
        return calls[0], control.call_args.kwargs

    def assert_benchmark_output(self, *, owner=BENCHMARK, explicit=None):
        expected = (explicit or self.root / "build" / "validation" / "benchmark").resolve()
        first, control_a = self.run_benchmark(owner=owner, explicit=explicit)
        second, control_b = self.run_benchmark(owner=owner, explicit=explicit)
        for measured, control in ((first, control_a), (second, control_b)):
            self.assertEqual(measured["out"], expected)
            self.assertEqual(measured["directory"].parent, expected)
            self.assertEqual(control["directory"].parent, expected / "controls")
            self.assertEqual(measured["scene"].parent, expected / "fixtures")
        self.assertNotEqual(first["directory"], second["directory"])
        self.assertNotEqual(control_a["directory"], control_b["directory"])
        self.assertEqual(first["scene"], second["scene"])
        suites = list(expected.glob("suite-*.json"))
        self.assertEqual(len(suites), 2)
        ids = [json.loads(path.read_text(encoding="utf-8"))["suite_id"] for path in suites]
        self.assertEqual(len(set(ids)), 2)
        self.assertEqual(self.legacy.read_bytes(), b"Retained history\n")

    def test_benchmark_default_retains_unique_runs_and_immutable_fixture_reuse(self):
        self.assert_benchmark_output()

    def test_benchmark_explicit_out_overrides_default(self):
        self.assert_benchmark_output(explicit=self.root / "chosen output")
        self.assertFalse((self.root / "build" / "validation" / "benchmark").exists())

    def mutated_owner(self, owner, before, after):
        source = Path(owner.__file__).read_text(encoding="utf-8")
        self.assertEqual(source.count(before), 1)
        mutated = types.ModuleType("generated_output_mutant")
        mutated.__file__ = owner.__file__
        mutated.__package__ = getattr(owner, "__package__", None)
        exec(compile(source.replace(before, after, 1), owner.__file__, "exec"),
             mutated.__dict__)
        return mutated

    def test_former_root_report_defaults_fail_ordinary_contract_assertions(self):
        for owner, basename in ((LINT, "lint-failure.txt"),
                                (AUDIT, "architecture-audit-failure.txt")):
            mutant = self.mutated_owner(
                owner, 'REPO_ROOT / "build" / "reports" / "' + basename + '"',
                'REPO_ROOT / "reports" / "' + basename + '"')
            with self.subTest(basename=basename), self.assertRaises(AssertionError):
                self.assert_default_logs(lint=mutant if owner is LINT else LINT,
                                         audit=mutant if owner is AUDIT else AUDIT)

    def test_former_benchmark_default_fails_actual_main_output_assertions(self):
        mutant = self.mutated_owner(
            BENCHMARK, 'default=ROOT / "build" / "validation" / "benchmark"',
            'default=ROOT / "reports" / "benchmark"')
        with self.assertRaises(AssertionError):
            self.assert_benchmark_output(owner=mutant)

    def test_ignored_explicit_benchmark_out_fails_actual_main_output_assertions(self):
        mutant = self.mutated_owner(
            BENCHMARK, "out = args.out.resolve()",
            'out = (ROOT / "build" / "validation" / "benchmark").resolve()')
        with self.assertRaises(AssertionError):
            self.assert_benchmark_output(owner=mutant, explicit=self.root / "chosen output")


if __name__ == "__main__":
    unittest.main()
