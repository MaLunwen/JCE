"""Pure controls for the reviewed XP dependency-consumer configuration hook."""
import ast
import copy
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
HOOK = ROOT / "conan/hooks/hook_windows_xp_consumers.py"
XP = "user.jce:windows_xp"
VARIABLES = "tools.cmake.cmaketoolchain:extra_variables"
TOOLCHAIN = "C:/source/cmake/toolchains/windows-x64-xp-llvm.cmake"
DIGEST = "4a" * 32
VERSIONS = {"dav1d": "1.5.3", "libaom-av1": "3.13.3", "flecs": "4.1.5",
            "miniz": "3.0.2", "kuba-zip": "0.3.0", "bgfx": "1.146.9306-550"}


def load(path=HOOK, name="jce_xp_consumers_hook"):
    spec = importlib.util.spec_from_file_location(name, path)
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


class BuildEnv:
    def __init__(self, definitions=""):
        self.definitions = definitions

    def dumps(self):
        return self.definitions

    def define_path(self, name, value):
        other = [line for line in self.definitions.splitlines() if not line.startswith(name + "=")]
        self.definitions = "\n".join(other + [name + "=(path)" + value])


class Settings:
    def __init__(self, **values):
        self.values = {"os": "Windows", "arch": "x86_64", "compiler": "clang",
                       "compiler.version": "22", "compiler.libcxx": "libc++", **values}

    def get_safe(self, key):
        return self.values.get(key)


class WindowsXpConsumersHookTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.hook = load()

    def owner(self, name="dav1d", version=None, xp=True, settings=None, values=None):
        settings = settings or Settings()
        conf = {XP: xp, "user.jce:xp_consumer_toolchain": TOOLCHAIN,
                "tools.cmake.cmaketoolchain:user_toolchain": [TOOLCHAIN],
                "user.jce:xp_codec_header": "C:/owned ports/codec.h",
                "user.jce:xp_codec_headers_sha256": DIGEST,
                "user.jce:xp_stdio_header": "C:/owned ports/stdio.h",
                "user.jce:xp_stdio_headers_sha256": DIGEST,
                "user.jce:xp_bx_memory_header": "C:/owned ports/bx_memory.h",
                "user.jce:xp_bx_memory_headers_sha256": DIGEST,
                "tools.build:compiler_executables": {
                    "c": "C:/verified toolchain/bin/i686-w64-mingw32-clang.exe",
                    "rc": "C:/verified toolchain/bin/i686-w64-mingw32-windres.exe"},
                "tools.build:cflags": ["-DCALLER_C=9"],
                "tools.build:cxxflags": ["-DCALLER_CPP=7"],
                "tools.build:defines": ["CALLER_DEFINE=3"],
                VARIABLES: {"JCE_XP_TOOLCHAIN_ROOT": "C:/verified toolchain",
                            "THREADS_PREFER_PTHREAD_FLAG": True,
                            "CALLER_TYPED": {"value": "x", "type": "STRING", "cache": True, "force": False}}}
        conf.update(values or {})
        return SimpleNamespace(name=name, version=version or VERSIONS.get(name, "1"),
            conf=Conf(conf), settings=settings, buildenv=BuildEnv(),
            info=SimpleNamespace(settings=settings, conf=Conf()))

    def identity(self, owner):
        self.hook.post_package_id(owner)
        return copy.deepcopy(owner.info.conf.values)

    def reject(self, owner):
        before = copy.deepcopy(owner.conf.values)
        for callback in (self.hook.post_package_id, self.hook.pre_generate):
            with self.subTest(callback=callback.__name__), self.assertRaises(ValueError):
                callback(owner)
            self.assertEqual(owner.conf.values, before)
            self.assertEqual(owner.info.conf.values, {})

    def test_real_generated_xp_profile_channels_keep_selected_libcpp(self):
        producer = load(ROOT / "tools/build/xp_editor.py", "xp_consumer_actual_profile")
        for arch, conan_arch in (("x86", "x86"), ("x64", "x86_64")):
            toolchain = ROOT / "cmake/toolchains" / ("windows-" + arch + "-xp-llvm.cmake")
            profile = producer.target_profile(ROOT / "build/tests/xp-consumers-toolchain", arch, toolchain)
            conf = {}
            in_conf = False
            for line in profile.splitlines():
                if line.startswith("["):
                    in_conf = line == "[conf]"
                elif in_conf and "=" in line:
                    key, value = line.split("=", 1)
                    try:
                        conf[key] = ast.literal_eval(value)
                    except (ValueError, SyntaxError):
                        conf[key] = value
            conf["user.jce:xp_consumer_toolchain"] = toolchain.as_posix()
            self.assertIn("-stdlib=libc++", conf["tools.build:cxxflags"])
            for name in VERSIONS:
                with self.subTest(arch=arch, package=name):
                    owner = self.owner(name, settings=Settings(arch=conan_arch), values=conf)
                    before = copy.deepcopy(owner.conf.values)
                    identity = self.identity(owner)
                    self.hook.pre_generate(owner)
                    self.assertEqual(self.identity(owner), identity)
                    for channel in ("tools.build:cflags", "tools.build:cxxflags"):
                        for flag in before[channel]:
                            self.assertIn(flag, owner.conf.get(channel))
                    fingerprint = owner.info.conf.get(self.hook._INPUTS)
                    canonical = self.hook._configuration(owner, owner.info.settings)
                    self.assertEqual(canonical[3], fingerprint)
                    changed = self.owner(name, settings=Settings(arch=conan_arch),
                        values={**conf, "tools.build:cxxflags": [flag for flag in conf["tools.build:cxxflags"]
                            if flag != "-stdlib=libc++"]})
                    self.assertNotEqual(self.identity(changed), identity)

    def test_conflicting_standard_library_choices_refuse(self):
        for name in VERSIONS:
            for flags in (["-stdlib=libstdc++"], ["-stdlib=platform"],
                          ["-stdlib", "libc++"], ["--stdlib=libc++"],
                          ["-stdlib=libc++", "-stdlib=libstdc++"],
                          ["-stdlib=libstdc++", "-stdlib=libc++"]):
                self.reject(self.owner(name, values={"tools.build:cxxflags": flags}))

    def test_modern_and_unrelated_packages_are_exact_noops(self):
        for name in VERSIONS:
            owner = self.owner(name, xp=False, settings=Settings(os="Linux", compiler="gcc"))
            before = copy.deepcopy(owner.conf.values)
            self.assertEqual(self.identity(owner), {})
            self.hook.pre_generate(owner)
            self.assertEqual(owner.conf.values, before)
        owner = self.owner("unrelated", xp="invalid")
        self.assertEqual(self.identity(owner), {})
        self.hook.pre_generate(owner)

    def test_exact_admission_and_c_only_x86_x64(self):
        for name in VERSIONS:
            for arch in ("x86", "x86_64"):
                for libcxx in (None, "libc++"):
                    with self.subTest(name=name, arch=arch, libcxx=libcxx):
                        owner = self.owner(name, settings=Settings(arch=arch, **{"compiler.libcxx": libcxx}))
                        self.assertTrue(self.identity(owner))
                        self.hook.pre_generate(owner)
        for name in VERSIONS:
            self.reject(self.owner(name, version="unreviewed"))
        for xp in (None, 0, 1, "True", [], {}):
            self.reject(self.owner(xp=xp))
        for values in ({"os": "WindowsStore"}, {"os": "Linux"}, {"compiler": "msvc"},
                       {"compiler.version": "21"}, {"compiler.frontend": "MSVC"},
                       {"compiler.libcxx": "libstdc++11"}, {"arch": "armv8"},
                       {"compiler.runtime": "static"}, {"compiler.runtime_type": "Release"},
                       {"compiler.runtime_version": "v144"}):
            self.reject(self.owner(settings=Settings(**values)))

    def test_partition_idempotence_and_unrelated_flags_preserved(self):
        for name in VERSIONS:
            owner = self.owner(name)
            before = copy.deepcopy(owner.conf.values)
            identity = self.identity(owner)
            self.hook.pre_generate(owner)
            once = copy.deepcopy(owner.conf.values)
            self.hook.pre_generate(owner)
            self.assertEqual(owner.conf.values, once)
            self.assertEqual(self.identity(owner), identity)
            generated = dict(owner.conf.get(VARIABLES))
            if name == "bgfx":
                self.assertIn(self.hook._BGFX_ISA_INCLUDE, generated)
                del generated[self.hook._BGFX_ISA_INCLUDE]
            self.assertEqual(generated, before[VARIABLES])
            for channel in ("tools.build:cflags", "tools.build:cxxflags", "tools.build:defines"):
                for flag in before[channel]:
                    self.assertIn(flag, owner.conf.get(channel))
            joined = " ".join(owner.conf.get("tools.build:cflags"))
            if name == "bgfx":
                self.assertIn("-DMINIZ_NO_STDIO=1", owner.conf.get("tools.build:cflags"))
                self.assertIn("-DJCE_WINDOWS_XP_SYNC_PORT=1", owner.conf.get("tools.build:cflags"))
                self.assertIn("-DJCE_WINDOWS_XP_CODEC_PORT=1", owner.conf.get("tools.build:cflags"))
                self.assertIn(self.hook._spellings("C:/owned ports/codec.h")[1], owner.conf.get("tools.build:cflags"))
                cpp = " ".join(owner.conf.get("tools.build:cxxflags"))
                self.assertIn("-DMINIZ_NO_STDIO=1", owner.conf.get("tools.build:cxxflags"))
                self.assertIn(self.hook._spellings("C:/owned ports/bx_memory.h")[1], owner.conf.get("tools.build:cxxflags"))
                self.assertIn("-DJCE_WINDOWS_XP_BX_MEMORY_PORT=1", owner.conf.get("tools.build:cxxflags"))
                self.assertNotIn("codec.h", cpp)
                self.assertNotIn("JCE_WINDOWS_XP_SYNC_PORT", cpp)
                self.assertNotIn("JCE_WINDOWS_XP_CODEC_PORT", cpp)
                self.assertNotIn("bx_memory.h", joined)
                self.assertNotIn("JCE_WINDOWS_XP_BX_MEMORY_PORT", joined)
                for guard in ("JCE_WINDOWS_XP_SYNC_PORT", "JCE_WINDOWS_XP_CODEC_PORT", "MINIZ_NO_STDIO", "JCE_WINDOWS_XP_BX_MEMORY_PORT"):
                    self.assertIn(guard + "=1", owner.conf.get("tools.build:defines"))
            elif name in ("dav1d", "libaom-av1"):
                self.assertIn("-DJCE_WINDOWS_XP_SYNC_PORT=1", owner.conf.get("tools.build:cflags"))
                self.assertIn("-DJCE_WINDOWS_XP_CODEC_PORT=1", owner.conf.get("tools.build:cxxflags"))
                self.assertNotIn("STDIO_PORT", joined)
            else:
                self.assertIn("-DJCE_WINDOWS_XP_STDIO_PORT=1", owner.conf.get("tools.build:cflags"))
                self.assertNotIn("CODEC_PORT", joined)

    def test_header_relocation_reuses_only_equal_cohort_identity(self):
        for name, kind in (("dav1d", "codec"), ("libaom-av1", "codec"), ("bgfx", "codec"), ("bgfx", "bx_memory"), ("miniz", "stdio")):
            baseline = self.identity(self.owner(name))
            relocated = self.owner(name, values={"user.jce:xp_" + kind + "_header": "D:/relocated cohort/consumer.h"})
            self.assertEqual(self.identity(relocated), baseline)
            changed = self.owner(name, values={"user.jce:xp_" + kind + "_headers_sha256": "5b" * 32})
            self.assertNotEqual(self.identity(changed), baseline)
            changed = self.owner(name, values={"tools.build:cflags": ["-DCALLER_C=10"]})
            self.assertNotEqual(self.identity(changed), baseline)
            changed_cpp = self.owner(name, values={"tools.build:cxxflags": ["-DCALLER_CPP=11"]})
            self.assertNotEqual(self.identity(changed_cpp), baseline)
        self.assertNotEqual(self.identity(self.owner("miniz")), self.identity(self.owner("kuba-zip")))

    def test_meson_arguments_are_distinct_argv_and_cmake_keeps_escape_level(self):
        try:
            from conan.tools.meson.helpers import to_meson_value
        except ImportError:
            self.skipTest("installed Conan serializer unavailable")
        for name in ("dav1d", "libaom-av1"):
            owner = self.owner(name)
            self.hook.pre_generate(owner)
            flags = owner.conf.get("tools.build:cflags")
            if name == "dav1d":
                offset = flags.index("-include")
                self.assertEqual(flags[offset + 1], "C:/owned ports/codec.h")
                self.assertEqual(ast.literal_eval(to_meson_value(flags)), flags)
            else:
                self.assertIn('-include \\"C:/owned ports/codec.h\\"', flags)
                self.assertNotIn("-include", flags)

    def test_bad_paths_digests_and_toolchain_overrides_refuse(self):
        for header in ("relative.h", "C:relative.h", "C:/a/../b.h", "C:/a/./b.h", "C:/a/", "C:/a;evil.h",
                       "C:/a$ENV.h", "C:/a\nfile.h", "C:/a'file.h", "C:/a\"file.h"):
            self.reject(self.owner(values={"user.jce:xp_codec_header": header}))
        for digest in (None, "", "a" * 63, "A" * 64, "g" * 64):
            self.reject(self.owner(values={"user.jce:xp_codec_headers_sha256": digest}))
        for values in ({"tools.cmake.cmaketoolchain:user_toolchain": ["C:/other.cmake"]},
                       {"tools.cmake.cmaketoolchain:user_toolchain": [TOOLCHAIN, TOOLCHAIN]},
                       {"user.jce:xp_consumer_toolchain": "relative.cmake"},
                       {"tools.cmake.cmaketoolchain:toolchain_file": "C:/other.cmake"},
                       {"tools.cmake.cmaketoolchain:enabled_blocks": ["generic_system"]},
                       {"tools.meson.mesontoolchain:extra_machine_files": ["C:/other.ini"]}):
            self.reject(self.owner(values=values))

    def test_reserved_defines_undefines_and_opaque_flags_refuse(self):
        for channel in ("tools.build:cflags", "tools.build:cxxflags", "tools.build:defines"):
            for flag in ("-UJCE_WINDOWS_XP_CODEC_PORT", "-DJCE_WINDOWS_XP_SYNC_PORT=0",
                         "/DJCE_WINDOWS_XP_CODEC_PORT=2", "-D JCE_WINDOWS_XP_STDIO_PORT=1"):
                self.reject(self.owner(values={channel: [flag]}))
        for flags in (["@response.rsp"], ["-Xclang", "-include"], ["-Wp,-U,JCE_WINDOWS_XP_CODEC_PORT"],
                      ["-include", "C:/unverified.h"], ["-imacros", "C:/hidden.h"],
                      ["--config=C:/hidden.cfg"], ["--target=x86_64-pc-windows-msvc"]):
            self.reject(self.owner(values={"tools.build:cflags": flags}))
        self.reject(self.owner("bgfx", values={"tools.build:defines": ["MINIZ_NO_STDIO=0"]}))

    def test_typed_cmake_reserved_and_replacement_flags_refuse(self):
        for key, value in (("CMAKE_C_FLAGS", "-O2"), ("CMAKE_CXX_FLAGS_RELEASE", "-O3"),
                           ("CMAKE_SYSTEM_NAME", "WindowsStore"), ("JCE_WINDOWS_XP_CODEC_PORT", 0),
                           ("CMAKE_PROJECT_INCLUDE", "C:/hidden.cmake"), ("CMAKE_C_COMPILE_OBJECT", "compiler_without_flags")):
            for entry in (value, {"value": value, "type": "STRING", "cache": True, "force": False}):
                self.reject(self.owner(values={VARIABLES: {key: entry}}))
        for entry in ({"value": "x", "force": 1}, {"value": "x", "cache": "yes"},
                      {"value": "x", "unknown": 1}, {"value": "x", "type": []},
                      {"value": "x", "type": "STRING)"}, {"value": 'x"broken'}):
            self.reject(self.owner(values={VARIABLES: {"CALLER": entry}}))

    def test_cmake_variable_names_are_literal_identifiers(self):
        for name in ("", "bad;key", "key\n", "key)command("):
            self.reject(self.owner(values={VARIABLES: {name: "x"}}))

    def test_hook_is_pure_and_header_files_are_never_opened(self):
        policy = load(ROOT / "tools/lint/check_conan_source_policy.py", "xp_consumer_source_policy")
        self.assertEqual(policy.violations(HOOK.read_text(encoding="utf-8")), [])
        with patch("builtins.open", side_effect=AssertionError("hook touched files")):
            for name in VERSIONS:
                owner = self.owner(name)
                self.identity(owner)
                self.hook.pre_generate(owner)


    def test_missing_xp_selector_preserves_identity_and_configuration(self):
        owner = self.owner()
        del owner.conf.values[XP]
        before = copy.deepcopy(owner.conf.values)
        self.assertEqual(self.identity(owner), {})
        self.hook.pre_generate(owner)
        self.assertEqual(owner.conf.values, before)

    def test_path_aliases_and_conflicting_function_macros_refuse(self):
        for path in ("C:/ambiguous./consumer.h", "C:/ambiguous /consumer.h", "C:/a//consumer.h",
                     "C:/a/consumer.h:alternate", "C:/a/*/consumer.h", "C:/a/?/consumer.h", "C:/a/\tconsumer.h"):
            self.reject(self.owner(values={"user.jce:xp_codec_header": path}))
        for name in ("JCE_WINDOWS_XP_SYNC_PORT", "JCE_WINDOWS_XP_CODEC_PORT"):
            self.reject(self.owner(values={"tools.build:cflags": ["-D" + name + "(x)=1"]}))
        self.reject(self.owner(values={"tools.build:cflags": ["-imacrosC:/hidden.h"]}))

    def test_real_conan_identity_removes_live_settings_and_partitions_cohorts(self):
        try:
            from conan import ConanFile
            from conan.tools.env import Environment
            from conan.internal.errors import conanfile_remove_attr
            from conan.internal.model.conf import Conf as ConanConf
            from conan.internal.model.info import ConanInfo, RequirementsInfo
            from conan.internal.model.settings import Settings as ConanSettings
        except ImportError:
            self.skipTest("installed Conan lifecycle unavailable")
        for name, cohort in (("dav1d", "codec"), ("bgfx", "codec"), ("bgfx", "bx_memory")):
            ids = []
            for xp, header, digest, arch in ((False, "C:/a.h", DIGEST, "x86_64"),
                    (True, "C:/a.h", DIGEST, "x86_64"), (True, "/relocated/a.h", DIGEST, "x86_64"),
                    (True, "C:/a.h", "9b" * 32, "x86_64"), (True, "C:/a.h", DIGEST, "x86")):
                owner = ConanFile("xp-consumer-id-control")
                owner.name = name
                owner._conan_buildenv = Environment()
                owner.version = VERSIONS[name]
                owner.settings = ConanSettings({"os": ["Windows"], "arch": ["x86", "x86_64"],
                    "compiler": {"clang": {"version": ["22"], "libcxx": ["libc++"], "cppstd": ["20"]}}})
                owner.settings.update_values([("os", "Windows"), ("arch", arch), ("compiler", "clang"),
                    ("compiler.version", "22"), ("compiler.libcxx", "libc++"), ("compiler.cppstd", "20")])
                owner.settings.rm_safe("compiler.libcxx")
                owner.settings.rm_safe("compiler.cppstd")
                owner.conf = ConanConf()
                for key, value in self.owner(name=name, values={"user.jce:xp_" + cohort + "_header": header,
                        "user.jce:xp_" + cohort + "_headers_sha256": digest}, xp=xp).conf.values.items():
                    owner.conf.define(key, value)
                owner.info = ConanInfo(settings=owner.settings.copy_conaninfo_settings(),
                    options=owner.options.copy_conaninfo_options(), reqs_info=RequirementsInfo({}),
                    build_requires_info=RequirementsInfo({}), conf=ConanConf())
                baseline = owner.info.package_id()
                with conanfile_remove_attr(owner, ["settings", "options", "cpp_info"], "package_id"):
                    self.hook.post_package_id(owner)
                actual = owner.info.package_id()
                self.assertEqual(actual == baseline, not xp)
                ids.append(actual)
            self.assertEqual(ids[1], ids[2])
            self.assertEqual(len(set(ids)), 4)

    def test_installed_conan_meson_argv_and_cmake_template_preserve_operands(self):
        try:
            from conan.tools.meson.toolchain import MesonToolchain
            from conan.tools.cmake.toolchain.blocks import ExtraFlagsBlock
            from jinja2 import Template
        except ImportError:
            self.skipTest("installed Conan generators unavailable")
        owner = self.owner()
        self.hook.pre_generate(owner)
        meson = SimpleNamespace(_conanfile_conf=owner.conf, _sys_root=None, extra_ldflags=[],
            threads_flags=[], apple_extra_flags=[], arch_flag="-m64", arch_link_flag="-m64",
            extra_cxxflags=[], extra_cflags=[], _rpath_link_flag=[], extra_defines=[])
        actual = MesonToolchain._get_extra_flags(meson)
        for channel in ("cflags", "cxxflags"):
            offset = actual[channel].index("-include")
            self.assertEqual(actual[channel][offset + 1], "C:/owned ports/codec.h")
            self.assertIn("-DJCE_WINDOWS_XP_CODEC_PORT=1", actual[channel])
        for name in ("libaom-av1", "bgfx"):
            owner = self.owner(name)
            self.hook.pre_generate(owner)
            rendered = Template(ExtraFlagsBlock._template).render(cflags=owner.conf.get("tools.build:cflags"),
                cxxflags=owner.conf.get("tools.build:cxxflags"), defines=owner.conf.get("tools.build:defines"),
                suffix="", config="")
            c_line = next(line for line in rendered.splitlines() if "string(APPEND CONAN_C_FLAGS " in line)
            cpp_line = next(line for line in rendered.splitlines() if "string(APPEND CONAN_CXX_FLAGS " in line)
            self.assertIn(r'-include \"C:/owned ports/codec.h\"', c_line)
            self.assertIn("JCE_WINDOWS_XP_CODEC_PORT=1", c_line)
            if name == "bgfx":
                self.assertIn("MINIZ_NO_STDIO=1", c_line)
                self.assertIn("MINIZ_NO_STDIO=1", cpp_line)
                self.assertIn(r'-include \"C:/owned ports/bx_memory.h\"', cpp_line)
                self.assertIn("JCE_WINDOWS_XP_BX_MEMORY_PORT=1", cpp_line)
                self.assertNotIn("codec.h", cpp_line)
                self.assertNotIn("JCE_WINDOWS_XP_CODEC_PORT", cpp_line)
                self.assertNotIn("JCE_WINDOWS_XP_SYNC_PORT", cpp_line)
                self.assertNotIn("bx_memory.h", c_line)
                self.assertNotIn("JCE_WINDOWS_XP_BX_MEMORY_PORT", c_line)
            else:
                self.assertIn(r'-include \"C:/owned ports/codec.h\"', cpp_line)

    def test_bgfx_codec_aliases_cannot_be_forced_into_cpp_flags(self):
        for flags in ([self.hook._spellings("C:/owned ports/codec.h")[1]],
                ["-DJCE_WINDOWS_XP_CODEC_PORT=1"], ["-DJCE_WINDOWS_XP_SYNC_PORT=1"]):
            self.reject(self.owner("bgfx", values={"tools.build:cxxflags": flags}))

    def test_bgfx_bx_memory_aliases_cannot_be_forced_into_c_flags(self):
        for flags in ([self.hook._spellings("C:/owned ports/bx_memory.h")[1]],
                ["-DJCE_WINDOWS_XP_BX_MEMORY_PORT=1"]):
            self.reject(self.owner("bgfx", values={"tools.build:cflags": flags}))

    def test_bgfx_memory_cohort_is_required_and_conflict_checked(self):
        for key, value in (("user.jce:xp_bx_memory_header", None),
                ("user.jce:xp_bx_memory_header", "relative.h"),
                ("user.jce:xp_bx_memory_headers_sha256", None),
                ("user.jce:xp_bx_memory_headers_sha256", "A" * 64)):
            self.reject(self.owner("bgfx", values={key: value}))
        for channel in ("tools.build:cxxflags", "tools.build:defines"):
            for flag in ("-UJCE_WINDOWS_XP_BX_MEMORY_PORT", "-DJCE_WINDOWS_XP_BX_MEMORY_PORT=0"):
                self.reject(self.owner("bgfx", values={channel: [flag]}))
        self.reject(self.owner("bgfx", values={VARIABLES: {
            "JCE_WINDOWS_XP_BX_MEMORY_PORT": {"value": 0, "type": "BOOL", "cache": True}}}))

    def test_non_bgfx_identity_is_unchanged_and_requires_no_memory_cohort(self):
        historical = {"dav1d": "02d09283cf53cd130bbc277fa955b867a2761a077332962154ed679607dc14c8",
            "libaom-av1": "cd0f75696c87dfe42e7722e5c0decbb713fc9cd804d902377919bdb3b4f67697",
            "flecs": "3628183b53bca9956ec8d0f5d35a6161ab657f77afc60424de0b499b1bb43557",
            "miniz": "0894f19117c86749447ca92613d377b37f7abdab46f9fe0724073f80f2dd7504",
            "kuba-zip": "455c72cfcce688b0da057d807c649e83fd042dfe18c67d0fa78f19013d13bf76"}
        for name, expected in historical.items():
            for present in (False, True):
                owner = self.owner(name)
                for key in ("user.jce:xp_bx_memory_header", "user.jce:xp_bx_memory_headers_sha256"):
                    if present:
                        owner.conf.values[key] = "invalid-but-unselected"
                    else:
                        del owner.conf.values[key]
                identity = self.identity(owner)
                self.assertEqual(identity[self.hook._INPUTS], expected)
                self.assertNotIn("user.jce:xp_bx_memory_headers_sha256", identity)
                self.hook.pre_generate(owner)
                self.assertNotIn("bx_memory.h", " ".join(owner.conf.get("tools.build:cxxflags")))

    def test_typed_metadata_and_toolchain_request_enter_identity(self):
        baseline = self.identity(self.owner())
        variables = self.owner().conf.get(VARIABLES)
        variables["CALLER_TYPED"]["cache"] = False
        self.assertNotEqual(self.identity(self.owner(values={VARIABLES: variables})), baseline)
        other = "D:/verified/windows-x64-xp-llvm.cmake"
        self.assertNotEqual(self.identity(self.owner(values={"user.jce:xp_consumer_toolchain": other,
            "tools.cmake.cmaketoolchain:user_toolchain": [other]})), baseline)
        for key in (self.hook._CONFIG, self.hook._INPUTS):
            owner = self.owner()
            owner.info.conf.define(key, "conflicting")
            before = copy.deepcopy(owner.info.conf.values)
            with self.assertRaises(ValueError):
                self.hook.post_package_id(owner)
            self.assertEqual(owner.info.conf.values, before)

    def test_ordinary_missing_content_identity_is_detected(self):
        source = HOOK.read_text(encoding="utf-8")
        old = "digest = conanfile.conf.get(sha_key)"
        self.assertEqual(source.count(old), 1)
        namespace = {"__name__": "xp_consumer_digest_negative_control"}
        exec(compile(source.replace(old, 'digest = "4a" * 32'), str(HOOK), "exec"), namespace)
        with patch.object(self, "hook", SimpleNamespace(**namespace)):
            with self.assertRaises(AssertionError):
                self.test_header_relocation_reuses_only_equal_cohort_identity()

    def test_ordinary_missing_forced_include_is_detected(self):
        source = HOOK.read_text(encoding="utf-8")
        old = "if header and not included:"
        self.assertEqual(source.count(old), 1)
        namespace = {"__name__": "xp_consumer_include_negative_control"}
        exec(compile(source.replace(old, "if False and header and not included:"), str(HOOK), "exec"), namespace)
        owner = self.owner()
        self.hook.pre_generate(owner)
        self.assertIn("-include", owner.conf.get("tools.build:cflags"))
        owner = self.owner()
        namespace["pre_generate"](owner)
        with self.assertRaises(AssertionError):
            self.assertIn("-include", owner.conf.get("tools.build:cflags"))


    def test_x86_bgfx_forced_separate_layout_preserves_flags_and_is_idempotent(self):
        owner = self.owner("bgfx", settings=Settings(arch="x86"), values={
            "tools.build:cxxflags": ["-DCALLER_CPP=7", "-O3", "-msse2"]})
        before = copy.deepcopy(owner.conf.values)
        expected = {"value": False, "type": "BOOL", "cache": True, "force": True}
        identity = self.identity(owner)
        self.assertEqual(owner.conf.values, before, "package ID must not mutate generation configuration")
        selected = self.hook._configuration(owner, owner.info.settings)
        self.assertEqual(selected[0][VARIABLES]["BX_AMALGAMATED"], expected)
        self.hook.pre_generate(owner)
        once = copy.deepcopy(owner.conf.values)
        self.hook.pre_generate(owner)
        self.assertEqual(owner.conf.values, once)
        self.assertEqual(self.identity(owner), identity)
        self.assertEqual(owner.conf.get(VARIABLES)["BX_AMALGAMATED"], expected)
        for key, value in before[VARIABLES].items():
            self.assertEqual(owner.conf.get(VARIABLES)[key], value)
        for channel in ("tools.build:cflags", "tools.build:cxxflags"):
            for flag in before[channel]:
                self.assertIn(flag, owner.conf.get(channel))
            self.assertNotIn("-O2", owner.conf.get(channel))
            self.assertNotIn("-O1", owner.conf.get(channel))

    def test_x86_false_layout_requests_share_one_normalized_identity(self):
        base = self.owner("bgfx", settings=Settings(arch="x86"))
        expected = self.identity(base)
        for entry in (False, 0, "OFF", "false", "0", "NO", "N", {"value": False},
                {"value": "OFF", "type": "BOOL", "cache": True, "force": True}):
            variables = copy.deepcopy(base.conf.get(VARIABLES))
            variables["BX_AMALGAMATED"] = entry
            owner = self.owner("bgfx", settings=Settings(arch="x86"), values={VARIABLES: variables})
            self.assertEqual(self.identity(owner), expected)
            self.hook.pre_generate(owner)
            self.assertEqual(owner.conf.get(VARIABLES)["BX_AMALGAMATED"],
                {"value": False, "type": "BOOL", "cache": True, "force": True})

    def test_x86_amalgamation_value_and_typed_metadata_conflicts_refuse(self):
        for entry in (True, 1, "ON", "TRUE", "AUTO", "", "NOTFOUND", "IGNORE",
                {"value": False, "type": "STRING"}, {"value": False, "cache": False},
                {"value": False, "force": False}, {"value": False, "type": "BOOL", "cache": 1}):
            self.reject(self.owner("bgfx", settings=Settings(arch="x86"),
                values={VARIABLES: {"BX_AMALGAMATED": entry}}))

    def test_actual_conan_extra_variables_render_forced_bool_cache(self):
        try:
            from conan.internal.model.conf import Conf as ConanConf
            from conan.tools.cmake.toolchain.blocks import ExtraVariablesBlock
        except ImportError:
            self.skipTest("installed Conan CMake variable generator unavailable")
        owner = self.owner("bgfx", settings=Settings(arch="x86"))
        self.hook.pre_generate(owner)
        conf = ConanConf()
        conf.define(VARIABLES, owner.conf.get(VARIABLES))
        block = ExtraVariablesBlock(SimpleNamespace(conf=conf), None, "extra_variables")
        rendered = block.get_rendered_content()
        self.assertIn('set(BX_AMALGAMATED False CACHE BOOL "BX_AMALGAMATED" FORCE)', rendered)
        self.assertIn('set(CMAKE_PROJECT_bgfx_INCLUDE "C:/owned ports/windows-xp-bgfx-isa.cmake" '
                      'CACHE FILEPATH "CMAKE_PROJECT_bgfx_INCLUDE" FORCE)', rendered)
        self.assertIn('set(CALLER_TYPED "x" CACHE STRING "CALLER_TYPED")', rendered)
        self.assertNotIn("set(BX_AMALGAMATED True", rendered)

    def test_x64_bgfx_isa_invalidates_old_binaries_and_preserves_layout(self):
        owner = self.owner("bgfx")
        identity = self.identity(owner)
        self.assertNotEqual(identity[self.hook._INPUTS],
            "1dd2a1e32cf44f4170a3ef246dd0e7644c4e2a7471c7b8fe2ba54150be4e1e43")
        for variables in ({}, {"BX_AMALGAMATED": True}, {"BX_AMALGAMATED": False},
                {"BX_AMALGAMATED": {"value": False, "type": "BOOL", "cache": True, "force": False}}):
            owner = self.owner("bgfx", values={VARIABLES: variables})
            self.hook.pre_generate(owner)
            expected = {**variables, self.hook._BGFX_ISA_INCLUDE: {
                "value": "C:/owned ports/windows-xp-bgfx-isa.cmake",
                "type": "FILEPATH", "cache": True, "force": True}}
            self.assertEqual(owner.conf.get(VARIABLES), expected)

    def test_bgfx_isa_refuses_unreviewed_flags_and_project_include(self):
        for arch in ("x86", "x86_64"):
            settings = Settings(arch=arch)
            for channel in ("tools.build:cflags", "tools.build:cxxflags"):
                for flag in ("-msse4.2", "-mavx2", "-mpopcnt", "-march=native"):
                    self.reject(self.owner("bgfx", settings=settings, values={channel: [flag]}))
            self.reject(self.owner("bgfx", settings=settings, values={VARIABLES: {
                self.hook._BGFX_ISA_INCLUDE: "C:/caller.cmake"}}))


    def test_x86_dav1d_resource_binding_is_pure_and_idempotent(self):
        owner = self.owner(settings=Settings(arch="x86"))
        owner.buildenv = BuildEnv("CALLER=preserved")
        before = copy.deepcopy(owner.conf.values)
        identity = self.identity(owner)
        self.assertEqual(owner.conf.values, before)
        self.assertEqual(owner.buildenv.dumps(), "CALLER=preserved")
        self.hook.pre_generate(owner)
        once = owner.buildenv.dumps()
        self.assertIn("WINDRES=(path)C:/verified toolchain/bin/i686-w64-mingw32-windres.exe", once)
        self.assertIn("CALLER=preserved", once)
        self.hook.pre_generate(owner)
        self.assertEqual(owner.buildenv.dumps(), once)
        self.assertEqual(self.identity(owner), identity)
        owner.buildenv = BuildEnv("WINDRES=C:/verified toolchain/bin/i686-w64-mingw32-windres.exe")
        self.assertEqual(self.identity(owner), identity)

    def test_x86_resource_paths_are_bound_to_target_c_compiler(self):
        for root in ("D:/portable toolchain", "//server/share/portable toolchain", "/cross toolchain"):
            values = {VARIABLES: {"JCE_XP_TOOLCHAIN_ROOT": root},
                "tools.build:compiler_executables": {
                    "c": root + "/bin/i686-w64-mingw32-clang.exe",
                    "rc": root + "/bin/i686-w64-mingw32-windres.exe"}}
            owner = self.owner(settings=Settings(arch="x86"), values=values)
            identity = self.identity(owner)
            self.hook.pre_generate(owner)
            self.assertIn("WINDRES=(path)" + values["tools.build:compiler_executables"]["rc"], owner.buildenv.dumps())
            self.assertEqual(self.identity(owner), identity)
        base = self.owner(settings=Settings(arch="x86"))
        for values in ({"c": "C:/verified toolchain/bin/x86_64-w64-mingw32-clang.exe"},
                {"c": "C:/other/bin/i686-w64-mingw32-clang.exe"}, {"c": "relative.exe"},
                {"rc": None}, {"rc": "C:/verified toolchain/bin/windres.exe"},
                {"rc": "C:/other/bin/i686-w64-mingw32-windres.exe"},
                {"rc": "C:/verified toolchain/bin/../i686-w64-mingw32-windres.exe"}):
            compilers = {**base.conf.get("tools.build:compiler_executables"), **values}
            self.reject(self.owner(settings=Settings(arch="x86"),
                values={"tools.build:compiler_executables": compilers}))
        self.reject(self.owner(settings=Settings(arch="x86"), values={"tools.build:compiler_executables": {}}))
        self.reject(self.owner(settings=Settings(arch="x86"), values={VARIABLES: {"JCE_XP_TOOLCHAIN_ROOT": "relative"}}))

    def test_x86_resource_environment_conflicts_refuse_without_mutation(self):
        selected = "C:/verified toolchain/bin/i686-w64-mingw32-windres.exe"
        for definition in ("WINDRES=C:/other.exe", "WINDRES=!", "WINDRES+= " + selected,
                "WINDRES=+" + selected, "WINDRES=(path)" + selected + ";C:/other.exe",
                "WINDRES=" + selected + "\nWINDRES=" + selected,
                "WindRes=" + selected, "WINDRES=%OTHER%", "WINDRES=" + '"' + selected + '"'):
            owner = self.owner(settings=Settings(arch="x86"))
            owner.buildenv = BuildEnv(definition)
            self.reject(owner)
            self.assertEqual(owner.buildenv.dumps(), definition)

    def test_resource_configuration_is_unread_for_all_other_consumers(self):
        for name in VERSIONS:
            for arch in ("x86", "x86_64"):
                if name == "dav1d" and arch == "x86":
                    continue
                owner = self.owner(name, settings=Settings(arch=arch))
                identity = self.identity(owner)
                owner.conf.define("tools.build:compiler_executables", "unselected-invalid")
                owner.buildenv = None
                self.assertEqual(self.identity(owner), identity)
                self.hook.pre_generate(owner)
                self.assertIsNone(owner.buildenv)
        owner = self.owner(xp=False, settings=Settings(arch="x86"))
        owner.buildenv = None
        self.hook.pre_generate(owner)
        self.assertIsNone(owner.buildenv)

    def test_installed_conan_meson_uses_explicit_x86_resource_compiler(self):
        try:
            from conan import ConanFile
            from conan.tools.env import Environment, VirtualBuildEnv
            from conan.internal.model.conf import Conf as ConanConf
            from conan.internal.model.settings import Settings as ConanSettings
            from conan.tools.meson.toolchain import MesonToolchain
        except ImportError:
            self.skipTest("installed Conan Meson and environment generators unavailable")
        owner = ConanFile("xp-resource-generator-control")
        owner.name, owner.version = "dav1d", VERSIONS["dav1d"]
        owner.settings = ConanSettings({"os": ["Windows"], "arch": ["x86", "x86_64"],
            "build_type": ["Release"], "compiler": {"clang": {
                "version": ["22"], "libcxx": ["libc++"], "cppstd": ["20"]}}})
        owner.settings.update_values([("os", "Windows"), ("arch", "x86"), ("build_type", "Release"),
            ("compiler", "clang"), ("compiler.version", "22"), ("compiler.libcxx", "libc++"), ("compiler.cppstd", "20")])
        owner.settings_build = ConanSettings({"os": ["Windows"], "arch": ["x86_64"]})
        owner.settings_build.update_values([("os", "Windows"), ("arch", "x86_64")])
        owner.conf = ConanConf()
        for key, value in self.owner().conf.values.items():
            owner.conf.define(key, value)
        owner.conf_build = ConanConf()
        owner._conan_buildenv = Environment()
        owner._conan_buildenv.define("CALLER", "preserved")
        owner._conan_dependencies = SimpleNamespace(build=SimpleNamespace(topological_sort={}),
            host=SimpleNamespace(topological_sort={}))
        missing = MesonToolchain(owner)
        self.assertIsNone(missing.windres, "ordinary missing WINDRES must stay observable")
        self.assertNotIn("windres =", missing._content)
        self.hook.pre_generate(owner)
        actual = VirtualBuildEnv(owner).vars().get("WINDRES")
        self.assertEqual(actual, "C:/verified toolchain/bin/i686-w64-mingw32-windres.exe")
        self.assertEqual(VirtualBuildEnv(owner).vars().get("CALLER"), "preserved")
        meson = MesonToolchain(owner)
        self.assertEqual(meson.windres, actual)
        self.assertIn("windres = '" + actual + "'", meson._content)
        self.assertIn("cpu_family = 'x86'", meson._content)

    def test_ordinary_missing_resource_binding_is_detected(self):
        source = HOOK.read_text(encoding="utf-8")
        old = 'conanfile.buildenv.define_path("WINDRES", selected[4]["windres"])'
        self.assertEqual(source.count(old), 1)
        namespace = {"__name__": "xp_resource_omission_control"}
        exec(compile(source.replace(old, "pass"), str(HOOK), "exec"), namespace)
        owner = self.owner(settings=Settings(arch="x86"))
        namespace["pre_generate"](owner)
        with self.assertRaises(AssertionError):
            self.assertIn("WINDRES=", owner.buildenv.dumps())



if __name__ == "__main__":
    unittest.main()
