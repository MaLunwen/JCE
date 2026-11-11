"""Negative and positive controls for the source/layout owner. No native build."""
from __future__ import annotations
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("jce_build_roots_v9", ROOT / "tools/lint/check_build_output_roots.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class BuildOutputRootControls(unittest.TestCase):
    def fixture(self):
        tmp = tempfile.TemporaryDirectory(prefix="jce-build-layout-v9-")
        self.addCleanup(tmp.cleanup)
        repo = Path(tmp.name)
        contract = {
            "schema": 1, "root": "build",
            "roots": {
                "tests": {"purpose": "synthetic test workspaces", "retention": "ephemeral"},
                "host": {"purpose": "native tools", "retention": "cache"},
                "desktop": {"purpose": "variants", "retention": "cache"},
                "validation": {"purpose": "evidence", "retention": "evidence"}},
            "test_producers": {"tools/tests/test_sample.py": {"sample-tests": 1}},
        }
        output = repo / "contracts/build-output-roots.json"
        output.parent.mkdir(parents=True)
        output.write_text(json.dumps(contract), encoding="utf-8")
        source = repo / "tools/tests/test_sample.py"
        source.parent.mkdir(parents=True)
        source.write_text('from pathlib import Path\nbase = Path("build/tests/sample-tests")\n', encoding="utf-8")
        return repo, source

    def test_checked_in_owners_have_canonical_paths(self):
        self.assertEqual(GATE.source_errors(ROOT), [])

    def test_fresh_clone_and_normalized_build_are_admitted(self):
        repo, _ = self.fixture()
        self.assertEqual(GATE.existing_errors(repo), [])
        for name in ("tests", "host", "desktop", "validation"):
            (repo / "build" / name).mkdir(parents=True, exist_ok=True)
        self.assertEqual(GATE.existing_errors(repo), [])

    def test_old_and_newly_unregistered_test_workspaces_fail(self):
        repo, sample = self.fixture()
        sample.write_text('base = "build/sample-tests"\n', encoding="utf-8")
        errors = GATE.source_errors(repo)
        self.assertTrue(any("not canonical" in error for error in errors), errors)
        sample.write_text('base = "build/tests/sample-tests"\n', encoding="utf-8")
        rogue = repo / "tools/tests/test_rogue.py"
        rogue.write_text('base = "build/rogue-tests"\n', encoding="utf-8")
        errors = GATE.source_errors(repo)
        self.assertTrue(any("unregistered root-level test output" in error for error in errors), errors)

    def test_unknown_root_is_rejected_without_mutation(self):
        repo, _ = self.fixture()
        unknown = repo / "build" / "rogue-tests"
        unknown.mkdir(parents=True)
        marker = unknown / "retain.txt"
        marker.write_text("must remain", encoding="utf-8")
        errors = GATE.existing_errors(repo)
        self.assertTrue(any("unregistered" in error for error in errors), errors)
        self.assertEqual(marker.read_text(encoding="utf-8"), "must remain")

    def test_registered_owners_must_exist_and_remain_complete(self):
        repo, source = self.fixture()
        source.unlink()
        errors = GATE.source_errors(repo)
        self.assertTrue(any("missing" in error for error in errors), errors)

    def test_real_build_roots_contract_keeps_native_cache_separate(self):
        contract = json.loads((ROOT / "contracts/build-output-roots.json").read_text(encoding="utf-8"))
        self.assertEqual(contract["root"], "build")
        for name in (".locks", "automation", "compat", "conan-config", "desktop",
                     "host", "host-conan", "reports", "shader-cache", "tests", "validation"):
            self.assertIn(name, contract["roots"])
        self.assertNotIn("obj", contract["roots"])
        self.assertNotIn("conan", contract["roots"])
        self.assertEqual(len({dest for owner in contract["test_producers"].values()
                              for dest in owner}), 14)


if __name__ == "__main__":
    unittest.main()
