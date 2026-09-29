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
    def test_editor_bundle_keeps_script_runtime_and_excludes_local_assets(self):
        spec = importlib.util.spec_from_file_location("jce_bundle_test", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "build/dist"
            src.mkdir(parents=True)
            exe = driver._host_exe("jce_editor")
            for name in (exe, "JceScript.dll", "JceScript.runtimeconfig.json", "libfixture.so.1"):
                (src / name).write_bytes(b"fixture")
            for name in ("jce_script_java_classes", "jce_script_python", "video", "fonts"):
                (src / name).mkdir()
                (src / name / "fixture").write_bytes(b"fixture")
            sdk = root / "sdk"
            (sdk / "lib/cmake/JCE").mkdir(parents=True)
            (sdk / "lib/cmake/JCE/JCEConfig.cmake").touch()
            for name in ("LICENSE", "THIRD_PARTY_LICENSES.md"):
                (root / name).write_bytes(b"notice")
            out = root / "package"
            args = SimpleNamespace(arch="x64", variant="dist", skip_build=True, out=str(out))
            target = {"host": driver.HOST, "arch": "x86_64"}
            with patch.object(driver, "ROOT", root), patch.object(driver, "resolve_target", return_value=target), patch.object(driver, "preset_binary_dir", return_value=src.parent), patch.object(driver, "sdk_install_dir", return_value=sdk), patch.object(driver, "git_short_sha", return_value="fixture"):
                driver.cmd_package_editor(args)
                for name in ("JceScript.runtimeconfig.json", "libfixture.so.1", "jce_script_java_classes/fixture", "jce_script_python/fixture", "LICENSE", "THIRD_PARTY_LICENSES.md"):
                    self.assertTrue((out / name).is_file(), name)
                self.assertFalse((out / "video").exists())
                self.assertFalse((out / "fonts").exists())
                runtime = root / "python-runtime"
                runtime.mkdir()
                originals = {"python312.dll": b"dll", "python312.zip": b"stdlib", "python312._pth": b"python312.zip\n.\n", "LICENSE.txt": b"upstream terms"}
                for name, data in originals.items():
                    (runtime / name).write_bytes(data)
                args.python_runtime = str(runtime)
                driver.cmd_package_editor(args)
                for name, data in originals.items():
                    self.assertEqual((out / name).read_bytes(), data)
                    self.assertEqual((runtime / name).read_bytes(), data)
                (runtime / "python312.zip").unlink()
                with self.assertRaises(SystemExit):
                    driver.cmd_package_editor(args)
                args.python_runtime = None
                (src / "JceScript.runtimeconfig.json").unlink()
                with self.assertRaises(SystemExit):
                    driver.cmd_package_editor(args)

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
