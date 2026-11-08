"""Provider controls for upstream 2D GL blit emulation; no native execution."""
from types import SimpleNamespace
import unittest

from tools.tests.test_xp_target_policy import bgfx_owner, module

MACRO = "BGFX_GL_CONFIG_BLIT_EMULATION"
IDENTITY = "user.jce:bgfx_gl_blit_config"


class BlitEmulationControls(unittest.TestCase):
    def setUp(self):
        self.hook = module("bgfx_blit_emulation", "conan/hooks/hook_bgfx_wasm_fix.py")

    def owner(self, tier="compatibility", xp=False, **settings):
        owner = bgfx_owner(xp, **settings)
        owner.conf.define("user.jce:bgfx_graphics_tier", tier)
        owner.cpp_info.components["bgfx"] = SimpleNamespace(defines=[])
        return owner

    def test_compatibility_partitions_old_binary_and_emits_effective_flag(self):
        for xp in (False, True):
            for arch in ("x86", "x86_64"):
                with self.subTest(xp=xp, arch=arch):
                    owner = self.owner(xp=xp, arch=arch)
                    owner.conf.define("tools.build:cxxflags", ["-O2"])
                    owner.conf.define("tools.build:defines", ["CALLER_OPTION=7"])
                    self.hook.post_package_id(owner)
                    self.hook.pre_generate(owner)
                    self.hook.post_package_info(owner)
                    self.assertEqual(owner.info.conf.get(IDENTITY), "upstream-2d-fbo-v1")
                    self.assertIn("-D" + MACRO + "=1", owner.conf.get("tools.build:cxxflags"))
                    self.assertIn(MACRO + "=1", owner.conf.get("tools.build:defines"))
                    self.assertIn("-O2", owner.conf.get("tools.build:cxxflags"))
                    self.assertIn("CALLER_OPTION=7", owner.conf.get("tools.build:defines"))
                    before = {key:list(owner.conf.get(key)) for key in ("tools.build:cxxflags", "tools.build:defines")}
                    self.hook.pre_generate(owner)
                    self.hook.post_package_info(owner)
                    for key, value in before.items():
                        self.assertEqual(owner.conf.get(key), value)
                    for target in (owner.cpp_info, owner.cpp_info.components["bgfx"]):
                        self.assertEqual(target.defines.count("JCE_BGFX_GL_BLIT_EMULATION=1"), 1)

    def test_full_tiers_keep_existing_default_and_identity(self):
        for tier in ("stable", "modern", "current"):
            owner = self.owner(tier)
            self.hook.post_package_id(owner)
            self.hook.pre_generate(owner)
            self.hook.post_package_info(owner)
            self.assertNotIn(IDENTITY, owner.info.conf.values)
            self.assertFalse(any(MACRO in value for value in owner.conf.get("tools.build:cxxflags")))
            self.assertFalse(any(MACRO in value for value in owner.conf.get("tools.build:defines")))
            self.assertFalse(any("GL_BLIT_EMULATION" in value for value in owner.cpp_info.defines))

    def test_conflicts_refuse_before_binary_reuse_or_generation(self):
        for tier, conflicting in (("compatibility", "0"), ("stable", "1")):
            cases = [("tools.build:cflags", ["-D" + MACRO + "=" + conflicting]),
                     ("tools.build:cxxflags", ["/D", MACRO + "=" + conflicting]),
                     ("tools.build:cxxflags", ["-U" + MACRO]),
                     ("tools.build:cxxflags", ["-Wp,-D," + MACRO + "=" + conflicting]),
                     ("tools.cmake.cmaketoolchain:extra_variables", {"CMAKE_CXX_FLAGS_RELEASE":"-Wp,-D" + MACRO + "=" + conflicting}),
                     ("tools.build:cflags", ["-Wp,-U," + MACRO]),
                     ("tools.build:defines", [MACRO + "=" + conflicting]),
                     ("tools.cmake.cmaketoolchain:extra_variables", {MACRO:{"value":conflicting,"type":"STRING"}}),
                     ("tools.cmake.cmaketoolchain:extra_variables", {"CMAKE_CXX_FLAGS_RELEASE":"-D" + MACRO + "=" + conflicting})]
            for stage in ("post_package_id", "pre_generate", "post_package_info"):
                for key, value in cases:
                    with self.subTest(tier=tier, stage=stage, channel=key, value=value):
                        owner = self.owner(tier)
                        owner.conf.define(key, value)
                        with self.assertRaises(ValueError):
                            getattr(self.hook, stage)(owner)
                        self.assertNotIn(IDENTITY, owner.info.conf.values)

    def test_exact_operands_do_not_reject_similar_names_or_paths(self):
        owner = self.owner()
        owner.conf.define("tools.build:cxxflags", ["-D" + MACRO + "_EXTRA=0", '-I"C:/sdk/BGFX_GL_CONFIG_BLIT_EMULATION"'])
        self.hook.pre_generate(owner)
        self.assertIn("-D" + MACRO + "_EXTRA=0", owner.conf.get("tools.build:cxxflags"))

    def test_compatibility_does_not_accept_unresolved_cmake_flags(self):
        owner = self.owner()
        owner.conf.define("tools.cmake.cmaketoolchain:extra_variables", {"CMAKE_CXX_FLAGS":"$ENV{UNKNOWN_FLAGS}"})
        with self.assertRaises(ValueError):
            self.hook.post_package_id(owner)

    def test_package_id_does_not_require_live_settings(self):
        owner = self.owner()
        del owner.settings
        self.hook.post_package_id(owner)
        self.assertEqual(owner.info.conf.get(IDENTITY), "upstream-2d-fbo-v1")

    def test_other_packages_are_unchanged(self):
        owner = self.owner()
        owner.name = "unrelated"
        for stage in ("post_package_id", "pre_generate", "post_package_info"):
            getattr(self.hook, stage)(owner)
        self.assertEqual(owner.info.conf.values, {})
        self.assertEqual(owner.cpp_info.defines, [])


if __name__ == "__main__":
    unittest.main()
