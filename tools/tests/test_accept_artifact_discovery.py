"""Real filesystem artifact selection controls; no executable/GUI is run."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("accept_discovery_owner", ROOT / "tools/build/jce_accept.py")
owner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(owner)
driver = owner._build_driver()


class AcceptanceDiscoveryControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="jce_accept_selection_")
        self.addCleanup(self.temp.cleanup)
        self.project = Path(self.temp.name) / "relocated project"
        self.project.mkdir()
        self.manifest = {"name": "Fixture App", "version": "1.2.3"}
        self.host = patch.object(driver, "HOST", "windows")
        self.host.start()
        self.addCleanup(self.host.stop)
        self.arch = patch.object(driver, "host_arch", return_value="x86_64")
        self.arch.start()
        self.addCleanup(self.arch.stop)

    def package(self, path):
        path.mkdir(parents=True)
        exe = path / ("Fixture.exe" if os.name == "nt" else "fixture")
        exe.write_bytes(b"PRESENCE_FIXTURE_NOT_EXECUTABLE_ABI_EVIDENCE")
        exe.chmod(0o755)
        return path

    def modern(self, arch="x86_64", variant="dist", tag="windows"):
        return self.package(self.project / "dist/apps/Fixture App" / (tag + "-" + arch + "-" + variant))

    def legacy(self, arch="x86_64", variant="dist"):
        path = self.package(self.project / "dist/games" / ("Fixture App-1.2.3-win32-" + arch))
        (path / "VERSION.txt").write_text(
            "name: Fixture App\nversion: 1.2.3\nplatform: win32\narch: " + arch + "\nvariant: " + variant + "\ncommit: fixture\n",
            encoding="utf-8")
        return path

    def select(self, arch="x64", variant="dist", explicit=None):
        return owner.select_bundle(self.project, self.manifest, arch, variant, explicit)

    def test_modern_match_wins_over_newer_legacy_and_wrong_variant(self):
        modern = self.modern()
        legacy = self.legacy()
        unrelated = self.modern("aarch64", "release")
        os.utime(legacy, (2000000000, 2000000000))
        os.utime(unrelated, (2100000000, 2100000000))
        with patch.object(owner, "_legacy_stamp_matches", side_effect=AssertionError("modern must not probe legacy")):
            selected, reason = self.select()
        self.assertEqual(selected, modern)
        self.assertIn("modern", reason)
        self.assertNotIn("WARN", reason)

    def test_wrong_architecture_never_becomes_selected_by_mtime(self):
        self.modern("aarch64")
        selected, reason = self.select("x64")
        self.assertIsNone(selected)
        self.assertIn("x86_64/dist", reason)

    def test_wrong_variant_is_not_a_release_or_dist_fallback(self):
        self.modern(variant="release")
        selected, reason = self.select(variant="dist")
        self.assertIsNone(selected)
        self.assertIn("dist", reason)

    def test_arch_alias_comes_from_actual_shared_target_owner(self):
        modern = self.modern("aarch64")
        for requested in ("arm64", "aarch64"):
            with self.subTest(requested=requested):
                self.assertEqual(self.select(requested)[0], modern)

    def test_default_architecture_comes_from_shared_host_owner(self):
        modern = self.modern("aarch64")
        with patch.object(driver, "host_arch", return_value="aarch64"):
            self.assertEqual(self.select(None)[0], modern)

    def test_modern_directory_without_an_artifact_is_not_accepted(self):
        (self.project / "dist/apps/Fixture App/windows-x86_64-dist").mkdir(parents=True)
        self.assertIsNone(self.select()[0])

    def test_modern_platform_compatibility_uses_shared_sdk_tag(self):
        modern = self.modern(tag=driver.SDK_TAG["windows"])
        self.assertEqual(self.select()[0], modern)

    def test_multiple_modern_matches_refuse_instead_of_picking_newest(self):
        self.modern()
        self.modern(tag="win32")
        with self.assertRaisesRegex(ValueError, "multiple modern artifacts"):
            self.select()

    def test_legacy_requires_exact_name_version_platform_arch_and_variant(self):
        legacy = self.legacy()
        selected, reason = self.select()
        self.assertEqual(selected, legacy)
        self.assertTrue(reason.startswith("WARN: legacy"))
        stamp = legacy / "VERSION.txt"
        original = stamp.read_text(encoding="utf-8")
        for key, wrong in (("name", "Other App"), ("version", "0.0.1"),
                           ("platform", "linux"), ("arch", "aarch64"), ("variant", "release")):
            with self.subTest(key=key):
                lines = [key + ": " + wrong if line.startswith(key + ":") else line
                         for line in original.splitlines()]
                stamp.write_text("\n".join(lines) + "\n", encoding="utf-8")
                self.assertIsNone(self.select()[0])
        stamp.unlink()
        self.assertIsNone(self.select()[0])

    def test_duplicate_or_oversized_legacy_declarations_are_refused(self):
        legacy = self.legacy()
        stamp = legacy / "VERSION.txt"
        original = stamp.read_text(encoding="utf-8")
        stamp.write_text(original + "variant: dist\n", encoding="utf-8")
        self.assertIsNone(self.select()[0])
        stamp.write_text(original + " " * 16385, encoding="utf-8")
        self.assertIsNone(self.select()[0])

    def test_explicit_bundle_retains_compatibility_without_implicit_selection(self):
        explicit = self.package(self.project / "chosen custom old directory")
        self.modern()
        with patch.object(driver, "resolve_target", side_effect=AssertionError("explicit override must not auto-select")):
            selected, reason = self.select("arm64", "debug", str(explicit))
        self.assertEqual(selected, explicit.resolve())
        self.assertTrue(reason.startswith("explicit --bundle"))

    def test_invalid_implicit_variant_and_traversing_name_are_refused(self):
        with self.assertRaisesRegex(ValueError, "unsupported acceptance variant"):
            self.select(variant="typo")
        for name in ("../foreign", "folder/app", "C:\\foreign", 7):
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, "invalid package name"):
                    owner.select_bundle(self.project, {"name": name}, "x64", "dist")
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                self.select("unsupported-arch")


class AcceptanceMemoryAvailabilityControls(unittest.TestCase):
    @staticmethod
    def line(rss):
        return ("engine: perf: 1.5 ms avg (667 FPS) | cpu 1.0 / gpu 0.5 ms | 10 draws | "
                f"gpu-mem N/A MiB (tex 0 + rt 0) | rss {rss} MiB | commit N/A MiB\n")

    def stage(self, line):
        report = owner.Report()
        owner.stage_perf(report, line, {"frame_ms": 10, "draws": 100, "rss_mb": 512})
        self.assertEqual(len(report.stages), 1)
        return report.stages[0]

    def test_unknown_gpu_memory_does_not_hide_available_rss_budget(self):
        stage = self.stage(self.line("20"))
        self.assertEqual(stage.state, owner.PASS)
        self.assertIn("rss 20 MiB", stage.detail)

    def test_unavailable_rss_fails_budget_with_a_reason_instead_of_an_exception(self):
        stage = self.stage(self.line("N/A"))
        self.assertEqual(stage.state, owner.FAIL)
        self.assertIn("RSS unavailable; cannot verify the 512 MiB ceiling", stage.detail)
        self.assertIn("UNVERIFIED:", stage.detail)
        self.assertNotIn("OVER:", stage.detail)

    def test_known_rounded_zero_is_preserved_and_oversized_rss_still_fails(self):
        self.assertEqual(self.stage(self.line("0")).state, owner.PASS)
        oversized = self.stage(self.line("513"))
        self.assertEqual(oversized.state, owner.FAIL)
        self.assertIn("rss 513 MiB > 512 MiB ceiling", oversized.detail)

    def test_legacy_zero_cannot_pass_a_memory_budget(self):
        line = self.line("0").replace("MiB", "MB").replace("N/A", "0")
        stage = self.stage(line)
        self.assertEqual(stage.state, owner.FAIL)
        self.assertIn("RSS unavailable", stage.detail)


if __name__ == "__main__":
    unittest.main()
