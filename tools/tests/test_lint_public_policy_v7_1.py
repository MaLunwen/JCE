"""One public lint registry and ordinary public AI/automation paths."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(relative, label):
    spec = importlib.util.spec_from_file_location(label, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class PublicLintPolicyV71(unittest.TestCase):
    def test_one_committed_public_lint_inventory(self):
        owner = load("tools/lint/run_all.py", "public_lint_v71")
        names = [owner.resolve(entry)[2] for entry in owner.LINTS]
        self.assertFalse(any("private/" in name for name in names), names)
        self.assertNotIn("check_private_tech_untracked.py", names)
        self.assertIn("tools/tests/test_lint_public_policy_v7_1.py", names)
        self.assertEqual(len(names), len(set(names)))
        self.assertFalse(hasattr(owner, "_PRIVATE_CHECKS"))

    def test_ai_automation_dirs_eligible_as_public_source(self):
        data = json.loads((ROOT / "contracts/source-layout.json").read_text(encoding="utf-8"))
        self.assertEqual(data["private_implementation_roots"], ["private/"])
        self.assertEqual(data["private_roots"], [])
        ignored = (ROOT / ".gitignore").read_text(encoding="utf-8").splitlines()
        self.assertIn("/private/", ignored)  # No accidental legacy publication.
        self.assertFalse((ROOT / "tools/lint/check_private_tech_untracked.py").exists())

    def test_actual_source_layout_negative_and_positive_controls(self):
        proc = subprocess.run([sys.executable, str(ROOT / "tools/lint/check_source_layout.py"),
                               "--self-check"], cwd=ROOT, capture_output=True,
                              text=True, timeout=40)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("4 public roots, 5 negative controls", proc.stdout)

    def test_legacy_private_folder_does_not_become_tracked_automatically(self):
        # No install tool or lint registry may stage arbitrary local material.
        runner = (ROOT / "tools/lint/run_all.py").read_text(encoding="utf-8")
        self.assertNotIn('LINTS.append(("private/tools/lint/"', runner)
        self.assertNotIn("Private suite unavailable", runner)


if __name__ == "__main__":
    unittest.main()
