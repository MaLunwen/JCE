"""Actual worktree header deletion must not hide existing private exports."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("exported_gate", ROOT / "tools/lint/check_exported_symbol_declared.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class WorkingHeaderControls(unittest.TestCase):
    def test_tracked_deleted_header_skips_but_existing_private_export_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            public = root / "engine/include/jce"
            private = root / "engine/src/module"
            public.mkdir(parents=True)
            private.mkdir(parents=True)
            for index in range(100):
                (public / ("api%d.h" % index)).write_text("/* public header */\n", encoding="utf-8")
            deleted = private / "deleted.h"
            deleted.write_text("JCE_API void removed(void);\n", encoding="utf-8")
            live = private / "live.h"
            live.write_text("JCE_API void illegal_private(void);\n", encoding="utf-8")
            subprocess.run(["git", "-C", str(root), "add", "engine"], check=True)
            deleted.unlink()
            listing = subprocess.check_output(["git", "-C", str(root), "ls-files", "--cached", "--others", "--exclude-standard"], text=True).splitlines()
            self.assertIn("engine/src/module/deleted.h", listing)
            with patch.object(GATE, "REPO_ROOT", root), patch.object(GATE._lgf, "working_tree", return_value=listing), \
                 patch.object(GATE, "EXEMPT", root / "none.txt"), contextlib.redirect_stdout(io.StringIO()), \
                 contextlib.redirect_stderr(io.StringIO()):
                _, private_paths = GATE.headers()
                self.assertEqual(private_paths, ["engine/src/module/live.h"])
                self.assertIn("illegal_private", GATE.symbols(private_paths))
                self.assertEqual(GATE.main(), 1)
                live.write_text("void ordinary_private(void);\n", encoding="utf-8")
                self.assertEqual(GATE.main(), 0)


if __name__ == "__main__":
    unittest.main()
