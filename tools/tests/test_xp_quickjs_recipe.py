"""Real recipe/settings controls; no Conan CLI, network, cache or compiler."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

try:
    import yaml
    from conan.errors import ConanInvalidConfiguration
    from conan.internal.default_settings import default_settings_yml
    from conan.internal.model.conf import ConfDefinition
    from conan.internal.model.cpp_info import CppInfo
    from conan.internal.model.settings import Settings
except ModuleNotFoundError:
    CONAN_AVAILABLE = False
else:
    CONAN_AVAILABLE = True

ROOT = Path(__file__).resolve().parents[2]
RECIPE = ROOT / "conan/recipes/quickjs-ng/conanfile.py"
DATA = RECIPE.with_name("conandata.yml")
LEGACY_DEFINE = "JCE_QUICKJS_LEGACY_EXCEPTION_API=1"
WARNING = "-Wno-error=unterminated-string-initialization"
LIBRARY_OPTIONS = {
    "BUILD_QJS_LIBC": False,
    "BUILD_EXAMPLES": False,
    "BUILD_STATIC_QJS_LIBC": False,
    "QJS_BUILD_LIBC": False,
}


class ToolchainPort:
    def __init__(self, recipe):
        self.recipe = recipe
        self.cache_variables = {}
        self.extra_cflags = []
        self.extra_cxxflags = []
        self.generated = False

    def generate(self):
        self.generated = True


def load_recipe():
    spec = importlib.util.spec_from_file_location("jce_xp_quickjs_recipe_subject", RECIPE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(CONAN_AVAILABLE, "Conan 2 recipe/settings subject unavailable")
class QuickJSRecipeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_recipe()
        cls.source = RECIPE.read_text(encoding="utf-8")
        cls.data = yaml.safe_load(DATA.read_text(encoding="utf-8"))

    def recipe(self, version="0.4.1", conf="True", arch="x86_64", os_name="Windows",
               compiler="clang", libcxx="libc++", runtime=None, recipe_class=None, frontend=None):
        cls = recipe_class or self.module.QuickJSNgConan
        subject = cls()
        subject.version = version
        definition = yaml.safe_load(default_settings_yml)
        # Same normal Conan settings extension used for the actual Clang 22 lane.
        if "22" not in definition["compiler"]["clang"]["version"]:
            definition["compiler"]["clang"]["version"].append("22")
        if frontend is not None:
            definition["compiler"]["clang"]["frontend"] = [None, "GNU", "gnu", "MSVC", "msvc"]
        subject.settings = Settings(definition)
        values = [("os", os_name), ("arch", arch), ("build_type", "Release"),
                  ("compiler", compiler)]
        if compiler == "clang":
            values += [("compiler.version", "22"), ("compiler.cppstd", "20")]
            if libcxx is not None:
                values.append(("compiler.libcxx", libcxx))
        elif compiler == "msvc":
            values += [("compiler.version", "194"), ("compiler.runtime", "static"),
                       ("compiler.runtime_type", "Release"), ("compiler.cppstd", "20")]
        elif compiler == "gcc":
            values += [("compiler.version", "13"), ("compiler.libcxx", "libstdc++11"),
                       ("compiler.cppstd", "20")]
        if frontend is not None:
            values.append(("compiler.frontend", frontend))
        values += [("compiler." + key, value) for key, value in (runtime or {}).items()]
        subject.settings.update_values(values)
        subject.settings.validate()
        configuration = ConfDefinition()
        if conf is not None:
            configuration.loads("user.jce:windows_xp=" + conf)
        subject.conf = configuration.get_conanfile_conf(None)
        subject.cpp_info = CppInfo(set_defaults=True)
        subject.conan_data = self.data
        return subject

    def generate(self, subject):
        port = ToolchainPort(subject)
        with patch.object(self.module, "CMakeToolchain", return_value=port) as constructor:
            subject.generate()
        constructor.assert_called_once_with(subject)
        self.assertTrue(port.generated)
        self.assertEqual(port.cache_variables, LIBRARY_OPTIONS)
        self.assertEqual(port.extra_cxxflags, [])
        return port

    def test_pin_parses_exactly_and_modern_source_remains_first(self):
        sources = self.data["sources"]
        self.assertEqual(list(sources), ["0.16.2", "0.4.1"])
        self.assertEqual(sources["0.16.2"], {
            "url": "https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.16.2.tar.gz",
            "sha256": "97c80625b26775a4c7ca618c004d4ea24cf99cbf867e4eba78bd927a8b23d106",
        })
        self.assertEqual(sources["0.4.1"], {
            "url": "https://github.com/quickjs-ng/quickjs/archive/99a7fbb27615d4d6a0f037bbfb7e46c173d98701.tar.gz",
            "sha256": "55d254deef70488a127fb0ae54913356ee2f7415902eb89ccd1851802ff3bb65",
        })

    def test_real_source_method_passes_selected_pin_to_verified_fetch(self):
        for version in self.data["sources"]:
            with self.subTest(version=version):
                subject = self.recipe(version=version, conf="True" if version == "0.4.1" else None)
                with patch.object(self.module, "get") as fetch:
                    subject.source()
                fetch.assert_called_once_with(subject, **self.data["sources"][version], strip_root=True)

    def test_both_xp_architectures_validate_generate_and_export_legacy_metadata(self):
        for arch in ("x86", "x86_64"):
            with self.subTest(arch=arch):
                subject = self.recipe(arch=arch)
                subject.validate()
                self.assertEqual(self.generate(subject).extra_cflags, [WARNING])
                subject.package_info()
                self.assertEqual(subject.cpp_info.libs, ["qjs"])
                self.assertEqual(subject.cpp_info.defines, [LEGACY_DEFINE])
                self.assertEqual(subject.cpp_info.system_libs, [])
                self.assertEqual(subject.cpp_info.get_property("cmake_file_name"), "quickjs-ng")
                self.assertEqual(subject.cpp_info.get_property("cmake_target_name"), "quickjs-ng::quickjs-ng")

    def test_modern_generation_metadata_and_platform_libraries_stay_unchanged(self):
        for os_name, compiler, conf in (("Windows", "msvc", None), ("Windows", "msvc", "False"),
                                        ("Linux", "clang", None), ("FreeBSD", "clang", "False")):
            with self.subTest(os=os_name, compiler=compiler, conf=conf):
                subject = self.recipe(version="0.16.2", conf=conf, os_name=os_name, compiler=compiler)
                subject.validate()
                self.assertEqual(self.generate(subject).extra_cflags, [])
                subject.package_info()
                self.assertEqual(subject.cpp_info.defines, [])
                self.assertEqual(subject.cpp_info.libs, ["qjs"])
                expected = [] if os_name == "Windows" else ["m", "dl", "pthread", "atomic"]
                self.assertEqual(subject.cpp_info.system_libs, expected)

    def test_legacy_requires_explicit_true_and_xp_requires_legacy_version(self):
        for version, conf in (("0.4.1", None), ("0.4.1", "False"),
                              ("0.16.2", "True"), ("0.4.0", "True")):
            with self.subTest(version=version, conf=conf):
                with self.assertRaises(ConanInvalidConfiguration):
                    self.recipe(version=version, conf=conf).validate()

    def test_real_conan_conf_smart_boolean_coercions_do_not_bypass_strict_gate(self):
        for value in ("1", "0", "on", "off", "true", "false", '"True"', "[]", "{}"):
            with self.subTest(value=value):
                for version in ("0.4.1", "0.16.2"):
                    with self.assertRaisesRegex(ConanInvalidConfiguration, "explicit boolean"):
                        self.recipe(version=version, conf=value).validate()

    def test_wrong_target_compiler_library_and_missing_library_are_rejected(self):
        for kwargs in ({"os_name": "Linux"}, {"arch": "armv8"}, {"compiler": "msvc"},
                       {"compiler": "gcc"}, {"libcxx": "libstdc++11"}, {"libcxx": None}):
            with self.subTest(kwargs=kwargs):
                with self.assertRaisesRegex(ConanInvalidConfiguration, r"GNU Clang/libc\+\+"):
                    self.recipe(**kwargs).validate()

    def test_custom_frontend_settings_admit_only_gnu(self):
        for frontend in ("GNU", "gnu"):
            with self.subTest(frontend=frontend):
                subject = self.recipe(frontend=frontend)
                subject.validate()
                self.assertEqual(self.generate(subject).extra_cflags, [WARNING])
        for frontend in ("MSVC", "msvc"):
            with self.subTest(frontend=frontend):
                with self.assertRaisesRegex(ConanInvalidConfiguration, "GNU Clang"):
                    self.recipe(frontend=frontend).validate()

    def test_each_msvc_runtime_setting_conflicts_even_individually(self):
        for key, value in (("runtime", "static"), ("runtime", "dynamic"),
                           ("runtime_type", "Release"), ("runtime_version", "v144")):
            with self.subTest(key=key, value=value):
                with self.assertRaisesRegex(ConanInvalidConfiguration, "without MSVC runtime"):
                    self.recipe(runtime={key: value}).validate()

    def test_direct_generation_and_metadata_fail_before_partial_outputs(self):
        subject = self.recipe(version="0.16.2")
        with patch.object(self.module, "CMakeToolchain") as constructor:
            with self.assertRaises(ConanInvalidConfiguration):
                subject.generate()
        constructor.assert_not_called()
        with self.assertRaises(ConanInvalidConfiguration):
            subject.package_info()
        self.assertEqual(subject.cpp_info.defines, [])
        self.assertEqual(subject.cpp_info.libs, [])

    def test_package_keeps_license_headers_and_static_library_copy_contract(self):
        for version in ("0.16.2", "0.4.1"):
            with self.subTest(version=version):
                subject = self.recipe(version=version, conf="True" if version == "0.4.1" else None)
                subject.folders.set_base_source("source")
                subject.folders.set_base_build("build")
                subject.folders.set_base_package("package")
                with patch.object(self.module, "copy") as copier:
                    subject.package()
                calls = copier.call_args_list
                self.assertEqual(calls[0].args[1:3], ("LICENSE", subject.source_folder))
                self.assertEqual(Path(calls[0].args[3]).name, "licenses")
                self.assertIn("quickjs.h", [call.args[1] for call in calls])
                self.assertEqual([call.args[1] for call in calls[-2:]], ["*.lib", "*.a"])
                self.assertTrue(all(call.kwargs == {"keep_path": False} for call in calls[-2:]))

    def mutant(self, old, new):
        self.assertEqual(self.source.count(old), 1, "ordinary negative control needs one wire")
        namespace = {"__name__": "jce_quickjs_recipe_negative_control"}
        exec(compile(self.source.replace(old, new), str(RECIPE), "exec"), namespace)
        return namespace["QuickJSNgConan"]

    def test_ordinary_missing_explicit_selection_guard_is_detected(self):
        mutated = self.mutant('if str(self.version) == "0.4.1":', 'if False:')
        # The real guard refuses; the ordinary omitted-guard control admits it.
        with self.assertRaises(ConanInvalidConfiguration):
            self.recipe(conf=None).validate()
        self.recipe(conf=None, recipe_class=mutated).validate()

    def test_ordinary_missing_runtime_guard_is_detected(self):
        old = 'or any(self.settings.compiler.get_safe(key) is not None for key in\n                       ("runtime", "runtime_type", "runtime_version"))'
        mutated = self.mutant(old, 'or False')
        with self.assertRaises(ConanInvalidConfiguration):
            self.recipe(runtime={"runtime": "static"}).validate()
        self.recipe(runtime={"runtime": "static"}, recipe_class=mutated).validate()

    def test_ordinary_missing_legacy_define_is_detected(self):
        mutated = self.mutant('self.cpp_info.defines.append("JCE_QUICKJS_LEGACY_EXCEPTION_API=1")', 'pass')
        subject = self.recipe(recipe_class=mutated)
        subject.package_info()
        self.assertNotIn(LEGACY_DEFINE, subject.cpp_info.defines)
        normal = self.recipe()
        normal.package_info()
        self.assertIn(LEGACY_DEFINE, normal.cpp_info.defines)


if __name__ == "__main__":
    unittest.main()
