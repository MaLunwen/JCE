"""Pure compatibility-tier controls; no Conan install, native build or GPU."""
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.tests.test_xp_target_policy import ROOT, bgfx_owner, module

CONF = "user.jce:bgfx_graphics_tier"
EXISTING = {"stable": dict(value=0, opengl=31, opengles=30),
            "modern": dict(value=1, opengl=43, opengles=31),
            "current": dict(value=2, opengl=46, opengles=32)}
COMPATIBILITY = dict(value=3, opengl=21, opengles=20)


class CompatibilityTierTests(unittest.TestCase):
    def setUp(self):
        self.hook = module("bgfx_compatibility_tier", "conan/hooks/hook_bgfx_wasm_fix.py")

    def selected(self, tier=None, *, xp=False, **settings):
        owner = bgfx_owner(xp, **settings)
        owner.cpp_info.components["bgfx"] = SimpleNamespace(defines=[])
        if tier is not None:
            owner.conf.define(CONF, tier)
        self.hook.post_package_id(owner)
        self.hook.pre_generate(owner)
        self.hook.post_package_info(owner)
        return owner

    def test_compatibility_is_appended_without_renumbering_existing_tiers(self):
        self.assertEqual(list(self.hook._GRAPHICS_TIERS), [*EXISTING, "compatibility"])
        self.assertEqual({name: self.hook._GRAPHICS_TIERS[name] for name in EXISTING}, EXISTING)
        self.assertEqual(self.hook._GRAPHICS_TIERS["compatibility"], COMPATIBILITY)

    def test_actual_hook_partitions_compatibility_and_exports_real_floors(self):
        for xp in (False, True):
            for arch in ("x86", "x86_64"):
                with self.subTest(xp=xp, arch=arch):
                    stable = self.selected("stable", xp=xp, arch=arch)
                    owner = self.selected("compatibility", xp=xp, arch=arch)
                    self.assertEqual(owner.info.conf.get(CONF), "compatibility")
                    self.assertNotEqual(owner.info.conf.values, stable.info.conf.values)
                    self.assertEqual({key: value for key, value in owner.info.conf.values.items() if key not in (CONF, "user.jce:bgfx_gl_blit_config")},
                                     {key: value for key, value in stable.info.conf.values.items() if key not in (CONF, "user.jce:bgfx_gl_blit_config")})
                    variables = owner.conf.get("tools.cmake.cmaketoolchain:extra_variables")
                    self.assertEqual((variables["BGFX_OPENGL_VERSION"], variables["BGFX_OPENGLES_VERSION"]), (21, 20))
                    expected = {"JCE_GRAPHICS_API_TIER_VALUE=3", "JCE_BGFX_OPENGL_VERSION=21", "JCE_BGFX_OPENGLES_VERSION=20"}
                    for target in (owner.cpp_info, owner.cpp_info.components["bgfx"]):
                        self.assertTrue(expected <= set(target.defines))
                    before = list(owner.conf.get("tools.build:cxxflags"))
                    self.hook.pre_generate(owner)
                    self.hook.post_package_info(owner)
                    self.assertEqual(owner.conf.get("tools.build:cxxflags"), before)
                    for target in (owner.cpp_info, owner.cpp_info.components["bgfx"]):
                        for value in expected:
                            self.assertEqual(target.defines.count(value), 1)
                    if xp:
                        enabled = {"OPENGL": 21, "DIRECT3D11": 1, "DIRECT3D12": 1, "VULKAN": 1}
                        renderers = {"BGFX_CONFIG_RENDERER_" + name + "=" + str(enabled.get(name, 0))
                                     for name in self.hook._XP_RENDERERS}
                        self.assertTrue(renderers <= set(owner.conf.get("tools.build:defines")))
                        for key in ("tools.build:cflags", "tools.build:cxxflags"):
                            self.assertTrue({"-D" + value for value in renderers} <= set(owner.conf.get(key)))
                        for name in ("D3D11", "D3D12", "VULKAN"):
                            self.assertIn(f"JCE_BGFX_XP_{name}=1", owner.cpp_info.defines)
                            self.assertIn(f"JCE_BGFX_XP_{name}=1", owner.cpp_info.components["bgfx"].defines)
                    else:
                        self.assertFalse(any(value.startswith("BGFX_CONFIG_RENDERER_") for value in owner.conf.get("tools.build:defines")))
                        for name in ("D3D11", "D3D12", "VULKAN"):
                            self.assertNotIn(f"JCE_BGFX_XP_{name}=1", owner.cpp_info.defines)

    def test_existing_tier_defaults_identity_and_definitions_remain_unchanged(self):
        for xp in (False, True):
            with self.subTest(xp=xp):
                default, stable = self.selected(xp=xp), self.selected("stable", xp=xp)
                self.assertEqual(default.info.conf.values, stable.info.conf.values)
                self.assertEqual(default.cpp_info.defines, stable.cpp_info.defines)
                for name, values in EXISTING.items():
                    owner = self.selected(name, xp=xp)
                    self.assertEqual(owner.info.conf.get(CONF), name)
                    for key, value in (("JCE_GRAPHICS_API_TIER_VALUE", values["value"]),
                                       ("JCE_BGFX_OPENGL_VERSION", values["opengl"]),
                                       ("JCE_BGFX_OPENGLES_VERSION", values["opengles"])):
                        self.assertIn(key + "=" + str(value), owner.cpp_info.defines)
        owner = self.selected("compatibility", os="Linux")
        self.assertIn("JCE_GRAPHICS_API_TIER_VALUE=3", owner.cpp_info.defines)
        self.assertNotIn("user.jce:bgfx_xp_backend", owner.info.conf.values)

    def test_unknown_tiers_and_old_xp_renderer_override_are_refused(self):
        for tier in ("", "legacy", "gl2.1", "3", "compatibility-extra"):
            with self.subTest(tier=tier):
                owner = bgfx_owner(True)
                owner.conf.define(CONF, tier)
                with self.assertRaises(ValueError):
                    self.hook.post_package_id(owner)
                self.assertNotIn(CONF, owner.info.conf.values)
        owner = bgfx_owner(True)
        owner.conf.define(CONF, "compatibility")
        owner.conf.define("tools.build:cflags", ["-DBGFX_CONFIG_RENDERER_OPENGL=31"])
        with self.assertRaises(ValueError):
            self.hook.pre_generate(owner)


class CompatibilitySelectionTests(unittest.TestCase):
    def setUp(self):
        self.driver = module("jce_compatibility_selection", "tools/build/jce.py")
        self.xp = module("xp_compatibility_selection", "tools/build/xp_editor.py")
        root = ROOT / "build/tests/bgfx-compatibility-tier"
        root.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=root)
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name)

    def test_public_native_selectors_accept_compatibility_and_keep_stable_default(self):
        with patch.dict(os.environ, {"JCE_GRAPHICS_API_TIER": "stable"}):
            parser = self.driver.build_parser()
        for command in ("editor", "sdk"):
            self.assertEqual(parser.parse_args([command]).graphics_api_tier, "stable")
            self.assertEqual(parser.parse_args([command, "--graphics-api-tier", "compatibility"]).graphics_api_tier,
                             "compatibility")
        self.assertEqual(self.driver.graphics_api_tier({}), "stable")
        selected = {"JCE_GRAPHICS_API_TIER": " Compatibility "}
        self.assertEqual(self.driver.graphics_api_tier(selected), "compatibility")
        self.assertIn("user.jce:bgfx_graphics_tier=compatibility", self.driver.bgfx_graphics_conan_args(selected))
        for tier in ("legacy", "3", "compatibility-extra"):
            with self.subTest(tier=tier), redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit):
                    self.driver.graphics_api_tier({"JCE_GRAPHICS_API_TIER": tier})
                with self.assertRaises(SystemExit):
                    parser.parse_args(["editor", "--graphics-api-tier", tier])

    def test_web_still_refuses_non_stable_floor_before_resolving_tools(self):
        with patch.object(self.driver, "toolchain_path", return_value=self.repo / "untouched-toolchain.cmake"), \
             patch.object(self.driver, "emscripten_paths", side_effect=AssertionError("Web tool resolution must not run")), \
             redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                self.driver.conan_install.__wrapped__({"emscripten": True}, "Release", {"JCE_GRAPHICS_API_TIER": "compatibility"})
        self.assertFalse((self.repo / "untouched-toolchain.cmake").exists())

    def test_xp_environment_and_shared_profile_arguments_select_compatibility(self):
        home = self.repo / "build/overlay"
        home.mkdir(parents=True)
        (home / "settings.yml").write_text("compiler:\n", encoding="utf-8")
        (home / ".jce-toolchain-overlay.json").write_text(json.dumps(dict(home=str(home), clang_major="22")), encoding="utf-8")
        inherited = dict(PATH="original", CFLAGS="unqualified", JCE_GRAPHICS_API_TIER="modern")
        env = self.xp._conan_environment(self.repo, home, self.repo / "compiler", 2, inherited)
        arguments = self.xp._conan_profile_arguments(self.repo / "host", self.repo / "build-profile", 2, "a" * 64)
        self.assertEqual(env["JCE_GRAPHICS_API_TIER"], "compatibility")
        self.assertIn("user.jce:bgfx_graphics_tier=compatibility", arguments)
        self.assertNotIn("user.jce:bgfx_graphics_tier=stable", arguments)
        self.assertNotIn("CFLAGS", env)
        self.assertEqual(inherited["JCE_GRAPHICS_API_TIER"], "modern")
        self.assertEqual(env["CMAKE_BUILD_PARALLEL_LEVEL"], "2")

    def test_hook_change_selects_new_configuration_home_without_mutating_previous_bytes(self):
        source = (ROOT / "conan/hooks/hook_bgfx_wasm_fix.py").read_bytes()
        hook = self.repo / "conan/hooks/hook_bgfx_wasm_fix.py"
        hook.parent.mkdir(parents=True)
        hook.write_bytes(source.replace(b'    "compatibility": {"value": 3, "opengl": 21, "opengles": 20},\n', b""))
        cache = self.repo / "build/cache"
        old = self.xp._default_conan_home(self.repo, cache)
        installed = old / "extensions/hooks/hook_bgfx_wasm_fix.py"
        installed.parent.mkdir(parents=True)
        installed.write_bytes(b"previous immutable installed hook")
        package = cache / "package-store/old-package.bin"
        package.parent.mkdir()
        package.write_bytes(b"previous immutable package")
        hook.write_bytes(source)
        new = self.xp._default_conan_home(self.repo, cache)
        self.assertNotEqual(old, new)
        self.assertEqual(new.parent, old.parent)
        self.assertEqual(installed.read_bytes(), b"previous immutable installed hook")
        self.assertEqual(package.read_bytes(), b"previous immutable package")


if __name__ == "__main__":
    unittest.main()
