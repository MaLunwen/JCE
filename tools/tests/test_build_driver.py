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
    def driver(self, name):
        spec = importlib.util.spec_from_file_location(name, ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        return driver

    def test_explicit_test_targets_and_default_commands(self):
        driver = self.driver("jce_selected_test_targets")
        for selected, regex, expected in ((None, None, ["jce_tests"]),
                (["test_fixture", "test_other"], "^test_(fixture|other)$",
                 ["test_fixture", "test_other"])):
            with self.subTest(selected=selected), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                args = SimpleNamespace(target=selected, filter=regex, arch=None,
                                       configure=False, jobs=1)
                commands = []
                with patch.object(driver, "ROOT", root), patch.object(driver, "DRY_RUN", True), \
                        patch.object(driver, "HOST", "linux"), \
                        patch.object(driver, "resolve_target", return_value={}), \
                        patch.object(driver, "msvc_env", return_value={}), \
                        patch.object(driver, "preset_binary_dir", return_value=root / "native"), \
                        patch.object(driver, "run", side_effect=lambda cmd, **kw: commands.append(cmd)), \
                        patch.object(driver.subprocess, "run", return_value=SimpleNamespace(returncode=0)):
                    driver.cmd_test.__wrapped__(args)
                self.assertEqual(commands[0], ["cmake", "--build", str(root / "native"),
                                               "--target", *expected])
                ctest = next(cmd for cmd in commands if cmd[0] == "ctest")
                if regex:
                    self.assertEqual(ctest[ctest.index("-R")+1], regex)
                else:
                    self.assertNotIn("-R", ctest)
                self.assertIn("--no-tests=error", ctest)

    def test_explicit_test_targets_require_filter_and_valid_names(self):
        driver = self.driver("jce_invalid_test_targets")
        for target, regex in ((["test_fixture"], None), (["test_fixture"], ""),
                (["bad;target"], "video"), (["bad target"], "video"),
                (["--clean-first"], "video"), (["bad/target"], "video"),
                (["bad\\target"], "video"), (["bad\nname"], "video")):
            with self.subTest(target=target), patch.object(driver, "resolve_target") as resolve:
                with self.assertRaises(SystemExit):
                    driver.cmd_test.__wrapped__(SimpleNamespace(target=target, filter=regex))
                resolve.assert_not_called()

    def test_editor_artifact_directory_and_default_reset_reuse_tree(self):
        driver = self.driver("jce_editor_artifact_test")
        target = {"key": "fixture", "host": driver.HOST, "cross": False}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tree = root / "native"
            selected = root / "artifacts with spaces"
            for requested in (selected, None):
                args = SimpleNamespace(arch=None, variant="release", clean=False,
                                       artifact_dir=requested, standalone=False, delivery_profile="authoring")
                commands = []
                with patch.object(driver, "ROOT", root), patch.object(driver, "DRY_RUN", True), \
                        patch.object(driver, "resolve_target", return_value=target), \
                        patch.object(driver, "msvc_env", return_value={}), \
                        patch.object(driver, "conan_install"), \
                        patch.object(driver, "preset_name", return_value="fixture"), \
                        patch.object(driver, "preset_binary_dir", return_value=tree), \
                        patch.object(driver, "run", side_effect=lambda cmd, **kw: commands.append(cmd)):
                    returned = driver.cmd_editor.__wrapped__(args)
                expected = selected if requested else tree / "release"
                self.assertEqual(returned, expected / driver._host_exe("jce_editor"))
                self.assertIn("-DJCE_EDITOR_ARTIFACT_DIR=" +
                              (selected.as_posix() if requested else ""), commands[0])
                self.assertEqual(commands[1][2:4], [str(tree), "--target"])

    def test_sdk_build_requests_only_the_target_toolchains_stl_bridge(self):
        driver = self.driver("jce_sdk_native_targets_test")
        class CapturedBuild(Exception):
            pass
        for host, web in (("linux", False), ("darwin", False),
                          ("windows", False), ("windows", True)):
            with self.subTest(host=host, web=web), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                target = {"host": host, "cross": False, "emscripten": web}
                commands = []
                def capture(command, **kwargs):
                    commands.append(command)
                    if "--build" in command and "jce_sdk_fat_lib" in command:
                        self.assertEqual(command[-2:], ["-j", "2"])
                        raise CapturedBuild()
                with patch.object(driver, "ROOT", root), \
                        patch.object(driver, "DRY_RUN", True), \
                        patch.object(driver, "conan_install"), \
                        patch.object(driver, "preset_name", return_value="fixture-preset"), \
                        patch.object(driver, "preset_binary_dir", return_value=root / "build"), \
                        patch.object(driver, "sdk_install_dir", return_value=root / "dist"), \
                        patch.object(driver, "toolchain_path", return_value=root / "fixture.cmake"), \
                        patch.object(driver, "msvc_env", return_value={"CMAKE_BUILD_PARALLEL_LEVEL": "2"}), \
                        patch.dict(driver.os.environ, {"CMAKE_BUILD_PARALLEL_LEVEL": "2"}), \
                        patch.object(driver, "run", side_effect=capture):
                    with self.assertRaises(CapturedBuild):
                        driver._sdk_one(target, "release", "Release", False)
                argv = commands[-1]
                self.assertIn("jce_scripting_sdk_artifacts", argv)
                self.assertEqual("jce_msvc_stl_shims" in argv, host == "windows" and not web)

    def test_dependency_policy_defaults_and_invalid_inputs(self):
        driver = self.driver("jce_package_policy_test")
        self.assertEqual(driver.system_package_conan_args({}), [
            "-c", "tools.system.package_manager:mode=check",
            "-c", "tools.system.package_manager:sudo=False"])
        explicit = driver.system_package_conan_args({
            "JCE_SYSTEM_PACKAGE_MODE": "install", "JCE_SYSTEM_PACKAGE_SUDO": "1"})
        self.assertIn("tools.system.package_manager:mode=install", explicit)
        self.assertIn("tools.system.package_manager:sudo=True", explicit)
        for env in ({"JCE_SYSTEM_PACKAGE_MODE": "skip"}, {"JCE_SYSTEM_PACKAGE_SUDO": "yes"}):
            with self.assertRaises(SystemExit):
                driver.system_package_conan_args(env)

    def test_posix_debug_conan_configuration_precedes_upstream_cmake(self):
        driver = self.driver("jce_posix_conan_test")
        with tempfile.TemporaryDirectory() as tmp:
            tc = Path(tmp) / "conan_toolchain.cmake"
            tc.touch()
            target = {"host": "linux", "profile": "linux-x64", "arch": "x86_64"}
            env = {"JCE_SYSTEM_PACKAGE_MODE": "install"}
            with patch.object(driver, "HOST", "linux"), patch.object(driver, "sync_conan_hooks"), patch.object(driver, "export_conan_recipes"), patch.object(driver, "toolchain_path", return_value=tc), patch.object(driver, "conan_dir", return_value=Path(tmp)), patch.object(driver, "host_build_profile", return_value="linux-x64"), patch.object(driver, "run") as run:
                driver.conan_install(target, "Debug", env)
            argv = run.call_args.args[0]
            self.assertIn("build_type=Debug", argv)
            self.assertNotIn("compiler.runtime_type=Debug", argv)
            self.assertEqual(run.call_args.kwargs["env"]["CMAKE_POLICY_VERSION_MINIMUM"], "3.5")
            self.assertNotIn("CMAKE_POLICY_VERSION_MINIMUM", env)

    def test_dependency_jobs_follow_the_build_environment_in_both_contexts(self):
        driver = self.driver("jce_dependency_jobs_test")
        for web in (False, True):
            with self.subTest(web=web), tempfile.TemporaryDirectory() as tmp:
                tc = Path(tmp) / "conan_toolchain.cmake"
                tc.touch()
                target = {"host": "linux", "profile": "linux-x64", "arch": "x86_64",
                          "emscripten": web}
                env = {"CMAKE_BUILD_PARALLEL_LEVEL": "2"}
                em = {"toolchain": "fixture.cmake", "cc": "emcc", "cxx": "em++"}
                with patch.dict(driver.os.environ, {"CMAKE_BUILD_PARALLEL_LEVEL": "99"}), \
                        patch.object(driver, "HOST", "linux"), \
                        patch.object(driver, "sync_conan_hooks"), \
                        patch.object(driver, "export_conan_recipes"), \
                        patch.object(driver, "toolchain_path", return_value=tc), \
                        patch.object(driver, "conan_dir", return_value=Path(tmp)), \
                        patch.object(driver, "host_build_profile", return_value="linux-x64"), \
                        patch.object(driver, "emscripten_paths", return_value=em), \
                        patch.object(driver, "run") as run:
                    driver.conan_install(target, "Release", env)
                argv = run.call_args.args[0]
                self.assertIn("tools.build:jobs=2", argv)
                self.assertEqual(argv[argv.index("tools.build:jobs=2") - 1], "-c:a")
                self.assertNotIn("tools.build:jobs=99", argv)

    def test_conan_metadata_refreshes_graph_inputs_and_reuses_native_tree(self):
        driver = self.driver("jce_conan_fingerprint_test")
        for host, web in (("windows", False), ("linux", False),
                          ("darwin", False), ("linux", True)):
            with self.subTest(host=host, web=web), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                profiles = root / "conan/profiles"
                profiles.mkdir(parents=True)
                (root / "contracts").mkdir()
                recipe = root / "conanfile.py"
                recipe.write_text("# initial recipe\n")
                profile = profiles / "fixture"
                profile.write_text("include(base)\n")
                base = profiles / "base"
                base.write_text("# base profile\n")
                lock = root / "conan.lock"
                lock.write_text('{"version": "0.5"}')
                tc = root / "native/generators/conan_toolchain.cmake"
                tc.parent.mkdir(parents=True)
                tc.touch()
                sentinel = root / "native/keep-compiled-object.o"
                sentinel.write_bytes(b"cached native object")
                target = {"host": host, "profile": "fixture", "arch": "x86_64",
                          "emscripten": web}
                env = {"CMAKE_BUILD_PARALLEL_LEVEL": "2"}
                em = {"toolchain": "fixture.cmake", "cc": "emcc", "cxx": "em++"}
                commands = []
                def capture(command, **kwargs):
                    commands.append(command)
                with patch.object(driver, "ROOT", root), \
                        patch.object(driver, "HOST", host), \
                        patch.object(driver, "sync_conan_hooks"), \
                        patch.object(driver, "export_conan_recipes"), \
                        patch.object(driver, "find_vs_install", return_value=None), \
                        patch.object(driver, "toolchain_path", return_value=tc), \
                        patch.object(driver, "conan_dir", return_value=root / "native"), \
                        patch.object(driver, "host_build_profile", return_value="fixture"), \
                        patch.object(driver, "emscripten_paths", return_value=em), \
                        patch.object(driver, "run", side_effect=capture):
                    driver.stamp_graphics_tier(tc, "stable")
                    driver.conan_install(target, "Release", env)
                    self.assertEqual(len(commands), 1)  # old tree: no fingerprint
                    driver.conan_install(target, "Release", env)
                    self.assertEqual(len(commands), 1)  # identical inputs reused
                    for path in (recipe, profile, base, lock):
                        path.write_text(path.read_text() + "\n# changed")
                        count = len(commands)
                        driver.conan_install(target, "Release", env)
                        self.assertEqual(len(commands), count + 1, str(path))
                        driver.conan_install(target, "Release", env)
                        self.assertEqual(len(commands), count + 1)
                    count = len(commands)
                    env["JCE_SYSTEM_PACKAGE_MODE"] = "report"
                    driver.conan_install(target, "Release", env)
                    self.assertEqual(len(commands), count + 1)
                    self.assertEqual(sentinel.read_bytes(), b"cached native object")
                    self.assertTrue(tc.is_file())

    def test_failed_conan_refresh_cannot_leave_a_success_stamp(self):
        driver = self.driver("jce_conan_failed_fingerprint_test")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "conan").mkdir()
            recipe = root / "conanfile.py"
            recipe.write_text("# initial\n")
            tc = root / "conan_toolchain.cmake"
            tc.touch()
            target = {"host": "linux", "profile": "fixture", "arch": "x86_64", "key": "fixture"}
            with patch.object(driver, "ROOT", root), \
                    patch.object(driver, "HOST", "linux"), \
                    patch.object(driver, "sync_conan_hooks"), \
                    patch.object(driver, "export_conan_recipes"), \
                    patch.object(driver, "toolchain_path", return_value=tc), \
                    patch.object(driver, "conan_dir", return_value=root), \
                    patch.object(driver, "host_build_profile", return_value="fixture"), \
                    patch.object(driver, "run") as run:
                driver.conan_install(target, "Release", {})
                stamp = tc.parent / ".jce-conan-generation"
                self.assertTrue(stamp.is_file())
                recipe.write_text("# changed\n")
                run.side_effect = RuntimeError("install failed")
                with self.assertRaisesRegex(RuntimeError, "install failed"):
                    driver.conan_install(target, "Release", {})
                self.assertFalse(stamp.exists())
                run.side_effect = None
                driver.conan_install(target, "Release", {})
                self.assertTrue(stamp.is_file())

    def test_runtime_boot_settings_preserve_values_remove_stale_and_avoid_duplicates(self):
        driver = self.driver("jce_boot_settings_test")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project, cooked, boot = root / "project", root / "cooked", root / "boot"
            (project / ".jce").mkdir(parents=True)
            authored = project / ".jce/project-settings.json"
            output = boot / "settings/project-settings.json"
            authored.write_text('{"graphics":{"color_space":0}}', encoding="utf-8")
            driver._stage_runtime_boot_settings(project, cooked, boot)
            self.assertEqual(output.read_bytes(), authored.read_bytes())
            authored.unlink()
            driver._stage_runtime_boot_settings(project, cooked, boot)
            self.assertFalse(output.exists())
            authored.write_text("authored")
            (cooked / "settings").mkdir(parents=True)
            (cooked / "settings/project-settings.json").write_text("cooked")
            driver._stage_runtime_boot_settings(project, cooked, boot)
            self.assertFalse(output.exists())

    def test_tests_can_configure_debug_and_use_native_cooker_name(self):
        driver = self.driver("jce_fresh_test_driver_test")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = root / "build"
            build.mkdir()
            (build / "CMakeCache.txt").touch()
            env = {"JCE_TEST_TOOLCHAIN": "sentinel"}
            args = SimpleNamespace(arch="x64", variant="debug", configure=True, jobs=2,
                                   filter="^test_fixture$", repeat_until_fail=20)
            with patch.object(driver, "ROOT", root), patch.object(driver, "HOST", "linux"), patch.object(driver, "resolve_target", return_value={"key": "fixture"}), patch.object(driver, "preset_binary_dir", return_value=build) as binary, patch.object(driver, "preset_name", return_value="linux-x64-debug"), patch.object(driver, "msvc_env", return_value=env), patch.object(driver, "conan_install") as conan, patch.object(driver, "run") as run, patch.object(driver.subprocess, "run", return_value=SimpleNamespace(returncode=0)):
                driver.cmd_test(args)
            self.assertEqual(2, binary.call_count)
            binary.assert_called_with({"key": "fixture"}, "debug")
            conan.assert_called_once_with({"key": "fixture"}, "Debug", env)
            self.assertIn("-DJCE_BUILD_TESTS=ON", run.call_args_list[0].args[0])
            self.assertIn("--preset", run.call_args_list[0].args[0])
            self.assertEqual(run.call_args.args[0][-1], str(build / "tools/jce_cook"))
            ctest = next(call.args[0] for call in run.call_args_list if call.args[0][0] == "ctest")
            self.assertIn("--no-tests=error", ctest)
            self.assertEqual(ctest[ctest.index("-R") + 1], "^test_fixture$")
            self.assertEqual(ctest[ctest.index("--repeat") + 1], "until-fail:20")

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
            args = SimpleNamespace(arch="x64", variant="dist", skip_build=True, out=str(out), delivery_profile="authoring")
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
            with patch.dict(os.environ, {"JCE_TEST_REAL_FONT": "1"}), patch.object(driver, "resolve_target", return_value={"key": "fixture"}), patch.object(driver, "preset_binary_dir", return_value=build), patch.object(driver, "msvc_env", return_value=env), patch.object(driver, "run", side_effect=record), patch.object(driver, "ctest_jobs", return_value="1"), patch.object(driver.subprocess, "run", return_value=SimpleNamespace(returncode=0)) as probe:
                driver.cmd_test(SimpleNamespace(arch="x64", jobs=1, rerun_failed=False))
            self.assertEqual(probe.call_args.args[0][-1], "import PIL, numpy")
            self.assertIs(probe.call_args.kwargs["env"], calls[1][1]["env"])
            self.assertEqual(len(calls), 4)
            self.assertTrue(any(call[0][0] == "ctest" for call in calls))
            self.assertIs(calls[0][1]["env"], env)
            self.assertIs(calls[2][1]["env"], env)
            self.assertEqual(env, {"JCE_TEST_TOOLCHAIN": "sentinel"})
            for argv, kwargs in (calls[1], calls[3]):
                self.assertEqual(kwargs["env"]["JCE_TEST_REAL_FONT"], "1", argv)
                self.assertEqual(kwargs["env"]["JCE_TEST_TOOLCHAIN"], "sentinel", argv)
                self.assertIs(kwargs["env"], calls[1][1]["env"], argv)

    def test_colour_verification_falls_back_to_configured_python(self):
        spec = importlib.util.spec_from_file_location("jce_python_fallback_test", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            python = build / "configured-python"
            python.touch()
            (build / "CMakeCache.txt").write_text("_Python3_EXECUTABLE:INTERNAL=%s\n" % python)
            with patch.object(driver, "resolve_target", return_value={"key": "fixture"}), patch.object(driver, "preset_binary_dir", return_value=build), patch.object(driver, "msvc_env", return_value={"key": "fixture"}), patch.object(driver, "run") as run, patch.object(driver, "ctest_jobs", return_value="1"), patch.object(driver.subprocess, "run", side_effect=[SimpleNamespace(returncode=1), SimpleNamespace(returncode=0)]) as probe:
                driver.cmd_test(SimpleNamespace(arch="x64", jobs=1, rerun_failed=False))
            self.assertEqual(probe.call_count, 2)
            self.assertEqual(probe.call_args.args[0][0], str(python))
            self.assertEqual(run.call_args.args[0][0], str(python))

    def test_colour_verification_reports_missing_dependencies(self):
        spec = importlib.util.spec_from_file_location("jce_python_missing_test", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            (build / "CMakeCache.txt").touch()
            with patch.object(driver, "resolve_target", return_value={"key": "fixture"}), patch.object(driver, "preset_binary_dir", return_value=build), patch.object(driver, "msvc_env", return_value={"key": "fixture"}), patch.object(driver, "run") as run, patch.object(driver, "ctest_jobs", return_value="1"), patch.object(driver.subprocess, "run", return_value=SimpleNamespace(returncode=1)):
                with self.assertRaisesRegex(SystemExit, "requires Pillow and numpy"):
                    driver.cmd_test(SimpleNamespace(arch="x64", jobs=1, rerun_failed=False))
            self.assertEqual(run.call_count, 3)


if __name__ == "__main__":
    unittest.main()
