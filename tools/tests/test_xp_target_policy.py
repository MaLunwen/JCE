"""Windows target policy controls; no compiler, dependency install or VM runs."""
import importlib.util
from contextlib import nullcontext
import tempfile
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def module(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


class Conf:
    def __init__(self, values=None):
        self.values = dict(values or {})

    def get(self, key, default=None, **unused):
        return self.values.get(key, default)

    def define(self, key, value):
        self.values[key] = value


def bgfx_owner(xp=False, **changed):
    values = {"os": "Windows", "compiler": "clang", "compiler.libcxx": "libc++",
              "arch": "x86_64", "build_type": "Release"}
    values.update(changed)
    settings = SimpleNamespace(get_safe=lambda key: values.get(key), os=values["os"])
    return SimpleNamespace(name="bgfx", conf=Conf({"user.jce:windows_xp": xp}),
        settings=settings, info=SimpleNamespace(settings=settings, conf=Conf()),
        cpp_info=SimpleNamespace(defines=[], components={}))


class XpBgfxPolicyTests(unittest.TestCase):
    def setUp(self):
        self.hook = module("xp_bgfx_hook", "conan/hooks/hook_bgfx_wasm_fix.py")

    def test_default_modern_configuration_and_identity_do_not_acquire_xp_policy(self):
        owner = bgfx_owner()
        self.hook.post_package_id(owner)
        self.hook.pre_generate(owner)
        self.hook.post_package_info(owner)
        self.assertNotIn("user.jce:bgfx_xp_backend", owner.info.conf.values)
        for name in ("D3D11", "D3D12", "VULKAN"):
            self.assertNotIn(f"JCE_BGFX_XP_{name}=1", owner.cpp_info.defines)
        self.assertNotIn("BGFX_CONFIG_VIDEO", owner.conf.get("tools.cmake.cmaketoolchain:extra_variables"))
        self.assertFalse(any("BGFX_CONFIG_RENDERER_" in value for value in
                             owner.conf.get("tools.build:defines")))

    def test_xp_has_complete_backend_selection_and_distinct_identity(self):
        owner = bgfx_owner(True)
        self.hook.post_package_id(owner)
        self.hook.pre_generate(owner)
        before = {key: list(owner.conf.get(key)) for key in
                  ("tools.build:cflags", "tools.build:cxxflags", "tools.build:defines")}
        self.hook.pre_generate(owner)
        for key, previous in before.items():
            self.assertEqual(owner.conf.get(key), previous)
        self.assertEqual(owner.info.conf.get("user.jce:bgfx_xp_backend"), "opengl-d3d11-d3d12-vulkan-v3")
        enabled = {"OPENGL": 31, "DIRECT3D11": 1, "DIRECT3D12": 1, "VULKAN": 1}
        expected = {"BGFX_CONFIG_RENDERER_" + name + "=" + str(enabled.get(name, 0))
                    for name in self.hook._XP_RENDERERS}
        self.assertTrue(expected <= set(owner.conf.get("tools.build:defines")))
        for key in ("tools.build:cflags", "tools.build:cxxflags"):
            self.assertTrue({"-D" + value for value in expected} <= set(owner.conf.get(key)))
        variables = owner.conf.get("tools.cmake.cmaketoolchain:extra_variables")
        self.assertEqual(variables["BGFX_CONFIG_VIDEO"],
                         {"value": False, "cache": True, "type": "BOOL", "force": True})
        self.hook.post_package_info(owner)
        for name in ("D3D11", "D3D12", "VULKAN"):
            self.assertIn(f"JCE_BGFX_XP_{name}=1", owner.cpp_info.defines)
        self.assertNotIn("JCE_BGFX_XP_OPENGL_ONLY=1", owner.cpp_info.defines)

    def test_xp_conflicting_compiler_directory_and_cmake_channels_rejected(self):
        cases = [("tools.build:cflags", ["-DBGFX_CONFIG_RENDERER_DIRECT3D11=0"]),
                 ("tools.build:cflags", ["-Wp,-DBGFX_CONFIG_RENDERER_DIRECT3D11=0"]),
                 ("tools.build:cxxflags", ["-Wp,-U,BGFX_CONFIG_RENDERER_OPENGL"]),
                 ("tools.build:cxxflags", ["/U", "BGFX_CONFIG_RENDERER_OPENGL"]),
                 ("tools.build:defines", ["BGFX_CONFIG_RENDERER_VULKAN=0"]),
                 ("tools.build:defines", ["BGFX_CONFIG_VIDEO=1"]),
                 ("tools.build:cxxflags", ["-DBGFX_CONFIG_VIDEO=1"]),
                 ("tools.cmake.cmaketoolchain:extra_variables", {"BGFX_CONFIG_VIDEO": True}),
                 ("tools.cmake.cmaketoolchain:extra_variables", {"BGFX_CONFIG_RENDERER_WEBGPU": 1}),
                 ("tools.cmake.cmaketoolchain:extra_variables", {"CMAKE_CXX_FLAGS_RELEASE": "-DBGFX_CONFIG_RENDERER_DIRECT3D12=0"}),
                 ("tools.cmake.cmaketoolchain:extra_variables", {"CMAKE_C_FLAGS": "$ENV{UNQUALIFIED_FLAGS}"})]
        for key, value in cases:
            with self.subTest(channel=key, value=value):
                owner = bgfx_owner(True)
                owner.conf.define(key, value)
                with self.assertRaises(ValueError):
                    self.hook.post_package_id(owner)
                self.assertNotIn("user.jce:bgfx_xp_backend", owner.info.conf.values)

    def test_xp_rejects_wrong_platform_abi_and_boolean(self):
        for changed in ({"os": "Linux"}, {"compiler": "msvc"}, {"arch": "arm64"},
                        {"compiler.runtime": "static"}, {"compiler.libcxx": "libstdc++11"}):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                self.hook.pre_generate(bgfx_owner(True, **changed))
        with self.assertRaises(ValueError):
            self.hook.pre_generate(bgfx_owner("true"))

    def test_package_id_uses_info_settings_not_removed_live_settings(self):
        owner = bgfx_owner(True)
        del owner.settings
        self.hook.post_package_id(owner)
        self.assertEqual(owner.info.conf.get("user.jce:bgfx_xp_backend"), "opengl-d3d11-d3d12-vulkan-v3")


class XpCompilerOverlayTests(unittest.TestCase):
    def test_native_and_web_reuse_one_bounded_overlay_owner(self):
        configuration = module("xp_compiler_configuration", "tools/build/conan_config.py")
        with patch.object(configuration, "_prepare_home", return_value={"clang_major": "22"}) as owner:
            self.assertEqual(configuration.prepare_compiler_home("base", "new", "22"), {"clang_major": "22"})
            owner.assert_called_once_with("base", "new", "22", package_store=None)
        for value in (None, "21.0", "0", "22;fake", 22):
            with self.subTest(value=value), self.assertRaises(ValueError):
                configuration.prepare_compiler_home("base", "new", value)


class XpCliTests(unittest.TestCase):
    def test_editor_xp_shortcut_reuses_producer_and_native_host_tools(self):
        driver = module("xp_editor_shortcut", "tools/build/jce.py")
        fixtures = ROOT / "build/tests/xp-parser-fixtures"
        fixtures.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=fixtures) as temporary:
            root = Path(temporary).resolve()
            driver.ROOT = root / "JCE"
            default = root / "llvm-mingw-xp-22.1.7-msvcrt-x86_64"
            env_root = root / "environment root"
            override = root / "explicit root"
            for toolchain in (default, env_root, override):
                (toolchain / "bin").mkdir(parents=True)
            shaderc = root / "shaderc.exe"
            host_tools = {"jce_pak": root / "jce_pak.exe", "jce_bin2obj": root / "jce_bin2obj.exe"}
            cases = [([], {}, default, "x64"),
                     (["--arch", "x86"], {"JCE_XP_TOOLCHAIN_ROOT": str(env_root)}, env_root, "x86"),
                     (["--toolchain-root", str(override)],
                      {"JCE_XP_TOOLCHAIN_ROOT": str(env_root)}, override, "x64")]
            for extra, env, selected, arch in cases:
                with self.subTest(extra=extra), patch.dict(driver.os.environ, env, clear=True), \
                     patch.object(driver, "HOST", "windows"), \
                     patch.object(driver, "_cache_scope", return_value=nullcontext()) as lease, \
                     patch.object(driver, "cmd_editor") as modern, \
                     patch.object(driver, "find_host_shaderc", return_value=shaderc), \
                     patch.object(driver, "ensure_host_tools", return_value=host_tools) as ensure, \
                     patch.object(driver, "_run_xp_tool") as run:
                    driver.main(["editor", "--xp", *extra])
                    command = run.call_args.args[1]
                    self.assertEqual(command[command.index("--toolchain-root") + 1], str(selected))
                    self.assertEqual(command[command.index("--arch") + 1], arch)
                    self.assertEqual(command[command.index("--phase") + 1], "build")
                    self.assertEqual(command[command.index("--variant") + 1], "release")
                    for flag, path in (("--shaderc", shaderc), ("--host-pak", host_tools["jce_pak"]),
                                       ("--host-bin2obj", host_tools["jce_bin2obj"])):
                        self.assertEqual(command[command.index(flag) + 1], str(path))
                    self.assertNotIn("--export-inventory", command)
                    ensure.assert_called_once_with(driver.os.environ, ("jce_pak", "jce_bin2obj"))
                    lease.assert_called_once_with(driver._host_cache_paths())
                    modern.assert_not_called()

    def test_direct_xp_editor_explicit_variants_and_codec_options(self):
        driver = module("xp_editor_codec_variants", "tools/build/jce.py")
        for args, variant, codec in (
                ([], "release", None),
                (["--variant", "release", "--patented-codecs", "off"], "release", "off"),
                (["--variant", "dist", "--patented-codecs", "on"], "dist", "on")):
            with self.subTest(args=args), patch.object(driver, "_run_xp_tool") as run:
                driver.main(["xp-editor", "--toolchain-root", str(ROOT / "fixtures/xp-toolchain"), *args])
                command = run.call_args.args[1]
                self.assertEqual(command[command.index("--variant") + 1], variant)
                self.assertEqual("--patented-codecs" in command, codec is not None)
                if codec is not None:
                    self.assertEqual(command[command.index("--patented-codecs") + 1], codec)

    def test_editor_xp_shortcut_rejects_incompatible_modes_before_build(self):
        driver = module("xp_editor_shortcut_invalid", "tools/build/jce.py")
        with patch.object(driver, "HOST", "windows"), patch.object(driver, "ensure_host_tools") as ensure, \
             patch.object(driver, "cmd_editor") as modern, patch.object(driver, "_run_xp_tool") as xp:
            for args in (["--xp", "--arch", "arm64"], ["--xp", "--variant", "debug"],
                         ["--xp", "--clean"], ["--xp", "--artifact-dir", "out"],
                         ["--xp", "--delivery-profile", "authoring"], ["--toolchain-root", "compiler"]):
                with self.subTest(args=args), self.assertRaises(SystemExit):
                    driver.main(["editor", *args])
            ensure.assert_not_called()
            xp.assert_not_called()
            modern.assert_not_called()
            driver.main(["editor", "--arch", "x64"])
            modern.assert_called_once()

    def test_bx_diagnostic_selector_is_forwarded_only_when_explicit(self):
        driver = module("xp_editor_bx_cli", "tools/build/jce.py")
        base = ["xp-editor", "--toolchain-root", "compiler"]
        for selected in ("auto", "off", "on"):
            args = driver.build_parser().parse_args(base + ["--bgfx-bx-debug", selected])
            with patch.object(driver, "_run_xp_tool") as run:
                args.func(args)
            command = run.call_args.args[1]
            self.assertEqual(command.count("--bgfx-bx-debug"), int(selected != "auto"))
            if selected != "auto":
                self.assertEqual(command[command.index("--bgfx-bx-debug") + 1], selected)
        self.assertEqual(driver.build_parser().parse_args(base).bgfx_bx_debug, "auto")

    def test_editor_background_request_is_forwarded_only_when_selected(self):
        driver = module("xp_editor_background_cli", "tools/build/jce.py")
        base = ["xp-vm", "--host", "192.0.2.1", "--user", "fixture",
                "--host-key-sha256", "fixture-pin", "--expected-arch", "x86",
                "--artifact", str(ROOT / "build/fixture.exe"), "--kind", "editor",
                "--out", str(ROOT / "build/validation/fixture-background")]
        for selected in (False, True):
            args = driver.build_parser().parse_args(base + (["--editor-run-in-background"] if selected else []))
            with patch.object(driver, "_run_xp_tool") as run:
                driver.cmd_xp_vm(args)
            command = run.call_args.args[1]
            self.assertEqual(command.count("--editor-run-in-background"), int(selected))

    def test_exact_guest_export_inputs_are_forwarded_without_credential_values(self):
        driver = module("xp_target_export_cli", "tools/build/jce.py")
        args = driver.build_parser().parse_args([
            "xp-guest-exports", "--host", "192.0.2.1", "--user", "fixture",
            "--host-key-sha256", "fixture-pin", "--expected-arch", "x64",
            "--remote-system32", "/C/WINDOWS/System32", "--out", "build/validation/fixture",
            "--password-env", "JCE_FIXTURE_CREDENTIAL", "--dll", "kernel32.dll",
            "--dll", "user32.dll", "--retain-dlls"])
        with patch.object(driver, "_run_xp_tool") as run:
            args.func(args)
        command = run.call_args.args[1]
        self.assertEqual(command.count("--dll"), 2)
        self.assertIn("--retain-dlls", command)
        self.assertEqual(command[command.index("--password-env") + 1], "JCE_FIXTURE_CREDENTIAL")
        self.assertNotIn("--phase", command)

    def test_platform_control_schema_and_delegate_use_shared_job_budget(self):
        driver = module("xp_target_os_cli", "tools/build/jce.py")
        parser = driver.build_parser()
        args = parser.parse_args(["xp-os-probe", "--arch", "x86",
                                  "--toolchain-root", "compiler", "--run-local"])
        self.assertIsNone(args.jobs)
        with patch.object(driver, "_run_xp_tool") as run, patch.object(driver, "build_jobs", return_value="2"):
            args.func(args)
        command = run.call_args.args[1]
        self.assertEqual(command[command.index("--jobs") + 1], "2")
        self.assertIn("--run-local", command)
        self.assertNotIn("--phase", command)

    def test_vm_collection_is_forwarded_without_credential_values(self):
        driver = module("xp_target_vm_cli_forwarding", "tools/build/jce.py")
        args = driver.build_parser().parse_args([
            "xp-vm", "--host", "192.0.2.1", "--user", "fixture",
            "--host-key-sha256", "fixture-pin", "--artifact", "probe.exe",
            "--expected-arch", "x86", "--kind", "editor", "--out", "build/validation/fixture",
            "--password-env", "JCE_FIXTURE_CREDENTIAL", "--collect-relative", "shot.png"])
        with patch.object(driver, "_run_xp_tool") as run:
            args.func(args)
        command = run.call_args.args[1]
        index = command.index("--collect-relative")
        self.assertEqual(command[index + 1], "shot.png")
        self.assertEqual(command[command.index("--password-env") + 1], "JCE_FIXTURE_CREDENTIAL")

    def test_parser_schema_is_independent_of_selected_or_fixture_root(self):
        driver = module("xp_target_cli_independent_root", "tools/build/jce.py")
        fixtures = ROOT / "build/tests/xp-parser-fixtures"
        fixtures.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=fixtures) as temporary:
            driver.ROOT = Path(temporary)
            parser = driver.build_parser()
            self.assertIs(parser.parse_args(["targets"]).func, driver.cmd_targets)
            parsed = parser.parse_args(["xp-editor", "--toolchain-root", "compiler"])
            self.assertIs(parsed.func, driver.cmd_xp_editor)

    def test_explicit_xp_routes_do_not_invoke_modern_editor_dispatch(self):
        driver = module("xp_target_cli", "tools/build/jce.py")
        parser = driver.build_parser()
        parsed = parser.parse_args(["xp-editor", "--arch", "x86", "--toolchain-root", "compiler", "--phase", "prepare"])
        self.assertIs(parsed.func, driver.cmd_xp_editor)
        self.assertIsNone(parsed.jobs)
        self.assertIs(parser.parse_args(["xp-probe", "--arch", "x64"]).func, driver.cmd_xp_probe)


if __name__ == "__main__":
    unittest.main()
