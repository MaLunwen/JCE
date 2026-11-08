"""XP Flecs official addon exclusions: config, real Conan identity, consumer flags."""
import copy
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
ROOT=Path(__file__).resolve().parents[2]
HOOK=ROOT/"conan/hooks/hook_cmake_policy_fix.py"
XP="user.jce:windows_xp"
CONF="user.jce:flecs_xp_config"
REVISION="no-http-rest-default-os-v1"
MACROS=("FLECS_NO_HTTP","FLECS_NO_REST","FLECS_NO_OS_API_IMPL")
VARIABLES="tools.cmake.cmaketoolchain:extra_variables"
MISSING=object()


class Conf:
    def __init__(self,values=None):self.values=copy.deepcopy(values or {})
    def get(self,key,default=None,check_type=None):return self.values.get(key,default)
    def define(self,key,value):self.values[key]=value


class Settings:
    def __init__(self,**values):
        self.values={"os":"Windows","arch":"x86_64","compiler":"clang",**values}
    def get_safe(self,key):return self.values.get(key)


class XpFlecsConfigurationTests(unittest.TestCase):
    @staticmethod
    def fixture(xp=MISSING,settings=None,values=None,name="flecs"):
        spec=importlib.util.spec_from_file_location("xp_flecs_hook",HOOK)
        hook=importlib.util.module_from_spec(spec);spec.loader.exec_module(hook)
        settings=settings or Settings()
        conf=Conf({"tools.build:cflags":["-DCALLER_C=1"],"tools.build:cxxflags":["-DCALLER_CPP=1"],
                   "tools.build:defines":["CALLER_DEFINE=1"],VARIABLES:{"CALLER_VARIABLE":42},**(values or {})})
        if xp is not MISSING:conf.define(XP,xp)
        cpp=SimpleNamespace(defines=[],components={"_flecs":SimpleNamespace(defines=[])})
        owner=SimpleNamespace(name=name,settings=settings,conf=conf,cpp_info=cpp,
                              info=SimpleNamespace(settings=settings,conf=Conf()))
        return hook,owner

    def check_selection(self,hook,owner):
        hook.post_package_id(owner)
        self.assertEqual(owner.info.conf.values,{CONF:REVISION},"XP Flecs identity missing")
        hook.pre_generate(owner)
        once=copy.deepcopy(owner.conf.values)
        hook.pre_generate(owner)
        self.assertEqual(owner.conf.values,once)
        for key,prefix in (("tools.build:cflags","-D"),("tools.build:cxxflags","-D"),("tools.build:defines","")):
            for macro in MACROS:self.assertEqual(owner.conf.get(key).count(prefix+macro+"=1"),1,"Flecs compile definition missing")
        self.assertIn("-DCALLER_C=1",owner.conf.get("tools.build:cflags"))
        self.assertIn("-DCALLER_CPP=1",owner.conf.get("tools.build:cxxflags"))
        self.assertIn("CALLER_DEFINE=1",owner.conf.get("tools.build:defines"))
        self.assertEqual(owner.conf.get(VARIABLES)["CALLER_VARIABLE"],42)
        self.assertEqual(owner.conf.get(VARIABLES)["CMAKE_POLICY_VERSION_MINIMUM"],"3.5")
        hook.post_package_info(owner);hook.post_package_info(owner)
        for target in (owner.cpp_info,*owner.cpp_info.components.values()):
            for macro in MACROS:self.assertEqual(target.defines.count(macro+"=1"),1,"Flecs consumer definition missing")

    def reject(self,hook,owner):
        before=copy.deepcopy(owner.conf.values)
        for entry in ("post_package_id","pre_generate","post_package_info"):
            with self.subTest(entry=entry),self.assertRaisesRegex(ValueError,"Flecs|windows_xp"):
                getattr(hook,entry)(owner)
            self.assertEqual(owner.conf.values,before)
            self.assertEqual(owner.info.conf.values,{})
            self.assertEqual(owner.cpp_info.defines,[])

    def test_x86_x64_c_only_settings_and_all_library_consumer_channels(self):
        for arch in ("x86","x86_64"):
            for libcxx in (None,"","libc++"):
                with self.subTest(arch=arch,libcxx=libcxx):
                    self.check_selection(*self.fixture(True,Settings(arch=arch,**{"compiler.libcxx":libcxx})))

    def test_modern_default_and_false_add_no_flecs_identity_or_flags(self):
        for xp in (MISSING,False):
            hook,owner=self.fixture(xp,Settings(os="Linux",compiler="gcc",arch="armv8"))
            original=copy.deepcopy(owner.conf.values)
            hook.post_package_id(owner);hook.pre_generate(owner);hook.post_package_info(owner)
            self.assertEqual(owner.info.conf.values,{})
            self.assertEqual(owner.cpp_info.defines,[])
            for key in ("tools.build:cflags","tools.build:cxxflags","tools.build:defines"):
                self.assertEqual(owner.conf.get(key),original[key])

    def test_invalid_selector_target_abi_and_revision_refuse_before_mutation(self):
        for xp in (None,0,1,"true","on",""):
            self.reject(*self.fixture(xp))
        for settings in ({"os":"Linux"},{"os":"WindowsStore"},{"arch":"armv8"},
                         {"compiler":"msvc"},{"compiler":"gcc"},{"compiler.libcxx":"libstdc++11"},
                         {"compiler.frontend":"MSVC"},{"compiler.runtime":"dynamic"},
                         {"compiler.runtime_type":"Release"},{"compiler.runtime_version":"v143"}):
            self.reject(*self.fixture(True,Settings(**settings)))
        self.reject(*self.fixture(True,values={CONF:"other"}))
        self.reject(*self.fixture(True,values={VARIABLES:{"CMAKE_SYSTEM_NAME":{"value":"WindowsStore"}}}))

    def test_reserved_conflicts_in_flags_defines_and_typed_cmake_variables(self):
        inputs=(("tools.build:cflags",["-U","FLECS_NO_HTTP"]),
                ("tools.build:cxxflags",["/D","FLECS_NO_REST=0"]),
                ("tools.build:cflags",["-DFLECS_HTTP=0"]),
                ("tools.build:cxxflags",["-DFLECS_OS_API_IMPL=1"]),
                ("tools.build:cflags",["-DFLECS_CUSTOM_BUILD"]),
                ("tools.build:defines",["FLECS_NO_OS_API_IMPL=0"]),
                ("tools.build:defines",["FLECS_REST"]),
                ("tools.build:defines",[7]),
                (VARIABLES,{"CMAKE_C_FLAGS":"-U FLECS_NO_HTTP"}),
                (VARIABLES,{"CMAKE_CXX_FLAGS_RELEASE":{"value":"/D FLECS_REST=1"}}),
                (VARIABLES,{"FLECS_NO_HTTP":{"value":"0","type":"STRING"}}),
                (VARIABLES,{"FLECS_OS_API_IMPL":"OFF"}))
        for key,value in inputs:
            with self.subTest(key=key,value=value):self.reject(*self.fixture(True,values={key:value}))

    def test_matching_split_flags_and_unrelated_macro_names_are_preserved(self):
        hook,owner=self.fixture(True)
        flags=["/D","FLECS_NO_HTTP=1","-DFLECS_NO_REST=1","/DFLECS_NO_OS_API_IMPL=1",
               "-DFLECS_HTTP_EXTRA=9","-UNOT_FLECS_NO_HTTP"]
        owner.conf.define("tools.build:cflags",flags[:])
        hook.post_package_id(owner);hook.pre_generate(owner);hook.pre_generate(owner)
        self.assertEqual(owner.conf.get("tools.build:cflags"),flags)

    def test_other_dependency_ignores_xp_flecs_policy(self):
        hook,owner=self.fixture(True,name="zlib")
        original=copy.deepcopy(owner.conf.values)
        hook.post_package_id(owner);hook.pre_generate(owner);hook.post_package_info(owner)
        self.assertEqual(owner.info.conf.values,{})
        self.assertEqual(owner.conf.get("tools.build:cflags"),original["tools.build:cflags"])

    def test_actual_conan_lifecycle_and_ids_keep_modern_exact_and_partition_xp(self):
        try:
            from conan import ConanFile
            from conan.internal.errors import conanfile_remove_attr
            from conan.internal.model.conf import Conf as ConanConf
            from conan.internal.model.info import ConanInfo,RequirementsInfo
            from conan.internal.model.settings import Settings as ConanSettings
        except ModuleNotFoundError:raise unittest.SkipTest("installed Conan unavailable")
        hook,_=self.fixture();ids=[]
        for xp in (MISSING,False,True):
            owner=ConanFile("flecs-xp-lifecycle");owner.name="flecs"
            owner.settings=ConanSettings({"os":["Windows"],"arch":["x86_64"],
                "compiler":{"clang":{"version":["22"],"libcxx":["libc++"],"cppstd":["20"]}}})
            owner.settings.update_values([("os","Windows"),("arch","x86_64"),("compiler","clang"),
                ("compiler.version","22"),("compiler.libcxx","libc++"),("compiler.cppstd","20")])
            owner.settings.rm_safe("compiler.libcxx");owner.settings.rm_safe("compiler.cppstd")
            owner.conf=ConanConf()
            if xp is not MISSING:owner.conf.define(XP,xp)
            owner.info=ConanInfo(settings=owner.settings.copy_conaninfo_settings(),
                options=owner.options.copy_conaninfo_options(),reqs_info=RequirementsInfo({}),
                build_requires_info=RequirementsInfo({}),conf=ConanConf())
            old=owner.info.package_id()
            with conanfile_remove_attr(owner,["settings","options","cpp_info"],"package_id"):
                hook.post_package_id(owner)
            selected=owner.info.package_id();ids.append(selected)
            if xp is True:self.assertNotEqual(selected,old,"XP Flecs reused unqualified package")
            else:self.assertEqual(selected,old,"modern Flecs package ID changed")
        self.assertEqual(ids[0],ids[1]);self.assertNotEqual(ids[1],ids[2])

    def test_actual_conan_flag_block_renders_all_official_exclusions(self):
        try:
            from conan.tools.cmake.toolchain.blocks import ExtraFlagsBlock
        except ModuleNotFoundError:raise unittest.SkipTest("installed Conan flag block unavailable")
        hook,owner=self.fixture(True)
        hook.pre_generate(owner)
        toolchain=SimpleNamespace(generator="Ninja",extra_cxxflags=[],extra_cflags=[],
            extra_sharedlinkflags=[],extra_exelinkflags=[])
        rendered=ExtraFlagsBlock(owner,toolchain,"extra_flags").get_rendered_content()
        self.assertIn("add_compile_definitions(",rendered)
        for macro in MACROS:self.assertIn(macro+"=1",rendered)

    def test_missing_identity_library_and_consumer_wires_fail_ordinary_assertions(self):
        source=HOOK.read_text()
        for wire in ("conanfile.info.conf.define(_FLECS_XP_CONFIG_CONF, _FLECS_XP_CONFIG)",
                     "compiler_flags.append(flag)","directory_macros.append(definition)",
                     "target.defines.append(define)"):
            with self.subTest(wire=wire):
                self.assertEqual(source.count(wire),1)
                namespace={};exec(compile(source.replace(wire,"pass"),str(HOOK),"exec"),namespace)
                _,owner=self.fixture(True)
                with self.assertRaises(AssertionError):self.check_selection(SimpleNamespace(**namespace),owner)


if __name__=="__main__":unittest.main()
