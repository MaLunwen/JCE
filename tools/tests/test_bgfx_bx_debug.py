"""Pure BX debug provider/identity controls; no compiler, package install or GPU."""
import copy
from pathlib import Path
from types import ModuleType, SimpleNamespace
import unittest
from unittest import mock

from tools.tests.test_xp_target_policy import bgfx_owner, module

CONF = "user.jce:bgfx_bx_debug"
OPTION = CONF + "_option"
COMPILED = CONF + "_compiled"
CONFIG = CONF + "_config"
INPUTS = CONF + "_inputs"
CONSUMER = CONF + "_consumer"
VARIABLES = "tools.cmake.cmaketoolchain:extra_variables"
MACRO = "BX_CONFIG_DEBUG"
ROOT = Path(__file__).resolve().parents[2]


class BxDebugControls(unittest.TestCase):
    def setUp(self):
        self.hook = module("jce_bgfx_bx_debug_controls", "conan/hooks/hook_bgfx_wasm_fix.py")

    def owner(self, request="auto", build_type="Release", consumer=None, **settings):
        owner = bgfx_owner(False, build_type=build_type, **settings)
        owner.conf.define(CONF, request)
        if consumer is not None:
            owner.conf.define(CONSUMER, consumer)
        owner.cpp_info.components = {name: SimpleNamespace(defines=[]) for name in ("bgfx", "bx", "bimg")}
        # Model the unchanged upstream Conan recipe package_info baseline.
        owner.cpp_info.components["bx"].defines = ["BX_CONFIG_DEBUG=%d" % int(build_type == "Debug")]
        return owner

    def metadata(self, owner):
        self.hook.post_package_info(owner)
        prefix = "JCE_BGFX_BX_DEBUG_"
        return {name[len(prefix):]: int(value) for definition in owner.cpp_info.defines
                for name, value in [definition.partition("=")[::2]] if name.startswith(prefix)}

    def check_selection(self, owner, request, option, compiled, changed):
        self.hook.post_package_id(owner)
        self.hook.pre_generate(owner)
        expected = dict(REQUEST=request, OPTION=option, COMPILED=compiled)
        consumer = owner.conf.get(CONSUMER, default="auto")
        if consumer != "auto":
            expected.update(CONSUMER_ASSERTED=1, CONSUMER_VALUE=int(consumer == "on"))
        self.assertEqual(self.metadata(owner), expected)
        if changed:
            self.assertEqual(owner.info.conf.get(COMPILED), str(compiled))
            self.assertEqual(owner.info.conf.get(CONFIG), "upstream-cmake-v1")
        else:
            for key in (OPTION, COMPILED, CONFIG, INPUTS):
                self.assertNotIn(key, owner.info.conf.values)
        for target in owner.cpp_info.components.values():
            self.assertTrue({"JCE_BGFX_BX_DEBUG_%s=%d" % item for item in expected.items()} <= set(target.defines))
        return expected

    def test_release_on_selects_upstream_option_and_new_binary_identity(self):
        owner = self.owner("on")
        owner.conf.define(VARIABLES, {"CALLER_OPTION": 7})
        owner.conf.define("tools.build:cxxflags", ["-DCALLER_FLAG=9"])
        self.check_selection(owner, 1, 1, 1, True)
        self.assertEqual(owner.info.conf.get(OPTION), "on")
        self.assertEqual([value for value in owner.cpp_info.components["bx"].defines if value.startswith(MACRO + "=")], [MACRO + "=1"])
        self.assertEqual(owner.conf.get(VARIABLES)[MACRO],
                         dict(value=True, cache=True, type="BOOL", force=True))
        self.assertEqual(owner.conf.get(VARIABLES)["CALLER_OPTION"], 7)
        self.assertIn("-DCALLER_FLAG=9", owner.conf.get("tools.build:cxxflags"))
        for key in ("tools.build:cflags", "tools.build:cxxflags", "tools.build:defines"):
            self.assertFalse(any(MACRO in flag for flag in owner.conf.get(key, default=[])))
        before = copy.deepcopy(owner.conf.values)
        self.hook.pre_generate(owner)
        self.hook.post_package_info(owner)
        self.assertEqual(owner.conf.values, before)
        self.assertEqual(owner.cpp_info.defines.count("JCE_BGFX_BX_DEBUG_COMPILED=1"), 1)

    def test_defaults_and_debug_forced_on_keep_historical_identity(self):
        for build in ("Release", "RelWithDebInfo", "MinSizeRel", "Debug"):
            for requested in ("auto", "off", "on"):
                with self.subTest(build=build, request=requested):
                    option = int(requested == "on")
                    compiled = int(build == "Debug" or option)
                    owner = self.owner(requested, build)
                    self.check_selection(owner, {"auto": -1, "off": 0, "on": 1}[requested], option,
                                         compiled, compiled != int(build == "Debug"))
                    if requested == "auto":
                        self.assertNotIn(MACRO, owner.conf.get(VARIABLES))
        owner = self.owner("off", "Debug", os="WindowsStore")
        self.check_selection(owner, 0, 0, 1, False)

    def test_cmake_booleans_are_preserved_and_do_not_mean_compiler_operands(self):
        for raw, option in (("ON", 1), ("OFF", 0), (True, 1), (False, 0),
                            ({"value": "ON", "type": "BOOL", "cache": True}, 1)):
            with self.subTest(raw=raw):
                owner = self.owner()
                owner.conf.define(VARIABLES, {MACRO: raw, "CALLER": 42})
                self.check_selection(owner, -1, option, option, bool(option))
                self.assertEqual(owner.conf.get(VARIABLES)[MACRO], raw)
        owner = self.owner("off", "Debug")
        owner.conf.define(VARIABLES, {MACRO: "OFF"})
        owner.conf.define("tools.build:defines", [MACRO + "=1"])
        self.check_selection(owner, 0, 0, 1, False)

    def test_unknown_custom_inputs_remain_unknown_and_partition_by_their_inputs(self):
        witnesses = []
        for value in ("first.cmake", "second.cmake"):
            owner = self.owner("on", consumer="on")
            owner.conf.define("tools.cmake.cmaketoolchain:user_toolchain", [value])
            self.check_selection(owner, 1, 1, -1, True)
            witnesses.append(owner.info.conf.get(INPUTS))
        self.assertNotEqual(*witnesses)
        for variable in ({MACRO: "$ENV{BX_UNRESOLVED}"}, {"CMAKE_C_FLAGS_RELEASE": "${FLAGS}"},
                         {"CMAKE_CONFIGURATION_TYPES": "Debug;Release"}, {"CMAKE_BUILD_TYPE": "Other"}):
            with self.subTest(variable=variable):
                owner = self.owner()
                owner.conf.define(VARIABLES, variable)
                self.hook.post_package_id(owner)
                self.assertEqual(self.metadata(owner)["COMPILED"], -1)
                self.assertEqual(len(owner.info.conf.get(INPUTS)), 64)
        owner = self.owner(build_type=None)
        self.hook.post_package_id(owner)
        self.assertEqual(self.metadata(owner)["COMPILED"], -1)

    def test_dynamic_build_lists_are_unknown_and_keep_request_specific_consumer_policy(self):
        for channel in ("tools.build:cflags", "tools.build:cxxflags", "tools.build:defines"):
            for request in ("auto", "off", "on"):
                fingerprints = []
                for value in ("${FIRST_UNRESOLVED_BX_INPUT}", "${SECOND_UNRESOLVED_BX_INPUT}"):
                    with self.subTest(channel=channel, request=request, value=value):
                        owner = self.owner(request, consumer=None if request == "auto" else request)
                        baseline = list(owner.cpp_info.components["bx"].defines)
                        owner.conf.define(channel, [value])
                        self.hook.post_package_id(owner)
                        self.assertEqual(owner.info.conf.get(COMPILED), "-1")
                        self.assertEqual(owner.info.conf.get(CONFIG), "upstream-cmake-v1")
                        fingerprints.append(owner.info.conf.get(INPUTS))
                        self.hook.pre_generate(owner)
                        self.assertEqual(self.metadata(owner)["COMPILED"], -1)
                        self.assertIn(value, owner.conf.get(channel))
                        actual = [item for item in owner.cpp_info.components["bx"].defines if item.startswith(MACRO + "=")]
                        self.assertEqual(actual, baseline if request == "auto" else [MACRO + "=" + str(int(request == "on"))])
                self.assertEqual(len(fingerprints), 2)
                self.assertTrue(all(len(value) == 64 for value in fingerprints))
                self.assertNotEqual(*fingerprints)

    def test_literal_build_lists_keep_known_defaults_and_unknown_fingerprints_stay_bounded(self):
        for channel, value in (("tools.build:cflags", "-DCALLER_C=1"),
                               ("tools.build:cxxflags", "-DCALLER_CXX=1"),
                               ("tools.build:defines", "CALLER_DEFINE=1")):
            owner = self.owner()
            owner.conf.define(channel, [value])
            self.check_selection(owner, -1, 0, 0, False)
            self.assertIn(value, owner.conf.get(channel))
        owner = self.owner()
        owner.conf.define("tools.build:cxxflags", ["${" + "X" * 9000 + "}"])
        with self.assertRaisesRegex(ValueError, "fingerprint exceeds"):
            self.hook.post_package_id(owner)
    def test_conflicting_reserved_operands_refuse_all_lifecycle_stages_and_channels(self):
        for requested, build, bad in (("on", "Release", "0"), ("off", "Release", "1"), ("off", "Debug", "0")):
            cases = [("tools.build:cflags", ["-D" + MACRO + "=" + bad]),
                     ("tools.build:cxxflags", ["/D", MACRO + "=" + bad]),
                     ("tools.build:cxxflags", ["-Wp,-D," + MACRO + "=" + bad]),
                     ("tools.build:cflags", ["-Wp,-U," + MACRO]),
                     ("tools.build:defines", [MACRO + "=" + bad]),
                     (VARIABLES, {"CMAKE_C_FLAGS_INIT": "-U" + MACRO}),
                     (VARIABLES, {"CMAKE_CXX_FLAGS_RELEASE": "-Wp,-D" + MACRO + "=" + bad})]
            for stage in ("post_package_id", "pre_generate", "post_package_info"):
                for key, value in cases:
                    with self.subTest(request=requested, build=build, stage=stage, key=key, value=value):
                        owner = self.owner(requested, build)
                        owner.conf.define(key, value)
                        with self.assertRaises(ValueError):
                            getattr(self.hook, stage)(owner)
                        self.assertNotIn(CONFIG, owner.info.conf.values)
        for requested, raw in (("on", "OFF"), ("off", "ON"), ("on", "${UNKNOWN}")):
            owner = self.owner(requested)
            owner.conf.define(VARIABLES, {MACRO: raw})
            with self.assertRaises(ValueError):
                self.hook.pre_generate(owner)
        owner = self.owner()
        owner.conf.define("tools.cmake.cmaketoolchain:user_toolchain", ["unobserved.cmake"])
        owner.conf.define("tools.build:defines", [MACRO + "=0"])
        with self.assertRaises(ValueError):
            self.hook.post_package_id(owner)

    def test_bx_component_preserves_defaults_and_distinguishes_unknown_auto_from_explicit(self):
        for request, build in (("auto", "Release"), ("off", "Release"), ("auto", "Debug"), ("off", "Debug")):
            owner = self.owner(request, build)
            original = list(owner.cpp_info.components["bx"].defines)
            self.metadata(owner)
            self.assertEqual(owner.cpp_info.components["bx"].defines[:len(original)], original)
        for request in ("auto", "on", "off"):
            owner = self.owner(request, consumer=None if request == "auto" else request)
            owner.conf.define("tools.cmake.cmaketoolchain:user_toolchain", ["unobserved.cmake"])
            original = list(owner.cpp_info.components["bx"].defines)
            self.assertEqual(self.metadata(owner)["COMPILED"], -1)
            actual = [value for value in owner.cpp_info.components["bx"].defines if value.startswith(MACRO + "=")]
            self.assertEqual(actual, original if request == "auto" else [MACRO + "=" + str(int(request == "on"))])
            self.hook.post_package_info(owner)
            self.assertEqual([value for value in owner.cpp_info.components["bx"].defines if value.startswith(MACRO + "=")], actual)

    def test_bx_component_refuses_noncanonical_contradictory_or_duplicate_exports(self):
        for values in ([MACRO + "=0", MACRO + "=0"], [MACRO + "=0", MACRO + "=1"],
                       [MACRO + "=2"], [MACRO], [" " + MACRO + "=0"], [MACRO + "=1"]):
            owner = self.owner("off")
            owner.cpp_info.components["bx"].defines = list(values)
            before = list(values)
            with self.subTest(values=values), self.assertRaises(ValueError):
                self.hook.post_package_info(owner)
            self.assertEqual(owner.cpp_info.components["bx"].defines, before)
        owner = self.owner("on")
        owner.cpp_info.components["bx"].defines += ["CALLER_COMPONENT=3"]
        self.metadata(owner)
        self.assertEqual(owner.cpp_info.components["bx"].defines[:2], [MACRO + "=1", "CALLER_COMPONENT=3"])
    def test_invalid_request_other_packages_and_package_id_settings_boundary(self):
        for value in ("yes", "invalid", 1, False):
            owner = self.owner(value)
            with self.assertRaises(ValueError):
                self.hook.post_package_id(owner)
        owner = self.owner("on")
        del owner.settings
        self.hook.post_package_id(owner)
        self.assertEqual(owner.info.conf.get(COMPILED), "1")
        owner = self.owner("on")
        owner.name = "unrelated"
        for stage in ("post_package_id", "pre_generate", "post_package_info"):
            getattr(self.hook, stage)(owner)
        self.assertEqual(owner.info.conf.values, {})
        self.assertEqual(owner.cpp_info.defines, [])

    def test_source_policy_is_pure_and_never_reads_a_custom_toolchain(self):
        owner = self.owner("on", consumer="on")
        owner.conf.define("tools.cmake.cmaketoolchain:user_toolchain", ["must-not-be-opened.cmake"])
        with mock.patch("builtins.open", side_effect=AssertionError("filesystem read")):
            self.hook.post_package_id(owner)
            self.hook.pre_generate(owner)
            self.hook.post_package_info(owner)
        self.assertEqual(self.metadata(owner)["COMPILED"], -1)

    def test_unknown_explicit_provider_requires_consumer_assertion_before_mutation(self):
        for request in ("on", "off"):
            owner = self.owner(request)
            owner.conf.define("tools.cmake.cmaketoolchain:user_toolchain", ["unknown.cmake"])
            original = copy.deepcopy(owner.cpp_info)
            self.hook.post_package_id(owner)
            with self.subTest(request=request), self.assertRaisesRegex(ValueError, "requires.*consumer"):
                self.hook.post_package_info(owner)
            self.assertEqual(owner.cpp_info.defines, original.defines)
            self.assertEqual(owner.cpp_info.components["bx"].defines, original.components["bx"].defines)

    def test_asserted_consumer_keeps_unknown_provider_and_normalizes_mandatory_macro(self):
        for request, build, consumer in (("on", "Release", "on"), ("off", "Release", "off"),
                                         ("off", "Debug", "on")):
            owner = self.owner(request, build, consumer=consumer)
            owner.conf.define("tools.cmake.cmaketoolchain:user_toolchain", ["unknown.cmake"])
            self.hook.post_package_id(owner)
            identity = copy.deepcopy(owner.info.conf.values)
            self.hook.pre_generate(owner)
            values = self.metadata(owner)
            self.assertEqual(values["COMPILED"], -1)
            self.assertEqual(values["CONSUMER_ASSERTED"], 1)
            self.assertEqual(values["CONSUMER_VALUE"], int(consumer == "on"))
            self.assertIn(MACRO + "=" + str(int(consumer == "on")), owner.cpp_info.components["bx"].defines)
            self.assertEqual(self.metadata(owner), values)
            self.assertEqual(owner.info.conf.values, identity)

    def test_consumer_assertions_do_not_change_provider_identity_or_generated_flags(self):
        identities, generated = [], []
        for consumer in ("auto", "off", "on"):
            owner = self.owner("on", consumer=consumer)
            owner.conf.define("tools.cmake.cmaketoolchain:user_toolchain", ["unknown.cmake"])
            self.hook.post_package_id(owner)
            identities.append(copy.deepcopy(owner.info.conf.values))
            self.hook.pre_generate(owner)
            generated.append({key: copy.deepcopy(owner.conf.get(key)) for key in
                              (VARIABLES, "tools.build:cflags", "tools.build:cxxflags", "tools.build:defines")})
        self.assertEqual(identities, [identities[0]] * 3)
        self.assertEqual(generated, [generated[0]] * 3)

    def test_consumer_conflicts_with_known_or_nominal_upstream_expression_refuse(self):
        cases = (("on", "Release", "off"), ("off", "Release", "on"),
                 ("off", "Debug", "off"), ("auto", "Release", "on"))
        for custom in (False, True):
            for request, build, consumer in cases:
                owner = self.owner(request, build, consumer=consumer)
                if custom:
                    owner.conf.define("tools.cmake.cmaketoolchain:user_toolchain", ["unknown.cmake"])
                original = copy.deepcopy(owner.cpp_info)
                with self.subTest(custom=custom, request=request, build=build), self.assertRaisesRegex(ValueError, "consumer.*conflict"):
                    self.hook.post_package_info(owner)
                self.assertEqual(owner.cpp_info.defines, original.defines)
                self.assertEqual(owner.cpp_info.components["bx"].defines, original.components["bx"].defines)

    def test_consumer_unknown_expression_is_asserted_without_provider_relabel(self):
        owner = self.owner(consumer="on")
        owner.conf.define(VARIABLES, {"CMAKE_CONFIGURATION_TYPES": "Debug;Release"})
        values = self.metadata(owner)
        self.assertEqual(values["COMPILED"], -1)
        self.assertEqual(values["CONSUMER_VALUE"], 1)
        self.assertEqual(values["CONSUMER_ASSERTED"], 1)
        self.assertIn(MACRO + "=1", owner.cpp_info.components["bx"].defines)
        owner = self.owner("on", consumer="off")
        owner.conf.define(VARIABLES, {"CMAKE_CONFIGURATION_TYPES": "Debug;Release"})
        with self.assertRaisesRegex(ValueError, "consumer.*conflict"):
            self.hook.post_package_info(owner)

    def test_consumer_invalid_inputs_refuse_and_default_metadata_stays_unchanged(self):
        for value in ("yes", "", 1, False, "${UNKNOWN}"):
            owner = self.owner(consumer=value)
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.hook.post_package_info(owner)
        owner = self.owner()
        owner.conf.define("tools.cmake.cmaketoolchain:user_toolchain", ["unknown.cmake"])
        values = self.metadata(owner)
        self.assertEqual(values, dict(REQUEST=-1, OPTION=0, COMPILED=-1))
        self.assertEqual(owner.cpp_info.components["bx"].defines[0], MACRO + "=0")

    def test_missing_owner_wires_fail_the_ordinary_positive_assertions(self):
        source = (ROOT / "conan/hooks/hook_bgfx_wasm_fix.py").read_text(encoding="utf-8")
        for wire, replacement in (
            ('variables["BX_CONFIG_DEBUG"] =', 'variables["BX_CONFIG_DEBUG_OMITTED"] ='),
            ('conanfile.info.conf.define(_BX_DEBUG_CONFIG_CONF, _BX_DEBUG_CONFIG)', 'pass'),
            ('"JCE_BGFX_BX_DEBUG_COMPILED=%d" % bx_debug[2],', ''),
            ('consumer = _bx_debug_export_configuration(conanfile, bx_debug)', 'consumer = -1')):
            with self.subTest(wire=wire):
                self.assertEqual(source.count(wire), 1)
                mutant = ModuleType("bx_debug_missing_wire")
                mutant.__file__ = self.hook.__file__
                exec(compile(source.replace(wire, replacement, 1), mutant.__file__, "exec"), mutant.__dict__)
                with mock.patch.object(self, "hook", mutant), self.assertRaises((AssertionError, KeyError)):
                    self.test_release_on_selects_upstream_option_and_new_binary_identity()


if __name__ == "__main__":
    unittest.main()
