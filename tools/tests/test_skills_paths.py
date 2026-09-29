"""Check skill paths without platform-specific ellipsis normalization."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("jce_skills_paths", ROOT / "tools/lint/check_skills.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class SkillsPathTests(unittest.TestCase):
    def check_body(self, root, text):
        findings, notes = [], []
        skill = root / "skills/jce"
        with patch.object(GATE, "REPO_ROOT", root), patch.object(GATE, "source_blob", return_value=""):
            GATE._check_body(skill / "SKILL.md", skill, text, {"jce"}, set(), findings, notes)
        return findings, notes

    def test_placeholder_fails_even_when_filesystem_reports_it_exists(self):
        for path in ("engine/include/...", "engine/src/.../module.c"):
            with self.subTest(path=path), patch.object(Path, "exists", return_value=True):
                findings, _ = self.check_body(Path("fixture"), f"Declaration: `{path}`")
                self.assertEqual(len(findings), 1)
                self.assertIn("placeholder", findings[0])

    def test_real_repository_path_passes_and_missing_source_still_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "engine/include/jce").mkdir(parents=True)
            (root / "engine/include/jce/api.h").write_text("/* fixture */\n")
            findings, _ = self.check_body(root, "Header: `engine/include/jce/api.h`")
            self.assertEqual(findings, [])
            findings, _ = self.check_body(root, "Header: `engine/include/jce/missing.h`")
            self.assertEqual(len(findings), 1)
            self.assertIn("does not exist", findings[0])

    def test_chinese_edition_uses_its_declared_language_trigger(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            skill = root / "skills/jce-zh-cn"
            skill.mkdir(parents=True)
            entry = "---\nname: jce-zh-cn\ndescription: 用于简体中文 JCE 指南。\nmetadata:\n  language: zh-CN\n---\n\n# JCE\n"
            (skill / "SKILL.md").write_text(entry, encoding="utf-8")
            with patch.object(GATE, "source_blob", return_value=""):
                findings, _ = GATE.check_skill(skill, {"jce-zh-cn"})
            self.assertEqual(findings, [])
            (skill / "SKILL.md").write_text(entry.replace("用于", "任意"), encoding="utf-8")
            with patch.object(GATE, "source_blob", return_value=""):
                findings, _ = GATE.check_skill(skill, {"jce-zh-cn"})
            self.assertEqual(len(findings), 1)
            self.assertIn("triggering conditions", findings[0])

    def test_missing_generated_path_remains_note_not_pass_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            findings, notes = self.check_body(Path(tmp), "SDK output: `dist/sdk`")
            self.assertEqual(findings, [])
            self.assertEqual(len(notes), 1)
            self.assertIn("generated or gitignored", notes[0])


if __name__ == "__main__":
    unittest.main()
