"""Validate bilingual, deterministic skill archives and tamper detection."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("jce_pack_skill_test", ROOT / "tools/pack_skill.py")
PACK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACK)


class SkillPackageTests(unittest.TestCase):
    def test_context_commit_does_not_change_archive_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ("jce", "jce-zh-cn"):
                with self.subTest(name=name), patch.object(PACK, "ARCHIVE_ROOT", name):
                    files = [(name + "/SKILL.md", b"fixture\n")]
                    cid = PACK.content_id_of(files)
                    a = PACK.build_manifest(files, cid, "oldcommit")
                    b = PACK.build_manifest(files, cid, "newcommit")
                    self.assertEqual(a, b)
                    first = PACK.write_zip(Path(tmp) / "first.zip", files + [(name + "/MANIFEST.md", a)])
                    second = PACK.write_zip(Path(tmp) / "second.zip", files + [(name + "/MANIFEST.md", b)])
                    self.assertEqual(first, second)

    def test_verification_refuses_tampered_payload_in_each_edition(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ("jce", "jce-zh-cn"):
                with self.subTest(name=name), patch.object(PACK, "ARCHIVE_ROOT", name):
                    directory = Path(tmp) / name
                    directory.mkdir()
                    entry = directory / "SKILL.md"
                    entry.write_bytes(b"fixture\n")
                    files = PACK.collect(directory)
                    cid = PACK.content_id_of(files)
                    (directory / "MANIFEST.md").write_bytes(PACK.build_manifest(files, cid))
                    self.assertEqual(PACK.cmd_verify(directory), 0)
                    entry.write_bytes(b"changed fixture\n")
                    self.assertEqual(PACK.cmd_verify(directory), 1)


if __name__ == "__main__":
    unittest.main()
