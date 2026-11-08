"""mimalloc XP hook controls: configuration/real Conan ID only, no native tools."""
import copy
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
HOOK = ROOT / "conan/hooks/hook_windows_xp_compat.py"
XP = "user.jce:windows_xp"
HEADER = "user.jce:mimalloc_xp_header"
SHA = "user.jce:mimalloc_xp_headers_sha256"
CONFIG = "user.jce:mimalloc_xp_config"
INPUTS = "user.jce:mimalloc_xp_inputs"
VARIABLES = "tools.cmake.cmaketoolchain:extra_variables"
GUARDS = ("JCE_WINDOWS_XP_SYNC_PORT", "JCE_WINDOWS_XP_MIMALLOC_PORT")
DIGEST = "4a" * 32
MISSING = object()


def load():
    spec = importlib.util.spec_from_file_location("jce_mimalloc_xp_hook", HOOK)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Conf:
    def __init__(self, values=None):
        self.values = copy.deepcopy(values or {})

    def get(self, key, default=None, check_type=None):
        value = self.values.get(key, default)
        if check_type is not None and not isinstance(value, check_type):
            raise ValueError("invalid configuration type")
        return value

    def define(self, key, value):
        self.values[key] = value


class Settings:
    def __init__(self, **values):
        self.values = {"os": "Windows", "arch": "x86_64", "compiler": "clang",
                       "compiler.version": "22", "compiler.libcxx": "libc++", **values}

    def get_safe(self, name):
        return self.values.get(name)


class MimallocXpConfigurationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.hook = load()
        cls.source = HOOK.read_text(encoding="utf-8")

    def owner(self, xp=True, settings=None, values=None, name="mimalloc", version="3.3.2"):
        settings = settings or Settings()
        conf = {HEADER: "C:/owned ports/consumer.h", SHA: DIGEST,
                "tools.build:cflags": ["-DCALLER_C=9"],
                "tools.build:cxxflags": ["-DCALLER_CXX=7"],
                "tools.build:defines": ["CALLER=3"], VARIABLES: {"CALLER_VARIABLE": 42}}
        if xp is not MISSING:
            conf[XP] = xp
        conf.update(values or {})
        return SimpleNamespace(name=name, version=version, conf=Conf(conf), settings=settings,
            info=SimpleNamespace(settings=settings, conf=Conf()))

    def check_selected(self, owner, hook=None):
        hook = hook or self.hook
        before = copy.deepcopy(owner.conf.values)
        hook.post_package_id(owner)
        self.assertEqual(owner.info.conf.get(SHA), owner.conf.get(SHA), "header identity missing")
        self.assertEqual(owner.info.conf.get(CONFIG), hook._REVISION)
        self.assertRegex(owner.info.conf.get(INPUTS), "^[0-9a-f]{64}$")
        hook.pre_generate(owner)
        once = copy.deepcopy(owner.conf.values)
        hook.pre_generate(owner)
        self.assertEqual(owner.conf.values, once)
        # Lifecycle order does not change the effective configuration identity.
        hook.post_package_id(owner)
        expected_path = hook._header_path(owner.conf.get(HEADER))
        for channel in ("tools.build:cflags", "tools.build:cxxflags"):
            flags = owner.conf.get(channel)
            for guard in GUARDS:
                self.assertIn("-D" + guard + "=1", flags, "guard compile flag missing")
            self.assertIn(self.hook._include_spellings(expected_path)[1], flags, "verified forced include missing")
            for original in before[channel]:
                self.assertIn(original, flags)
        for guard in GUARDS:
            self.assertEqual(owner.conf.get("tools.build:defines").count(guard + "=1"), 1)
        variables = owner.conf.get(VARIABLES)
        extra = variables["MI_EXTRA_CPPDEFS"]
        if isinstance(extra, dict):
            extra = extra["value"]
        self.assertIn("MI_USE_RTLGENRANDOM=1", extra.split(";"), "official RTL configuration missing")
        for option in hook._OFF_OPTIONS:
            self.assertTrue(hook._false(hook._cmake_value(variables[option])))
        self.assertEqual(variables["CALLER_VARIABLE"], before[VARIABLES]["CALLER_VARIABLE"])
        self.assertNotIn("_WIN32_WINNT", variables)
        self.assertNotIn("WINVER", variables)

    def reject(self, owner):
        original = copy.deepcopy(owner.conf.values)
        for callback in ("post_package_id", "pre_generate"):
            with self.subTest(callback=callback):
                with self.assertRaises(ValueError):
                    getattr(self.hook, callback)(owner)
                self.assertEqual(owner.conf.values, original)
                self.assertEqual(owner.info.conf.values, {})

    def test_x86_x64_c_only_and_both_guard_channels(self):
        for arch in ("x86", "x86_64"):
            for libcxx in (None, "libc++"):
                with self.subTest(arch=arch, libcxx=libcxx):
                    self.check_selected(self.owner(settings=Settings(arch=arch, **{"compiler.libcxx": libcxx})))

    def test_modern_default_false_and_unrelated_packages_are_exact_noops(self):
        for kwargs in ({"xp": MISSING}, {"xp": False}, {"name": "zlib", "xp": "invalid"}):
            with self.subTest(kwargs=kwargs):
                owner = self.owner(settings=Settings(os="Linux", compiler="gcc", arch="armv8"), **kwargs)
                before = copy.deepcopy(owner.conf.values)
                self.hook.post_package_id(owner)
                self.hook.pre_generate(owner)
                self.assertEqual(owner.conf.values, before)
                self.assertEqual(owner.info.conf.values, {})

    def test_strict_boolean_version_and_target_abi_admission(self):
        for value in (None, "True", "true", "on", "", 0, 1, [], {}):
            with self.subTest(xp=value):
                self.reject(self.owner(xp=value))
        for version in ("3.3.1", "3.3.3", None):
            self.reject(self.owner(version=version))
        for invalid in ({"os": "Linux"}, {"os": "WindowsStore"}, {"arch": "armv8"},
                        {"compiler": "msvc"}, {"compiler": "gcc"}, {"compiler.frontend": "MSVC"},
                        {"compiler.libcxx": "libstdc++11"}, {"compiler.libcxx": ""},
                        {"compiler.runtime": "static"}, {"compiler.runtime_type": "Release"},
                        {"compiler.runtime_version": "v144"}):
            with self.subTest(settings=invalid):
                self.reject(self.owner(settings=Settings(**invalid)))
        self.reject(self.owner(values={VARIABLES: {"CMAKE_SYSTEM_NAME": {"value": "WindowsStore"}}}))

    def test_absolute_header_path_quoting_and_no_filesystem_access(self):
        for path, expected in ((r"C:\owned ports\consumer.h", "C:/owned ports/consumer.h"),
                               ("/owned ports/consumer.h", "/owned ports/consumer.h"),
                               (r"\\server\share\owned ports\consumer.h", "//server/share/owned ports/consumer.h")):
            with self.subTest(path=path), patch("builtins.open", side_effect=AssertionError("hook read files")):
                owner = self.owner(values={HEADER: path})
                self.check_selected(owner)
                self.assertIn(self.hook._include_spellings(expected)[1], owner.conf.get("tools.build:cflags"))
        for path in ("relative.h", "C:relative.h", r"\root-relative.h", "C:/x/../consumer.h", "C:/x/",
                     "C:/x\n/y.h", "C:/x\x00/y.h", 'C:/x"/y.h', "C:/x'/y.h", "C:/x;y.h",
                     "C:/x${VAR}/y.h", "C:/x$<BOOL:1>/y.h", "C:/x[y].h", None, 7):
            with self.subTest(path=path):
                self.reject(self.owner(values={HEADER: path}))

    def test_real_conan_flag_renderer_preserves_include_quotes_and_directory_guards(self):
        try:
            from conan.internal.model.conf import Conf as ConanConf
            from conan.tools.cmake.toolchain.blocks import ExtraFlagsBlock
        except ImportError:
            self.skipTest("installed Conan CMake renderer unavailable")
        owner = self.owner()
        self.hook.pre_generate(owner)
        conf = ConanConf()
        for key, value in owner.conf.values.items():
            conf.define(key, value)
        owner.conf = conf
        toolchain = SimpleNamespace(generator="Ninja", extra_cflags=[], extra_cxxflags=[],
            extra_sharedlinkflags=[], extra_exelinkflags=[])
        rendered = ExtraFlagsBlock(owner, toolchain, "extra_flags").get_rendered_content()
        expected = '-include ' + chr(92) + '"C:/owned ports/consumer.h' + chr(92) + '"'
        self.assertIn(expected, rendered)
        for name in GUARDS:
            self.assertIn("-D" + name + "=1", rendered)
            self.assertIn('"' + name + '=1"', rendered)

    def test_combined_header_digest_is_required_and_exact_lowercase(self):
        for value in (None, "", "0" * 63, "g" * 64, "A" * 64, 7, True):
            with self.subTest(value=value):
                self.reject(self.owner(values={SHA: value}))
        owner = self.owner()
        del owner.conf.values[HEADER]
        self.reject(owner)

    def test_conflicting_compiler_defines_undefines_and_opaque_inputs(self):
        conflicts = (["-DJCE_WINDOWS_XP_MIMALLOC_PORT=0"], ['-D"JCE_WINDOWS_XP_SYNC_PORT=0"'],
                     ["-D'MI_USE_RTLGENRANDOM=0'"], ["/D", "JCE_WINDOWS_XP_SYNC_PORT=0"],
                     ["-U", "JCE_WINDOWS_XP_SYNC_PORT"], ["-UMI_USE_RTLGENRANDOM"],
                     ["-DMI_USE_RTLGENRANDOM=0"], ["-DMI_WIN_INIT_USE_CRT_TLS=0"],
                     ["-DMI_WIN_INIT_USE_FLS=0"], ["-DMI_WIN_INIT_USE_RAW_DLLMAIN=0"],
                     ["-DMI_WIN_INIT_USE_TLS_DLLMAIN=1"], ["-DMI_WIN_DIRECT_TLS=1"],
                     ["-include", "C:/foreign.h"], ["/FIC:/foreign.h"], ["@flags.rsp"],
                     ["-Wp,-DMI_USE_RTLGENRANDOM=0"], ["-Xclang", "-include"],
                     ["$<1:-DMI_WIN_INIT_USE_FLS=1>"], [12])
        for channel in ("tools.build:cflags", "tools.build:cxxflags"):
            for flags in conflicts:
                with self.subTest(channel=channel, flags=flags):
                    self.reject(self.owner(values={channel: flags}))

    def test_directory_and_cmake_preset_conflicts_fail_before_mutation(self):
        for define in ("JCE_WINDOWS_XP_SYNC_PORT=0", "-DJCE_WINDOWS_XP_MIMALLOC_PORT=0",
                       '"JCE_WINDOWS_XP_SYNC_PORT=0"', "/U MI_USE_RTLGENRANDOM", "MI_WIN_INIT_USE_FLS=0"):
            self.reject(self.owner(values={"tools.build:defines": [define]}))
        for entry in ({"CMAKE_C_FLAGS": "-U JCE_WINDOWS_XP_SYNC_PORT"},
                      {"CMAKE_CXX_FLAGS_RELEASE": {"value": "-DMI_USE_RTLGENRANDOM=0", "type": "STRING"}},
                      {"JCE_WINDOWS_XP_MIMALLOC_PORT": 0}, {"MI_USE_RTLGENRANDOM": "0"},
                      {"MI_WIN_USE_FLS": True}, {"MI_WIN_INIT_USE_RAW_DLLMAIN": "ON"},
                      {"MI_WIN_INIT_USE_TLS_DLLMAIN": 1}, {"MI_WIN_DIRECT_TLS": "YES"},
                      {"MI_WIN_INIT_USE_FLS": "OFF"}):
            with self.subTest(entry=entry):
                self.reject(self.owner(values={VARIABLES: entry}))

    def test_official_extra_definitions_preserve_other_items_and_typed_metadata(self):
        entry = {"value": "CALLER_EXTRA=5;MI_USE_RTLGENRANDOM=1", "type": "STRING", "cache": True}
        owner = self.owner(values={VARIABLES: {"CALLER_VARIABLE": 42, "MI_EXTRA_CPPDEFS": entry,
            "MI_WIN_USE_FLS": {"value": "OFF", "type": "BOOL", "cache": True}}})
        self.check_selected(owner)
        self.assertEqual(owner.conf.get(VARIABLES)["MI_EXTRA_CPPDEFS"], entry)
        owner = self.owner(values={VARIABLES: {"CALLER_VARIABLE": 42, "MI_EXTRA_CPPDEFS": "CALLER_EXTRA=5;"}})
        self.check_selected(owner)
        self.assertEqual(owner.conf.get(VARIABLES)["MI_EXTRA_CPPDEFS"], "CALLER_EXTRA=5;MI_USE_RTLGENRANDOM=1")
        for value in ("MI_USE_RTLGENRANDOM=0", '-D"MI_USE_RTLGENRANDOM=0"', "-U MI_USE_RTLGENRANDOM", "MI_WIN_INIT_USE_FLS=0",
                      "MI_WIN_INIT_USE_RAW_DLLMAIN=1", "MI_WIN_DIRECT_TLS=1", "$<1:MI_USE_RTLGENRANDOM=0>",
                      ["MI_USE_RTLGENRANDOM=1"], {"value": "MI_USE_RTLGENRANDOM=1", "type": "BOOL"}):
            with self.subTest(value=value):
                self.reject(self.owner(values={VARIABLES: {"MI_EXTRA_CPPDEFS": value}}))

    def test_identity_normalizes_only_verified_include_operand(self):
        first = self.owner(values={HEADER: "C:/first checkout/consumer.h"})
        second = self.owner(values={HEADER: "/different snapshot/consumer.h"})
        for owner in (first, second):
            self.hook.post_package_id(owner)
            self.hook.pre_generate(owner)
        self.assertEqual(first.info.conf.get(INPUTS), second.info.conf.get(INPUTS))
        self.assertNotEqual(first.conf.get("tools.build:cflags"), second.conf.get("tools.build:cflags"))
        for flags in (["-DCALLER_C=10"], ["-ffile-prefix-map=C:/first=C:/other"]):
            owner = self.owner(values={"tools.build:cflags": flags})
            self.hook.post_package_id(owner)
            self.assertNotEqual(first.info.conf.get(INPUTS), owner.info.conf.get(INPUTS))
        owner = self.owner(values={SHA: "9b" * 32})
        self.hook.post_package_id(owner)
        self.assertNotEqual(first.info.conf.get(INPUTS), owner.info.conf.get(INPUTS))

    def test_known_include_in_typed_preset_is_normalized_without_other_path_changes(self):
        ids = []
        for path in ("C:/first checkout/consumer.h", "/second checkout/consumer.h"):
            owner = self.owner(values={HEADER: path, VARIABLES: {"CMAKE_C_FLAGS": {
                "value": '-include "' + path + '" -I"/unrelated include"', "type": "STRING", "cache": True}}})
            self.hook.post_package_id(owner)
            ids.append(owner.info.conf.get(INPUTS))
        self.assertEqual(ids[0], ids[1])

    def test_base_cmake_flag_presets_keep_verified_include_and_typed_metadata(self):
        for name in ("CMAKE_C_FLAGS", "CMAKE_C_FLAGS_INIT", "CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_INIT"):
            with self.subTest(name=name):
                entry = {"value": "-O1 -DCALLER_PRESET=4", "type": "STRING", "cache": True}
                owner = self.owner(values={VARIABLES: {name: entry}})
                self.hook.post_package_id(owner)
                before_id = owner.info.conf.get(INPUTS)
                self.hook.pre_generate(owner)
                result = owner.conf.get(VARIABLES)[name]
                self.assertEqual(result["type"], "STRING")
                self.assertIs(result["cache"], True)
                self.assertTrue(result["value"].startswith(entry["value"]))
                self.assertIn(self.hook._include_spellings("C:/owned ports/consumer.h")[1], result["value"])
                for guard in GUARDS:
                    self.assertIn("-D" + guard + "=1", result["value"])
                self.hook.post_package_id(owner)
                self.assertEqual(before_id, owner.info.conf.get(INPUTS))

    def test_matching_split_guards_are_preserved_and_macro_boundaries_are_exact(self):
        flags = ["-D", "JCE_WINDOWS_XP_SYNC_PORT=1", "-DJCE_WINDOWS_XP_MIMALLOC_PORT=1",
                 "-DNOT_JCE_WINDOWS_XP_SYNC_PORT=0", "-UMI_WIN_INIT_USE_FLS_EXTRA"]
        owner = self.owner(values={"tools.build:cflags": flags})
        self.hook.post_package_id(owner)
        self.hook.pre_generate(owner)
        self.assertEqual(owner.conf.get("tools.build:cflags")[:len(flags)], flags)
        self.assertNotIn("-DJCE_WINDOWS_XP_SYNC_PORT=1", owner.conf.get("tools.build:cflags"))

    def test_info_settings_lifecycle_and_existing_identity_conflicts(self):
        owner = self.owner(settings=Settings(arch="x86"))
        class IdOwner:
            name = "mimalloc"
            version = "3.3.2"
            conf = owner.conf
            info = owner.info
            @property
            def settings(self):
                raise AssertionError("live settings accessed during package_id")
        self.hook.post_package_id(IdOwner())
        self.assertEqual(owner.info.conf.get(SHA), DIGEST)
        for key, value in ((CONFIG, "old-policy"), (SHA, "0" * 64), (INPUTS, "0" * 64)):
            owner = self.owner()
            owner.info.conf.define(key, value)
            before = copy.deepcopy(owner.info.conf.values)
            with self.assertRaises(ValueError):
                self.hook.post_package_id(owner)
            self.assertEqual(owner.info.conf.values, before)

    def test_real_conan_ids_keep_modern_and_partition_content_and_architecture(self):
        try:
            from conan import ConanFile
            from conan.internal.errors import conanfile_remove_attr
            from conan.internal.model.conf import Conf as ConanConf
            from conan.internal.model.info import ConanInfo, RequirementsInfo
            from conan.internal.model.settings import Settings as ConanSettings
        except ImportError:
            self.skipTest("installed Conan lifecycle unavailable")
        results = []
        for xp, header, digest, arch in ((MISSING, "C:/a.h", DIGEST, "x86_64"),
                (False, "C:/a.h", DIGEST, "x86_64"), (True, "C:/a.h", DIGEST, "x86_64"),
                (True, "/relocated/a.h", DIGEST, "x86_64"), (True, "C:/a.h", "9b" * 32, "x86_64"),
                (True, "C:/a.h", DIGEST, "x86")):
            owner = ConanFile("mimalloc-xp-id-control")
            owner.name = "mimalloc"; owner.version = "3.3.2"
            owner.settings = ConanSettings({"os": ["Windows"], "arch": ["x86", "x86_64"],
                "compiler": {"clang": {"version": ["22"], "libcxx": ["libc++"], "cppstd": ["20"]}}})
            owner.settings.update_values([("os", "Windows"), ("arch", arch), ("compiler", "clang"),
                ("compiler.version", "22"), ("compiler.libcxx", "libc++"), ("compiler.cppstd", "20")])
            owner.settings.rm_safe("compiler.libcxx"); owner.settings.rm_safe("compiler.cppstd")
            owner.conf = ConanConf(); owner.conf.define(HEADER, header); owner.conf.define(SHA, digest)
            if xp is not MISSING: owner.conf.define(XP, xp)
            owner.info = ConanInfo(settings=owner.settings.copy_conaninfo_settings(),
                options=owner.options.copy_conaninfo_options(), reqs_info=RequirementsInfo({}),
                build_requires_info=RequirementsInfo({}), conf=ConanConf())
            historical = owner.info.package_id()
            with conanfile_remove_attr(owner, ["settings", "options", "cpp_info"], "package_id"):
                self.hook.post_package_id(owner)
            if xp is MISSING or xp is False:
                self.assertEqual(historical, owner.info.package_id())
            else:
                self.assertNotEqual(historical, owner.info.package_id())
            results.append(owner.info.package_id())
        self.assertEqual(results[0], results[1])
        self.assertEqual(results[2], results[3])
        self.assertEqual(len(set(results)), 4)

    def mutant(self, old, new):
        self.assertEqual(self.source.count(old), 1)
        namespace = {"__name__": "jce_mimalloc_xp_negative_control"}
        exec(compile(self.source.replace(old, new), str(HOOK), "exec"), namespace)
        return SimpleNamespace(**namespace)

    def test_ordinary_missing_cxx_guard_is_detected_by_generated_configuration(self):
        hook = self.mutant("for key, value in channels.items():", 'for key, value in channels.items():\n        if key == "tools.build:cxxflags":\n            continue')
        with self.assertRaisesRegex(AssertionError, "guard compile flag missing"):
            self.check_selected(self.owner(), hook)

    def test_ordinary_missing_header_identity_field_is_detected(self):
        hook = self.mutant("_HEADERS_SHA: selected[2], ", "")
        with self.assertRaisesRegex(AssertionError, "header identity missing"):
            self.check_selected(self.owner(), hook)

    def test_ordinary_missing_official_rtl_merge_is_detected(self):
        old = 'return value + ("" if not value or value.endswith(";") else ";") + "MI_USE_RTLGENRANDOM=1"'
        hook = self.mutant(old, "return value")
        with self.assertRaisesRegex(AssertionError, "official RTL configuration missing"):
            self.check_selected(self.owner(), hook)

    def test_source_policy_accepts_hook_and_rejects_dependency_reads(self):
        spec = importlib.util.spec_from_file_location("mimalloc_xp_source_policy", ROOT / "tools/lint/check_conan_source_policy.py")
        policy = importlib.util.module_from_spec(spec); spec.loader.exec_module(policy)
        self.assertEqual(policy.violations(self.source), [])
        self.assertTrue(policy.violations(self.source + '\ndef _bad(c): return c.source_folder\n'))
        self.assertNotIn(b"\r", HOOK.read_bytes())
        self.assertLess(len(self.source.splitlines()), 2000)


if __name__ == "__main__":
    unittest.main()
