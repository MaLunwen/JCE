"""PhysFS floor selection and Conan identity controls; no dependency build or XP run."""
import copy
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest


ROOT = Path(__file__).resolve().parents[2]
HOOK_PATH = ROOT / "conan/hooks/hook_cmake_policy_fix.py"
XP = "user.jce:windows_xp"
FLOOR = "user.jce:physfs_win32_floor"
CONFIG = "user.jce:physfs_win32_floor_config"
VARIABLES = "tools.cmake.cmaketoolchain:extra_variables"
CHANNELS = (("tools.build:cflags", "-D"), ("tools.build:cxxflags", "-D"),
            ("tools.build:defines", ""))
_MISSING = object()


class Conf:
    def __init__(self, values=None):
        self.values = copy.deepcopy(values or {})

    def get(self, key, default=None, check_type=None):
        value = self.values.get(key, default)
        if check_type is not None and value is not None and not isinstance(value, check_type):
            raise ValueError("invalid conf type for " + key)
        return value

    def define(self, key, value):
        self.values[key] = value


class Settings:
    def __init__(self, **values):
        self.values = {"os": "Windows", "arch": "x86_64", "compiler": "clang",
                       "compiler.libcxx": "libc++", **values}

    def get_safe(self, key):
        return self.values.get(key)


class PhysFsXpFloorTests(unittest.TestCase):
    @staticmethod
    def fixture(xp=_MISSING, settings=None, values=None, name="physfs"):
        spec = importlib.util.spec_from_file_location("jce_physfs_xp_hook", HOOK_PATH)
        hook = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(hook)
        conf = Conf({VARIABLES: {"CALLER_SETTING": 42},
                     "tools.build:cflags": ["-DCALLER_C=1"],
                     "tools.build:cxxflags": ["-DCALLER_CXX=1"],
                     "tools.build:defines": ["CALLER_DEFINE=1"], **(values or {})})
        if xp is not _MISSING:
            conf.define(XP, xp)
        settings = settings or Settings()
        owner = SimpleNamespace(name=name, settings=settings, conf=conf,
                                info=SimpleNamespace(settings=settings, conf=Conf()))
        return hook, owner

    def check_selection(self, hook, owner, floor, config):
        original = copy.deepcopy(owner.conf.values)
        hook.post_package_id(owner)
        self.assertEqual(owner.info.conf.values, {FLOOR: floor, CONFIG: config},
                         "selected PhysFS package identity missing")
        hook.pre_generate(owner)
        once = copy.deepcopy(owner.conf.values)
        hook.pre_generate(owner)
        self.assertEqual(owner.conf.values, once, "PhysFS generation is not idempotent")
        for key, prefix in CHANNELS:
            values = owner.conf.get(key)
            for value in original.get(key, []):
                self.assertIn(value, values)
            for name in ("_WIN32_WINNT", "WINVER"):
                self.assertEqual(values.count(prefix + name + "=" + floor), 1,
                                 "selected PhysFS compile definition missing")
        self.assertEqual(owner.conf.get(VARIABLES)["CMAKE_POLICY_VERSION_MINIMUM"], "3.5")
        self.assertEqual(owner.conf.get(VARIABLES)["CALLER_SETTING"], 42)

    def check_rejected(self, hook, owner, pattern="PhysFS|windows_xp"):
        original = copy.deepcopy(owner.conf.values)
        for name in ("post_package_id", "pre_generate"):
            with self.subTest(entry=name):
                with self.assertRaisesRegex(ValueError, pattern):
                    getattr(hook, name)(owner)
                self.assertEqual(owner.info.conf.values, {}, "rejected package identity was mutated")
                self.assertEqual(owner.conf.values, original, "rejected generation mutated conf")

    def test_modern_default_and_false_retain_original_floor_config_and_channels(self):
        for xp in (_MISSING, False):
            with self.subTest(xp=xp):
                # Historical modern fixtures do not have compiler or arch settings.
                hook, owner = self.fixture(xp, Settings(os="Windows", compiler=None,
                                                      arch=None, **{"compiler.libcxx": None}))
                self.check_selection(hook, owner, "0x0601", "directory-flags-v1")
                self.assertEqual(hook._reserved_flag_names(["-DWINVER=0x0601"]), {"WINVER"})

    def test_explicit_xp_selects_both_architecture_floors_and_all_compile_channels(self):
        for arch, floor in (("x86", "0x0501"), ("x86_64", "0x0502")):
            with self.subTest(arch=arch):
                hook, owner = self.fixture(True, Settings(arch=arch))
                self.check_selection(hook, owner, floor, "directory-flags-xp-v1")

    def test_invalid_bool_arch_platform_compiler_or_msvc_runtime_refuses(self):
        for value in (None, "true", "false", "on", "off", "", 0, 1, [], {}):
            with self.subTest(xp=value):
                self.check_rejected(*self.fixture(value))
        invalid = ({"arch": "armv8"}, {"arch": "armv7"}, {"arch": None},
                   {"os": "Linux"}, {"os": "WindowsStore"},
                   {"compiler": "msvc"}, {"compiler": "gcc"},
                   {"compiler.libcxx": "libstdc++11"},
                   {"compiler.frontend": "MSVC"}, {"compiler.runtime": "dynamic"},
                   {"compiler.runtime_type": "Release"}, {"compiler.runtime_version": "v143"})
        for settings in invalid:
            with self.subTest(settings=settings):
                self.check_rejected(*self.fixture(True, Settings(**settings)))
        for system in ("WindowsStore", {"value": "WindowsStore", "type": "STRING"}):
            self.check_rejected(*self.fixture(True, values={VARIABLES: {"CMAKE_SYSTEM_NAME": system}}))
        self.check_rejected(*self.fixture(True, values={
            "tools.cmake.cmaketoolchain:system_name": "WindowsStore"}))

    def test_c_only_recipe_may_remove_cpp_settings_before_identity_and_generation(self):
        for libcxx in (None, ""):
            with self.subTest(libcxx=libcxx):
                hook, owner = self.fixture(True, Settings(**{"compiler.libcxx": libcxx}))
                self.check_selection(hook, owner, "0x0502", "directory-flags-xp-v1")

    def test_gnu_frontend_and_caller_cmake_policy_are_preserved(self):
        for frontend in ("GNU", "gnu"):
            with self.subTest(frontend=frontend):
                policy = {"value": "3.12", "cache": True, "type": "STRING"}
                settings = Settings(**{"compiler.frontend": frontend, "compiler.runtime": "",
                    "compiler.runtime_type": "", "compiler.runtime_version": ""})
                hook, owner = self.fixture(True, settings, values={VARIABLES: {
                    "CMAKE_POLICY_VERSION_MINIMUM": policy}})
                hook.post_package_id(owner)
                hook.pre_generate(owner)
                self.assertEqual(owner.conf.get(VARIABLES)["CMAKE_POLICY_VERSION_MINIMUM"], policy)
                self.assertEqual(owner.info.conf.get(FLOOR), "0x0502")

    def test_conflicting_floor_and_config_metadata_is_never_silently_overwritten(self):
        for xp, key, value in ((True, FLOOR, "0x0601"), (True, CONFIG, "directory-flags-v1"),
                               (False, FLOOR, "0x0502"), (False, CONFIG, "unknown"),
                               (True, FLOOR, 0x0502), (True, CONFIG, False)):
            with self.subTest(xp=xp, key=key, value=value):
                self.check_rejected(*self.fixture(xp, values={key: value}))
        hook, owner = self.fixture(True, values={FLOOR: "0x0502", CONFIG: "directory-flags-xp-v1"})
        self.check_selection(hook, owner, "0x0502", "directory-flags-xp-v1")

    def test_conflicting_reserved_flags_defines_and_cmake_inputs_refuse_before_mutation(self):
        conflicts = (("tools.build:cflags", ["-D_WIN32_WINNT=0x0601"]),
                     ("tools.build:cxxflags", ["/D", "WINVER=0x0501"]),
                     ("tools.build:cflags", ["-U", "_WIN32_WINNT"]),
                     ("tools.build:cxxflags", ["/UWINVER"]),
                     ("tools.build:cflags", ["-D", '"_WIN32_WINNT=0x0601"']),
                     ("tools.build:defines", ["WINVER=0x0601"]),
                     ("tools.build:defines", ["_WIN32_WINNT"]),
                     ("tools.build:defines", [12]),
                     ("tools.build:cflags", [12]),
                     (VARIABLES, {"CMAKE_C_FLAGS": "-D WINVER=0x0601"}),
                     (VARIABLES, {"CMAKE_CXX_FLAGS_RELEASE": {"value": "/U _WIN32_WINNT"}}),
                     (VARIABLES, {"WINVER": {"value": "0x0601"}}),
                     (VARIABLES, {"_WIN32_WINNT": "0x0601"}))
        for key, value in conflicts:
            with self.subTest(key=key, value=value):
                self.check_rejected(*self.fixture(True, values={key: value}))

    def test_matching_split_flags_and_exact_macro_boundaries_are_preserved(self):
        hook, owner = self.fixture(True)
        flags = ["/D", "_WIN32_WINNT=0x0502", "-DWINVER=0x0502", "-O2",
                 "-D_WIN32_WINNT_EXTRA=7", "-DNOT_WINVER=9", "/UWINVER_EXTRA"]
        owner.conf.define("tools.build:cflags", flags[:])
        owner.conf.define(VARIABLES, {"CALLER_SETTING": 42,
            "WINVER": {"value": "0x0502", "type": "STRING"},
            "CMAKE_CXX_FLAGS": "-D WINVER=0x0502"})
        hook.post_package_id(owner)
        hook.pre_generate(owner)
        hook.pre_generate(owner)
        self.assertEqual(owner.conf.get("tools.build:cflags"), flags)
        self.assertIn("WINVER=0x0502", owner.conf.get("tools.build:defines"))

    def test_other_dependencies_ignore_graph_wide_xp_selector(self):
        hook, owner = self.fixture(True, Settings(os="Linux", compiler="gcc"), name="zlib")
        original = copy.deepcopy(owner.conf.values)
        hook.post_package_id(owner)
        hook.pre_generate(owner)
        self.assertEqual(owner.info.conf.values, {})
        for key, _ in CHANNELS:
            self.assertEqual(owner.conf.get(key), original[key])
        self.assertEqual(owner.conf.get(VARIABLES)["CMAKE_POLICY_VERSION_MINIMUM"], "3.5")

    def test_post_package_id_uses_info_settings_without_live_settings_access(self):
        hook, owner = self.fixture(True, Settings(arch="x86"))

        class PackageIdOwner:
            name = "physfs"
            conf = owner.conf
            info = owner.info

            @property
            def settings(self):
                raise AssertionError("live Conan settings accessed during package_id")

        hook.post_package_id(PackageIdOwner())
        self.assertEqual(owner.info.conf.values, {FLOOR: "0x0501", CONFIG: "directory-flags-xp-v1"})

    def test_actual_conan_ids_keep_historical_default_and_partition_xp(self):
        try:
            from conan import ConanFile
            from conan.internal.errors import conanfile_remove_attr
            from conan.internal.model.conf import Conf as ConanConf
            from conan.internal.model.info import ConanInfo, RequirementsInfo
            from conan.internal.model.settings import Settings as ConanSettings
        except ModuleNotFoundError:
            raise unittest.SkipTest("installed Conan lifecycle unavailable")
        hook, _ = self.fixture()
        ids = []
        for xp in (_MISSING, False, True):
            owner = ConanFile("physfs-xp-floor-lifecycle")
            owner.name = "physfs"
            owner.settings = ConanSettings({"os": ["Windows"], "arch": ["x86_64"],
                "compiler": {"clang": {"version": ["22"], "libcxx": ["libc++"], "cppstd": ["20"]}}})
            owner.settings.update_values([("os", "Windows"), ("arch", "x86_64"),
                ("compiler", "clang"), ("compiler.version", "22"),
                ("compiler.libcxx", "libc++"), ("compiler.cppstd", "20")])
            # The pinned C-only PhysFS recipe removes these before package_id.
            owner.settings.rm_safe("compiler.cppstd")
            owner.settings.rm_safe("compiler.libcxx")
            owner.conf = ConanConf()
            if xp is not _MISSING:
                owner.conf.define(XP, xp)
            owner.info = ConanInfo(settings=owner.settings.copy_conaninfo_settings(),
                options=owner.options.copy_conaninfo_options(), reqs_info=RequirementsInfo({}),
                build_requires_info=RequirementsInfo({}), conf=ConanConf())
            historical = ConanInfo(settings=owner.settings.copy_conaninfo_settings(),
                options=owner.options.copy_conaninfo_options(), reqs_info=RequirementsInfo({}),
                build_requires_info=RequirementsInfo({}), conf=ConanConf())
            historical.conf.define(FLOOR, "0x0601")
            historical.conf.define(CONFIG, "directory-flags-v1")
            with conanfile_remove_attr(owner, ["settings", "options", "cpp_info"], "package_id"):
                hook.post_package_id(owner)
            selected_id = owner.info.package_id()
            ids.append(selected_id)
            if xp is not True:
                self.assertEqual(selected_id, historical.package_id(), "modern package ID changed")
            else:
                self.assertNotEqual(selected_id, historical.package_id(), "XP reused modern identity")
            before = owner.info.conf.dumps()
            hook.pre_generate(owner)
            self.assertEqual(owner.info.conf.dumps(), before)
        self.assertEqual(ids[0], ids[1])
        self.assertNotEqual(ids[1], ids[2])

    def test_actual_conan_flag_rendering_contains_selected_xp_floor(self):
        try:
            from conan.tools.cmake.toolchain.blocks import ExtraFlagsBlock
        except ModuleNotFoundError:
            raise unittest.SkipTest("installed Conan flag block unavailable")
        for arch, floor in (("x86", "0x0501"), ("x86_64", "0x0502")):
            hook, owner = self.fixture(True, Settings(arch=arch))
            hook.pre_generate(owner)
            toolchain = SimpleNamespace(generator="Ninja", extra_cxxflags=[], extra_cflags=[],
                                        extra_sharedlinkflags=[], extra_exelinkflags=[])
            rendered = ExtraFlagsBlock(owner, toolchain, "extra_flags").get_rendered_content()
            self.assertIn("add_compile_definitions(", rendered)
            for name in ("_WIN32_WINNT", "WINVER"):
                self.assertIn(name + "=" + floor, rendered)
            self.assertNotIn("0x0601", rendered)

    def test_missing_selected_floor_or_identity_wires_fail_ordinary_assertions(self):
        source = HOOK_PATH.read_text(encoding="utf-8")
        for wire, replacement in (
                ('floor = {"x86": "0x0501", "x86_64": "0x0502"}[arch]', 'floor = _PHYSFS_FLOOR'),
                ('conanfile.info.conf.define(_PHYSFS_FLOOR_CONF, floor)', 'pass'),
                ('conanfile.info.conf.define(_PHYSFS_FLOOR_CONFIG_CONF, config)', 'pass'),
                ('flags.append(definition)', 'pass'), ('defines.append(definition)', 'pass')):
            with self.subTest(wire=wire):
                self.assertEqual(source.count(wire), 1, "one actual production owner wire required")
                namespace = {}
                exec(compile(source.replace(wire, replacement), str(HOOK_PATH), "exec"), namespace)
                _, owner = self.fixture(True)
                with self.assertRaises(AssertionError):
                    self.check_selection(SimpleNamespace(**namespace), owner, "0x0502", "directory-flags-xp-v1")


if __name__ == "__main__":
    unittest.main()
