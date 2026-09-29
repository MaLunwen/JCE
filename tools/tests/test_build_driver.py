"""Test host-toolchain propagation without invoking a compiler."""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[2]


class BuildDriverTests(unittest.TestCase):
    def test_legacy_toolchain_stamp_cannot_reuse_patched_dependencies(self):
        spec = importlib.util.spec_from_file_location("jce_stamp_test", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec); spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory() as tmp:
            toolchain = Path(tmp) / "conan_toolchain.cmake"
            toolchain.touch()
            driver.graphics_tier_stamp(toolchain).write_text("stable\n")
            self.assertFalse(driver.graphics_tier_matches(toolchain, "stable"))
            driver.stamp_graphics_tier(toolchain, "stable")
            self.assertTrue(driver.graphics_tier_matches(toolchain, "stable"))
            self.assertFalse(driver.graphics_tier_matches(toolchain, "modern"))

    def test_hook_retirement_preserves_bytes_without_cross_volume_rename(self):
        spec = importlib.util.spec_from_file_location("jce_hook_test", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repository"
            home = Path(tmp) / "conan-home"
            (root / "conan/hooks").mkdir(parents=True)
            (root / "contracts").mkdir()
            hooks = home / "extensions/hooks"
            hooks.mkdir(parents=True)
            name = "hook_owned_legacy.py"
            original = b"# retired first-party fixture\n"
            installed = hooks / name
            installed.write_bytes(original)
            policy = {"retired_hooks": {name: hashlib.sha256(original).hexdigest()}}
            (root / "contracts/conan-source-policy.json").write_text(json.dumps(policy))
            with patch.object(driver, "ROOT", root), patch.dict(os.environ, {"CONAN_HOME": str(home)}), patch.object(Path, "replace", side_effect=OSError("different disk drive")):
                driver.sync_conan_hooks()
                driver.sync_conan_hooks()
            archive = root / "docs/delivery/retired-installed-conan-hooks" / name
            self.assertEqual(archive.read_bytes(), original)
            self.assertFalse(installed.exists())

    def test_failed_hook_archive_keeps_installed_original(self):
        spec = importlib.util.spec_from_file_location("jce_hook_failure_test", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repository"
            home = Path(tmp) / "conan-home"
            (root / "conan/hooks").mkdir(parents=True)
            (root / "contracts").mkdir()
            hooks = home / "extensions/hooks"
            hooks.mkdir(parents=True)
            name = "hook_owned_legacy.py"
            original = b"# retired first-party fixture\n"
            installed = hooks / name
            installed.write_bytes(original)
            policy = {"retired_hooks": {name: hashlib.sha256(original).hexdigest()}}
            (root / "contracts/conan-source-policy.json").write_text(json.dumps(policy))
            with patch.object(driver, "ROOT", root), patch.dict(os.environ, {"CONAN_HOME": str(home)}), patch.object(driver.shutil, "copy2", side_effect=OSError("archive write failed")):
                with self.assertRaises(OSError):
                    driver.sync_conan_hooks()
            self.assertEqual(installed.read_bytes(), original)

    def test_test_command_propagates_compiler_environment_to_every_process(self):
        spec = importlib.util.spec_from_file_location("jce_build_env_test", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory(prefix="jce-build-env-") as tmp:
            build = Path(tmp)
            (build / "CMakeCache.txt").touch()
            env = {"JCE_TEST_TOOLCHAIN": "sentinel"}
            calls = []
            def record(argv, **kwargs):
                calls.append((argv, kwargs))
            with patch.object(driver, "resolve_target", return_value={}), patch.object(driver, "preset_binary_dir", return_value=build), patch.object(driver, "msvc_env", return_value=env), patch.object(driver, "run", side_effect=record), patch.object(driver, "ctest_jobs", return_value="1"):
                driver.cmd_test(SimpleNamespace(arch="x64", jobs=1, rerun_failed=False))
            self.assertEqual(len(calls), 4)
            self.assertTrue(any(call[0][0] == "ctest" for call in calls))
            for argv, kwargs in calls:
                self.assertIs(kwargs.get("env"), env, argv)


if __name__ == "__main__":
    unittest.main()
