"""Immutable first-party header cohort controls; no compiler or dependency calls."""
from pathlib import Path
import importlib.util
import json
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("xp_platform_snapshot_owner", ROOT / "tools/build/xp_editor.py")
XP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(XP)
NAMES = ("jce_windows_xp_sync_internal.h", "jce_windows_xp_mimalloc_internal.h")


class PlatformSnapshotControls(unittest.TestCase):
    def setUp(self):
        base = ROOT / "build/validation/xp-platform-snapshot-tests"
        base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name) / "repo with spaces"
        self.source = self.repo / "engine/src/os/platform/compatibility"
        self.source.mkdir(parents=True)
        for name in NAMES:
            (self.source / name).write_bytes(b"/* immutable owned header */\n")
        self.output = self.repo / "build/compat/xp-platform-ports"

    def prepare(self):
        return XP._snapshot_platform_headers(self.repo, self.output)

    def test_exact_two_header_cohort_reuses_bytes_and_stamp(self):
        first = self.prepare()
        cohort = Path(first["snapshot"])
        before = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in cohort.iterdir()}
        self.assertEqual(set(before), set(NAMES) | {"owner.json"})
        self.assertEqual(self.prepare(), first)
        self.assertEqual({p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in cohort.iterdir()}, before)
        for name in NAMES:
            self.assertEqual((cohort / name).read_bytes(), (self.source / name).read_bytes())
        self.assertEqual(Path(first["header"]), cohort / NAMES[1])

    def test_new_source_content_selects_new_cohort_and_retains_old(self):
        first = self.prepare()
        original = (Path(first["snapshot"]) / NAMES[0]).read_bytes()
        (self.source / NAMES[0]).write_bytes(b"/* changed first-party header */\n")
        second = self.prepare()
        self.assertNotEqual(first["headers_sha256"], second["headers_sha256"])
        self.assertNotEqual(first["snapshot"], second["snapshot"])
        self.assertEqual((Path(first["snapshot"]) / NAMES[0]).read_bytes(), original)

    def test_modified_snapshot_is_refused_without_repair(self):
        port = self.prepare()
        target = Path(port["snapshot"]) / NAMES[0]
        target.write_bytes(b"changed snapshot")
        with self.assertRaisesRegex(ValueError, "snapshot changed; refusing repair"):
            self.prepare()
        self.assertEqual(target.read_bytes(), b"changed snapshot")

    def test_extra_entries_refuse_at_finite_budget(self):
        port = self.prepare()
        (Path(port["snapshot"]) / "foreign").write_bytes(b"do not remove")
        with self.assertRaisesRegex(ValueError, "entry budget"):
            self.prepare()
        self.assertTrue((Path(port["snapshot"]) / "foreign").exists())

    def test_wrong_manifest_is_not_overwritten(self):
        port = self.prepare()
        target = Path(port["snapshot"]) / "owner.json"
        target.write_text('{}\n', encoding="utf-8", newline="\n")
        with self.assertRaisesRegex(ValueError, "identity mismatch; refusing repair"):
            self.prepare()
        self.assertEqual(json.loads(target.read_text()), {})

    def test_missing_oversized_and_crlf_inputs_publish_nothing(self):
        path = self.source / NAMES[0]
        for content in (b"", b"x" * 65537, b"line\r\n"):
            with self.subTest(size=len(content)):
                path.write_bytes(content)
                with self.assertRaises(ValueError):
                    self.prepare()
                self.assertFalse(self.output.exists())
        path.unlink()
        with self.assertRaises(OSError):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_compiler_path_and_content_identity_are_explicit_profile_arguments(self):
        port = self.prepare()
        arguments = XP._conan_profile_arguments(Path("host"), Path("build"), 1, "a" * 64, port)
        self.assertIn("user.jce:mimalloc_xp_header=" + port["header"], arguments)
        self.assertIn("user.jce:mimalloc_xp_headers_sha256=" + port["headers_sha256"], arguments)
        self.assertIn("repo with spaces", port["header"])

    def test_built_identity_binds_header_content(self):
        cache = self.repo / "build/cache"
        files = [cache / name for name in ("editor.exe", "CMakeCache.txt", "conan/xp-graph.json", "conan/xp-generation.json", "host.profile", "build.profile")]
        for path in files:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"same admitted fixture")
        first = XP._built_identity(files[0], cache, "a" * 64, files[4], files[5], {"headers_sha256": "b" * 64})
        second = XP._built_identity(files[0], cache, "a" * 64, files[4], files[5], {"headers_sha256": "c" * 64})
        self.assertNotEqual(first, second)
        self.assertEqual(first["platform_headers_sha256"], "b" * 64)


if __name__ == "__main__":
    unittest.main()
