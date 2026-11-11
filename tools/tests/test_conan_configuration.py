"""Prove configuration hooks preserve upstream input and caller options."""
import ast
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[2]
class Conf:
    def __init__(self): self.values = {"tools.cmake.cmaketoolchain:extra_variables": {"CALLER_SETTING": 42}, "tools.build:cxxflags": ["-DCALLER_FLAG=1"], "tools.build:defines": ["CALLER_DEFINE=1"]}
    def get(self, key, default=None, check_type=None, **options): return self.values.get(key, default)
    def define(self, key, value): self.values[key] = value
class ConanConfigurationTests(unittest.TestCase):
    @staticmethod
    def bgfx_fixture(selector=None, os_name="Windows"):
        path = ROOT / "conan/hooks/hook_bgfx_wasm_fix.py"
        spec = importlib.util.spec_from_file_location("jce_bgfx_profiler_hook", path)
        hook = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(hook)
        conf = Conf()
        if selector is not None:
            conf.define("user.jce:bgfx_cpu_profiler", selector)
        cpp = SimpleNamespace(defines=[], components={
            name: SimpleNamespace(defines=[]) for name in ("bgfx", "bx", "bimg")})
        settings = SimpleNamespace(os=os_name, get_safe=lambda key: os_name if key == "os" else None)
        owner = SimpleNamespace(name="bgfx", settings=settings,
                                conf=conf, info=SimpleNamespace(conf=Conf(), settings=settings), cpp_info=cpp)
        return hook, owner

    def check_bgfx_cpu_profiler_selection(self, hook, owner, expected):
        hook.post_package_id(owner)
        self.assertEqual(owner.info.conf.values.get("user.jce:bgfx_cpu_profiler"),
                         "on" if expected else "off")
        self.assertEqual(owner.info.conf.values.get("user.jce:bgfx_cpu_profiler_config"),
                         "directory-defines-v1" if expected else None)
        hook.pre_generate(owner)
        hook.pre_generate(owner)
        flags = owner.conf.values["tools.build:cxxflags"]
        self.assertIn("-DCALLER_FLAG=1", flags)
        self.assertEqual(flags.count(f"-DBGFX_CONFIG_PROFILER={expected}"), 1)
        defines = owner.conf.values["tools.build:defines"]
        self.assertIn("CALLER_DEFINE=1", defines)
        self.assertEqual(defines.count(f"BGFX_CONFIG_PROFILER={expected}"), 1)
        self.assertEqual(owner.conf.values[
            "tools.cmake.cmaketoolchain:extra_variables"]["CALLER_SETTING"], 42)
        hook.post_package_info(owner)
        hook.post_package_info(owner)
        for target in (owner.cpp_info, *owner.cpp_info.components.values()):
            self.assertEqual(target.defines.count(
                f"JCE_BGFX_CPU_PROFILER_COMPILED={expected}"), 1)

    def test_bgfx_cpu_profiler_selects_real_compile_flags_identity_and_metadata(self):
        for selector, expected in ((None, 0), ("off", 0), ("on", 1)):
            for os_name in ("Windows", "Emscripten", "Macos"):
                with self.subTest(selector=selector, os=os_name):
                    hook, owner = self.bgfx_fixture(selector, os_name)
                    self.check_bgfx_cpu_profiler_selection(hook, owner, expected)

    def test_bgfx_cpu_profiler_ordinary_missing_owner_wire_controls(self):
        path = ROOT / "conan/hooks/hook_bgfx_wasm_fix.py"
        source = path.read_text(encoding="utf-8")
        for wire, replacement in (
                ('conanfile.info.conf.define(_CPU_PROFILER_CONF, "on" if profiler else "off")', 'pass'),
                ('flags.append(profiler_define)', 'pass'),
                ('conanfile.info.conf.define(_CPU_PROFILER_CONFIG_CONF, _CPU_PROFILER_CONFIG)', 'pass'),
                ('defines.append(profiler_definition)', 'pass'),
                ('"JCE_BGFX_CPU_PROFILER_COMPILED=%d" % profiler,', '')):
            with self.subTest(wire=wire):
                self.assertEqual(source.count(wire), 1, "expected one actual owner wire")
                namespace = {}
                exec(compile(source.replace(wire, replacement), str(path), "exec"), namespace)
                _, owner = self.bgfx_fixture("on")
                hook = SimpleNamespace(**namespace)
                # Every mutant executes the same production-owner assertions;
                # missing wiring must be an ordinary assertion, never setup error.
                with self.assertRaises(AssertionError):
                    self.check_bgfx_cpu_profiler_selection(hook, owner, 1)

    def test_bgfx_cpu_profiler_flag_tokens_keep_exact_macro_boundaries(self):
        for flags in (("-DBGFX_CONFIG_PROFILER=1",),
                      ("/D", "BGFX_CONFIG_PROFILER=1"),
                      ("-DNOT_BGFX_CONFIG_PROFILER=2", "-DBGFX_CONFIG_PROFILER_EXTRA=2",
                       "/UNOT_BGFX_CONFIG_PROFILER", '-I"C:\\BGFX_CONFIG_PROFILER\\include"')):
            with self.subTest(flags=flags):
                hook, owner = self.bgfx_fixture("on")
                owner.conf.values["tools.build:cxxflags"].extend(flags)
                try:
                    self.check_bgfx_cpu_profiler_selection(hook, owner, 1)
                except ValueError as error:
                    self.fail("valid exact compiler operands were refused: %s" % error)
        # Re-execute the complete owner with its former boundary-count parser.
        # Its legal second pre_generate and illegal value=2 are both checked
        # through ordinary unittest assertions rather than setup exceptions.
        path = ROOT / "conan/hooks/hook_bgfx_wasm_fix.py"
        source = path.read_text(encoding="utf-8")
        start = source.index("    # Non-POSIX tokenization")
        end = source.index("    variables = conanfile.conf.get(", start)
        old = '''    text = " ".join(flags)
    mentions = re.findall(r"\\bBGFX_CONFIG_PROFILER\\b", text)
    definitions = re.findall(
        r"(?:^|\\s)[-/]D\\s*BGFX_CONFIG_PROFILER\\s*=\\s*([01])(?=\\s|$)", text)
    if len(mentions) != len(definitions) or any(int(v) != enabled for v in definitions):
        raise ValueError("BGFX_CONFIG_PROFILER compiler flags conflict")
'''
        namespace = {}
        exec(compile("import re\n" + source[:start] + old + source[end:], str(path), "exec"), namespace)
        old_hook = SimpleNamespace(**namespace)
        _, owner = self.bgfx_fixture("on")
        with self.assertRaises(AssertionError):
            try:
                self.check_bgfx_cpu_profiler_selection(old_hook, owner, 1)
            except ValueError as error:
                self.fail("valid exact compiler operands were refused: %s" % error)
        _, owner = self.bgfx_fixture("on")
        owner.conf.define("tools.build:cxxflags", ["-DBGFX_CONFIG_PROFILER=2"])
        with self.assertRaises(AssertionError):
            with self.assertRaisesRegex(ValueError, "BGFX_CONFIG_PROFILER"):
                old_hook.pre_generate(owner)

    def test_bgfx_cpu_profiler_refuses_invalid_and_conflicting_input(self):
        for selector in ("", "1", "0", "true", "profile", "on;off"):
            with self.subTest(selector=selector):
                hook, owner = self.bgfx_fixture(selector)
                for entry in (hook.post_package_id, hook.pre_generate, hook.post_package_info):
                    with self.assertRaisesRegex(ValueError, "bgfx_cpu_profiler"):
                        entry(owner)
        for selector, flags in (("off", ["-DBGFX_CONFIG_PROFILER=1"]),
                                ("on", ["/DBGFX_CONFIG_PROFILER=0"]),
                                ("on", ["-U", "BGFX_CONFIG_PROFILER"]),
                                ("off", ["-D", "BGFX_CONFIG_PROFILER"]),
                                ("on", ["-DBGFX_CONFIG_PROFILER=2"]),
                                ("on", ["-DBGFX_CONFIG_PROFILER=1", "-DBGFX_CONFIG_PROFILER=0"])):
            with self.subTest(selector=selector, flags=flags):
                hook, owner = self.bgfx_fixture(selector)
                owner.conf.define("tools.build:cxxflags", flags)
                with self.assertRaisesRegex(ValueError, "BGFX_CONFIG_PROFILER"):
                    hook.pre_generate(owner)
        hook, owner = self.bgfx_fixture("on")
        owner.conf.values["tools.cmake.cmaketoolchain:extra_variables"]["BGFX_CONFIG_PROFILER"] = 0
        with self.assertRaisesRegex(ValueError, "BGFX_CONFIG_PROFILER"):
            hook.pre_generate(owner)
        for selector, definition in (("on", "BGFX_CONFIG_PROFILER=0"),
                                     ("off", "BGFX_CONFIG_PROFILER=1"),
                                     ("on", "BGFX_CONFIG_PROFILER=2"),
                                     ("on", "BGFX_CONFIG_PROFILER")):
            with self.subTest(selector=selector, definition=definition):
                hook, owner = self.bgfx_fixture(selector)
                owner.conf.values["tools.build:defines"].append(definition)
                with self.assertRaisesRegex(ValueError, "BGFX_CONFIG_PROFILER"):
                    hook.post_package_id(owner)

    def test_bgfx_cpu_profiler_ignores_other_packages(self):
        hook, owner = self.bgfx_fixture("invalid")
        owner.name = "unrelated"
        before = repr(owner)
        hook.post_package_id(owner); hook.pre_generate(owner); hook.post_package_info(owner)
        self.assertEqual(repr(owner), before)

    def test_every_hook_leaves_dependency_files_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "CMakeLists.txt"
            source.write_bytes(b"cmake_minimum_required(VERSION 2.8)\n# upstream\n")
            before = source.read_bytes()
            try:
                from conan.internal.model.settings import Settings
            except ModuleNotFoundError:
                raise unittest.SkipTest("installed Conan Settings fixture unavailable")
            settings = Settings({"os": ["Windows"], "build_type": ["Release"]})
            settings.update_values([("os", "Windows"), ("build_type", "Release")])
            c = SimpleNamespace(name="bgfx", settings=settings, conf=Conf(), source_folder=str(source.parent))
            for file in (ROOT / "conan/hooks").glob("hook_*.py"):
                spec = importlib.util.spec_from_file_location(file.stem, file)
                m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
                self.assertFalse(hasattr(m, "post_source"))
                self.assertFalse(hasattr(m, "pre_build"))
                m.pre_generate(c)
                self.assertEqual(source.read_bytes(), before)
            values = c.conf.values["tools.cmake.cmaketoolchain:extra_variables"]
            self.assertEqual(values["CALLER_SETTING"], 42)
            self.assertEqual(values["CMAKE_POLICY_VERSION_MINIMUM"], "3.5")
            self.assertEqual(values["BGFX_OPENGL_VERSION"], 31)
            self.assertIn("-DCALLER_FLAG=1", c.conf.values["tools.build:cxxflags"])
            self.assertIn("-DBGFX_CONFIG_MAX_MATRIX_CACHE=131072", c.conf.values["tools.build:cxxflags"])

    @staticmethod
    def renderer_closure(properties):
        # Fixture reproduces the upstream recipe's explicit component names
        # and CMakeDeps' package aggregate append behavior. No vendor edits.
        aggregate = properties.get(("bgfx", "cmake_target_name"), "bgfx::bgfx")
        graph = {
            "bgfx::bgfx": {"bgfx::bx", "bgfx::bimg"},
            "bgfx::bimg": {"bgfx::bx", "miniz::miniz"},
            "bgfx::bimg_encode": {"bgfx::bx", "libsquish::libsquish"},
            "bgfx::bimg_decode": {"bgfx::bx", "tinyexr::tinyexr"},
            "bgfx::texturec": {"bgfx::bimg", "bgfx::bimg_encode",
                               "bgfx::bimg_decode"},
        }
        graph.setdefault(aggregate, set()).update(graph.keys())
        def closure(root):
            visited, pending = set(), [root]
            while pending:
                target = pending.pop()
                if target not in visited:
                    visited.add(target)
                    pending.extend(graph.get(target, ()))
            return visited
        return aggregate, closure("bgfx::bgfx"), closure("bgfx::bimg_encode")

    @staticmethod
    def shaping_closure(properties):
        aggregate = properties.get(("harfbuzz", "cmake_target_name"), "harfbuzz::harfbuzz")
        graph = {"harfbuzz::harfbuzz": set(),
                 "harfbuzz::subset": {"harfbuzz::harfbuzz"}}
        graph.setdefault(aggregate, set()).update(graph.keys())
        visited, pending = set(), ["harfbuzz::harfbuzz"]
        while pending:
            target = pending.pop()
            if target not in visited:
                visited.add(target)
                pending.extend(graph.get(target, ()))
        return visited

    def test_recipe_separates_bgfx_aggregate_from_renderer_and_keeps_encoder(self):
        # Execute the actual generate method without importing Conan or
        # changing source/package caches. Real CMakeDeps generation is a
        # separate installed-SDK integration check.
        tree = ast.parse((ROOT / "conanfile.py").read_text())
        recipe = next(node for node in tree.body
                      if isinstance(node, ast.ClassDef) and node.name == "JCEConan")
        method = next(node for node in recipe.body
                      if isinstance(node, ast.FunctionDef) and node.name == "generate")
        emitted = {}
        class Deps:
            def __init__(self, owner):
                self.properties = {}
            def set_property(self, dependency, name, value, **kwargs):
                self.properties[(dependency, name)] = value
            def generate(self):
                emitted.update(self.properties)
        class Toolchain:
            def __init__(self, owner):
                pass
            def generate(self):
                pass
        namespace = {"CMakeDeps": Deps, "CMakeToolchain": Toolchain}
        exec(compile(ast.Module(body=[method], type_ignores=[]),
                     "conanfile.py", "exec"), namespace)
        # This closure control supplies the new configuration dependency;
        # shader policy and incremental selection have their own focused tests.
        conf = SimpleNamespace(get=lambda key, default=None, **kwargs: default,
                               define=lambda key, value: None)
        namespace["generate"](SimpleNamespace(settings=SimpleNamespace(os="Windows"), conf=conf))
        aggregate, renderer, encoder = self.renderer_closure(emitted)
        self.assertEqual(emitted[("harfbuzz", "cmake_target_name")], "harfbuzz::all")
        self.assertEqual(self.shaping_closure(emitted), {"harfbuzz::harfbuzz"})
        self.assertNotIn(("harfbuzz::harfbuzz", "cmake_target_name"), emitted)
        self.assertEqual(aggregate, "bgfx::all")
        self.assertIn("bgfx::bimg", renderer)
        self.assertIn("bgfx::bx", renderer)
        self.assertNotIn("bgfx::bimg_decode", renderer)
        self.assertNotIn("bgfx::texturec", renderer)
        self.assertIn("bgfx::bimg_encode", encoder)
        self.assertIn("libsquish::libsquish", encoder)
        self.assertNotIn(("bgfx::bgfx", "cmake_target_name"), emitted)
        self.assertEqual(emitted[("protobuf::libprotobuf", "cmake_target_name")],
                         "protobuf_BUILD::libprotobuf_BUILD")

    def test_unrenamed_shaping_aggregate_pollutes_core(self):
        self.assertIn("harfbuzz::subset", self.shaping_closure({}))
        self.assertNotIn("harfbuzz::subset", self.shaping_closure({
            ("harfbuzz", "cmake_target_name"): "harfbuzz::all"}))

    def test_runtime_protobuf_component_and_host_generator_stay_separate(self):
        cmake = (ROOT/"engine/CMakeLists.txt").read_text()
        self.assertIn("LINK_PRIVATE enet::enet protobuf::libprotobuf flecs::flecs_static", cmake)
        self.assertNotIn("LINK_PRIVATE enet::enet protobuf::protobuf", cmake)
        self.assertIn("protobuf_generate(", cmake)
        self.assertIn("TARGET jce_net", cmake)
        root = (ROOT/"CMakeLists.txt").read_text()
        self.assertIn("set(Protobuf_PROTOC_EXECUTABLE", root)
        self.assertIn("Conan build-context protoc", root)

    def test_unrenamed_aggregate_negative_control_pollutes_renderer(self):
        aggregate, renderer, _ = self.renderer_closure({})
        self.assertEqual(aggregate, "bgfx::bgfx")
        self.assertIn("bgfx::bimg_decode", renderer)
        self.assertIn("bgfx::texturec", renderer)

class ConanSourcePolicyTests(unittest.TestCase):
    def policy(self, source=None):
        path = ROOT / "tools/lint/check_conan_source_policy.py"
        spec = importlib.util.spec_from_file_location("jce_conan_source_policy_fixture", path)
        module = importlib.util.module_from_spec(spec)
        if source is None:
            spec.loader.exec_module(module)
        else:
            exec(compile(source, str(path), "exec"), module.__dict__)
        return module

    def check_real_hook_allowed(self, policy):
        hook = (ROOT / "conan/hooks/hook_bgfx_wasm_fix.py").read_text(encoding="utf-8")
        self.assertEqual(policy.violations(hook), [], "pure configuration lexer must remain allowed")

    def check_filesystem_access_refused(self, policy):
        self.assertTrue(policy.violations('import shlex\nshlex.os.unlink("dependency")'),
                        "filesystem access through lexer module must remain refused")

    def test_actual_hook_and_direct_pure_split_are_allowed(self):
        policy = self.policy()
        self.check_real_hook_allowed(policy)
        for source in ('import shlex\nshlex.split("flags", posix=False)',
                       'import shlex\nshlex.split(" ".join(flags), posix=False)'):
            self.assertEqual(policy.violations(source), [])
        self.check_filesystem_access_refused(policy)


    def test_canonical_hashing_whitelist_keeps_file_loader_and_reflection_controls(self):
        policy = self.policy()
        self.check_real_hook_allowed(policy)
        for source in (
                'import json\njson.dumps(data, sort_keys=True, ensure_ascii=True, allow_nan=False, separators=(",", ":"))',
                'import hashlib\nhashlib.sha256(canonical.encode("ascii")).hexdigest()'):
            self.assertEqual(policy.violations(source), [])
        self.check_json_file_access_refused(policy)
        self.check_hash_file_access_refused(policy)
        for source in ('import json as encoder', 'from json import dumps',
                       'import json\nloader=json', 'import json\njson.loads(text)',
                       'import json\ndef pre_generate(json): pass',
                       'import json\njson.dumps(data, sort_keys=True, ensure_ascii=True, allow_nan=False, separators=(",", ":"), default=loader)',
                       'import hashlib as digest', 'from hashlib import sha256',
                       'import hashlib\nloader=hashlib', 'import hashlib\nhashlib.new("sha256")',
                       'import hashlib\nhashlib.sha256(stream.read())',
                       'import hashlib\nhashlib.sha256(text.encode("utf-8"))',
                       'import hashlib\ndef pre_generate(hashlib): pass',
                       'import json\njson.__dict__["load"](stream)'):
            self.assertTrue(policy.violations(source), source)

    def check_json_file_access_refused(self, policy):
        self.assertTrue(policy.violations('import json\njson.dump(data, stream)'),
                        "JSON file access must remain refused")

    def check_hash_file_access_refused(self, policy):
        self.assertTrue(policy.violations('import hashlib\nhashlib.file_digest(stream, "sha256")'),
                        "hash file access must remain refused")

    def test_removed_hashing_restrictions_fail_ordinary_policy_assertions(self):
        path = ROOT / "tools/lint/check_conan_source_policy.py"
        source = path.read_text(encoding="utf-8")
        for wire, check, reason in (
                ('errors.append("json is restricted to canonical dumps calls")',
                 self.check_json_file_access_refused, "JSON file access must remain refused"),
                ('errors.append("hashlib is restricted to direct pure sha256 calls")',
                 self.check_hash_file_access_refused, "hash file access must remain refused")):
            self.assertEqual(source.count(wire), 1)
            policy = self.policy(source.replace(wire, "pass"))
            with self.assertRaisesRegex(AssertionError, reason):
                check(policy)

    def test_actual_self_check_keeps_all_filesystem_loader_controls(self):
        policy = self.policy()
        output = io.StringIO()
        with patch.object(policy.sys, "argv", ["check_conan_source_policy.py", "--self-check"]), \
                contextlib.redirect_stdout(output):
            self.assertFalse(policy.main())
        self.assertIn("self-check: PASS", output.getvalue())
        self.assertIn("Conan source policy: PASS", output.getvalue())

    def test_old_blanket_import_and_unrestricted_lexer_fail_ordinary_assertions(self):
        source = (ROOT / "tools/lint/check_conan_source_policy.py").read_text(encoding="utf-8")
        for wire, replacement, check, reason in (
                ('if not _is_pure_shlex_import(node):', 'if True:',
                 self.check_real_hook_allowed, "pure configuration lexer must remain allowed"),
                ('errors.append("shlex is restricted to direct pure string split calls")', 'pass',
                 self.check_filesystem_access_refused, "filesystem access through lexer module must remain refused")):
            with self.subTest(wire=wire):
                self.assertEqual(source.count(wire), 1)
                policy = self.policy(source.replace(wire, replacement))
                with self.assertRaisesRegex(AssertionError, reason):
                    check(policy)



class BgfxAnnotationConfigurationTests(unittest.TestCase):
    """Actual hook owner; annotation request is not compiled capability."""
    @staticmethod
    def fixture(selector="auto", config="Release", system="Windows", option=None):
        hook, owner = ConanConfigurationTests.bgfx_fixture()
        owner.settings = SimpleNamespace(os=system, get_safe=lambda key: config if key == "build_type" else system if key == "os" else None)
        owner.info.settings = owner.settings
        owner.conf.define("user.jce:bgfx_debug_annotation", selector)
        if option is not None:
            owner.conf.values["tools.cmake.cmaketoolchain:extra_variables"]["BGFX_CONFIG_DEBUG_ANNOTATION"] = option
        return hook, owner

    def check_selection(self, hook, owner, request, option, compiled):
        before_flags = list(owner.conf.values["tools.build:cxxflags"])
        before_defines = list(owner.conf.values["tools.build:defines"])
        before_option = owner.conf.values["tools.cmake.cmaketoolchain:extra_variables"].get("BGFX_CONFIG_DEBUG_ANNOTATION")
        hook.post_package_id(owner)
        default_compiled = int(owner.settings.os != "WindowsStore" and
                               owner.settings.get_safe("build_type") == "Debug")
        self.assertNotIn("user.jce:bgfx_debug_annotation", owner.info.conf.values)
        if compiled < 0 or compiled != default_compiled:
            self.assertEqual(owner.info.conf.values.get("user.jce:bgfx_debug_annotation_config"), "upstream-cmake-v1")
            option_identity = owner.info.conf.values.get("user.jce:bgfx_debug_annotation_option")
            if option >= 0:
                self.assertEqual(option_identity, "on" if option else "off")
            else:
                self.assertIsInstance(option_identity, str)
                self.assertTrue(option_identity.startswith("unknown:"))
            self.assertEqual(owner.info.conf.values.get("user.jce:bgfx_debug_annotation_compiled"), str(compiled))
        else:
            for key in ("option", "compiled", "config", "inputs"):
                self.assertNotIn("user.jce:bgfx_debug_annotation_" + key, owner.info.conf.values)
        if compiled < 0:
            self.assertRegex(owner.info.conf.values.get("user.jce:bgfx_debug_annotation_inputs", ""),
                             r"^[0-9a-f]{64}$")
        else:
            self.assertNotIn("user.jce:bgfx_debug_annotation_inputs", owner.info.conf.values)
        hook.pre_generate(owner); hook.pre_generate(owner)
        variables = owner.conf.values["tools.cmake.cmaketoolchain:extra_variables"]
        if before_option is not None:
            self.assertEqual(variables.get("BGFX_CONFIG_DEBUG_ANNOTATION"), before_option)
        elif request == "auto":
            self.assertNotIn("BGFX_CONFIG_DEBUG_ANNOTATION", variables)
        else:
            self.assertEqual(variables.get("BGFX_CONFIG_DEBUG_ANNOTATION"), option)
        self.assertEqual(variables["CALLER_SETTING"], 42)
        self.assertTrue(all(flag in owner.conf.values["tools.build:cxxflags"] for flag in before_flags))
        self.assertTrue(all(flag in owner.conf.values["tools.build:defines"] for flag in before_defines))
        self.assertFalse(any("BGFX_CONFIG_DEBUG_ANNOTATION" in flag for flag in
                             owner.conf.values["tools.build:cxxflags"] if flag not in before_flags))
        hook.post_package_info(owner); hook.post_package_info(owner)
        for target in (owner.cpp_info, *owner.cpp_info.components.values()):
            for name, value in (("REQUEST", -1 if request == "auto" else int(request == "on")),
                                ("OPTION", option), ("COMPILED", compiled)):
                self.assertEqual(target.defines.count(f"JCE_BGFX_DEBUG_ANNOTATION_{name}={value}"), 1)

    def test_actual_owner_resolves_upstream_debug_and_store_semantics(self):
        for system in ("Windows", "WindowsStore", "Linux", "Macos", "Emscripten"):
            for config in ("Release", "Debug", "RelWithDebInfo", "MinSizeRel"):
                for request, option in (("auto", 0), ("off", 0), ("on", 1)):
                    compiled = int(system != "WindowsStore" and (config == "Debug" or option))
                    with self.subTest(system=system, config=config, request=request):
                        hook, owner = self.fixture(request, config, system)
                        self.check_selection(hook, owner, request, option, compiled)

    def test_default_and_equivalent_options_keep_exact_historical_package_identity(self):
        for system, config, requests in (("Windows", "Release", ("auto", "off")),
                                         ("Windows", "Debug", ("auto", "off", "on")),
                                         ("WindowsStore", "Release", ("auto", "off", "on"))):
            for cpu in ("off", "on"):
                for request in requests:
                    hook, owner = self.fixture(request, config, system)
                    owner.conf.define("user.jce:bgfx_cpu_profiler", cpu)
                    hook.post_package_id(owner)
                    actual = {key: value for key, value in owner.info.conf.values.items() if key.startswith("user.jce:")}
                    expected = {"user.jce:bgfx_graphics_tier": "stable",
                                "user.jce:bgfx_cpu_profiler": cpu,
                                "user.jce:source_policy": "pristine-v1"}
                    if cpu == "on":
                        expected["user.jce:bgfx_cpu_profiler_config"] = "directory-defines-v1"
                    self.assertEqual(actual, expected, "unchanged default bgfx binary lost its historical package ID")
        hook, explicit = self.fixture("on")
        hook.post_package_id(explicit)
        hook, automatic = self.fixture("auto", option="ON")
        hook.post_package_id(automatic)
        self.assertEqual(explicit.info.conf.values, automatic.info.conf.values,
                         "equivalent enabled compiler configuration must reuse one package identity")

    def check_resolved_system_override_identity(self, hook, original, effective, compiled):
        _, baseline = self.fixture("auto", "Debug", original)
        hook.post_package_id(baseline)
        _, owner = self.fixture("auto", "Debug", original)
        owner.conf.values["tools.cmake.cmaketoolchain:extra_variables"]["CMAKE_SYSTEM_NAME"] = effective
        self.check_selection(hook, owner, "auto", 0, compiled)
        self.assertNotEqual(owner.info.conf.values, baseline.info.conf.values,
                            "resolved system override reused the historical opposite annotation binary")
        stripped = {key: value for key, value in owner.info.conf.values.items()
                    if not key.startswith("user.jce:bgfx_debug_annotation_")}
        self.assertEqual(stripped, baseline.info.conf.values)

    def test_resolved_system_overrides_partition_against_historical_settings_baseline(self):
        hook, _ = self.fixture()
        for original, effective, compiled in (("Windows", "WindowsStore", 0),
                                               ("WindowsStore", "Windows", 1)):
            with self.subTest(original=original, effective=effective):
                self.check_resolved_system_override_identity(hook, original, effective, compiled)

    def test_old_effective_system_baseline_fails_ordinary_identity_assertions(self):
        path = ROOT / "conan/hooks/hook_bgfx_wasm_fix.py"
        source = path.read_text(encoding="utf-8")
        wire = 'default_compiled = int(os_name != "WindowsStore" and build_type == "Debug")'
        self.assertEqual(source.count(wire), 1)
        namespace = {}
        exec(compile(source.replace(wire,
             'default_compiled = int(system != "WindowsStore" and build_type == "Debug")'),
             str(path), "exec"), namespace)
        for original, effective, compiled in (("Windows", "WindowsStore", 0),
                                               ("WindowsStore", "Windows", 1)):
            with self.assertRaises(AssertionError):
                self.check_resolved_system_override_identity(SimpleNamespace(**namespace),
                                                             original, effective, compiled)

    def test_auto_preserves_typed_option_and_unknown_is_not_off(self):
        for raw in (True, 1, "ON", {"value": "ON", "cache": True, "type": "BOOL", "force": True}):
            hook, owner = self.fixture(option=raw)
            self.check_selection(hook, owner, "auto", 1, 1)
        for raw in (False, 0, "OFF", "thing-NOTFOUND"):
            hook, owner = self.fixture(option=raw)
            self.check_selection(hook, owner, "auto", 0, 0)
        hook, owner = self.fixture(option="$<CONFIG:Debug>")
        self.check_selection(hook, owner, "auto", -1, -1)
        hook, owner = self.fixture(config=None)
        self.check_selection(hook, owner, "auto", 0, -1)
        for key, value in (("CMAKE_BUILD_TYPE", "Debug"), ("CMAKE_SYSTEM_NAME", "$"+"{CUSTOM_SYSTEM}"),
                           ("CMAKE_CONFIGURATION_TYPES", "Release;Debug")):
            hook, owner = self.fixture()
            owner.conf.values["tools.cmake.cmaketoolchain:extra_variables"][key] = value
            self.check_selection(hook, owner, "auto", 0, -1)
        hook, owner = self.fixture("on")
        owner.conf.define("tools.cmake.cmaketoolchain:user_toolchain", ["caller.cmake"])
        self.check_selection(hook, owner, "on", 1, -1)

    def test_unknown_request_inputs_partition_identity_and_canonical_order_is_stable(self):
        for kind, left, right in (
                ("system", "$"+"{SYSTEM_A}", "$"+"{SYSTEM_B}"),
                ("config", "Debug", "MinSizeRel"),
                ("toolchain", ["one.cmake"], ["two.cmake"]),
                ("flags", "$"+"{FLAGS_A}", "$"+"{FLAGS_B}"),
                ("option", "$"+"{OPTION_A}", "$"+"{OPTION_B}")):
            identities = []
            for value in (left, right):
                hook, owner = self.fixture()
                variables = owner.conf.values["tools.cmake.cmaketoolchain:extra_variables"]
                if kind == "toolchain":
                    owner.conf.define("tools.cmake.cmaketoolchain:user_toolchain", value)
                else:
                    key = {"system": "CMAKE_SYSTEM_NAME", "config": "CMAKE_BUILD_TYPE",
                           "flags": "CMAKE_CXX_FLAGS", "option": "BGFX_CONFIG_DEBUG_ANNOTATION"}[kind]
                    variables[key] = value
                hook.post_package_id(owner)
                first = dict(owner.info.conf.values)
                digest = first.get("user.jce:bgfx_debug_annotation_inputs")
                self.assertRegex(digest or "", r"^[0-9a-f]{64}$")
                # Canonical JSON must ignore dict insertion order, with no
                # external toolchain reads or mutation of request values.
                owner.conf.values["tools.cmake.cmaketoolchain:extra_variables"] = dict(reversed(list(variables.items())))
                hook.post_package_id(owner)
                self.assertEqual(owner.info.conf.values, first)
                identities.append(digest)
            self.assertNotEqual(*identities, kind)
        for value in (["x.cmake"] * 65, ["x" * 8193]):
            hook, owner = self.fixture()
            owner.conf.define("tools.cmake.cmaketoolchain:user_toolchain", value)
            with self.assertRaisesRegex(ValueError, "annotation unknown-input fingerprint"):
                hook.post_package_id(owner)

    def test_invalid_selector_and_reserved_conflicts_refuse_all_hook_entries(self):
        for request in ("", "1", "true", "debug", "on;off"):
            hook, owner = self.fixture(request)
            for entry in (hook.post_package_id, hook.pre_generate, hook.post_package_info):
                with self.assertRaisesRegex(ValueError, "bgfx_debug_annotation"):
                    entry(owner)
        for request, config, raw in (("on", "Release", "OFF"), ("off", "Debug", "ON"),
                                     ("on", "Release", "$"+"{CALLER_OPTION}")):
            hook, owner = self.fixture(request, config, option=raw)
            with self.assertRaisesRegex(ValueError, "BGFX_CONFIG_DEBUG_ANNOTATION"):
                hook.post_package_id(owner)
        for config, flags in (("Release", ["-DBGFX_CONFIG_DEBUG_ANNOTATION=1"]),
                              ("Debug", ["/DBGFX_CONFIG_DEBUG_ANNOTATION=0"]),
                              ("Debug", ["-U", "BGFX_CONFIG_DEBUG_ANNOTATION"]),
                              ("Debug", ["-D", "BGFX_CONFIG_DEBUG_ANNOTATION"]),
                              ("Debug", ["-DBGFX_CONFIG_DEBUG_ANNOTATION=2"])):
            hook, owner = self.fixture("off", config)
            owner.conf.values["tools.build:cxxflags"].extend(flags)
            with self.assertRaisesRegex(ValueError, "BGFX_CONFIG_DEBUG_ANNOTATION"):
                hook.pre_generate(owner)
        for key in ("CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_DEBUG"):
            hook, owner = self.fixture("off", "Debug")
            owner.conf.values["tools.cmake.cmaketoolchain:extra_variables"][key] = {
                "value": "-DBGFX_CONFIG_DEBUG_ANNOTATION=0"}
            with self.assertRaisesRegex(ValueError, "BGFX_CONFIG_DEBUG_ANNOTATION"):
                hook.post_package_id(owner)
        hook, owner = self.fixture("off", "Debug")
        owner.conf.values["tools.cmake.cmaketoolchain:extra_variables"]["CMAKE_CXX_FLAGS_RELEASE"] = "-DBGFX_CONFIG_DEBUG_ANNOTATION=0"
        self.check_selection(hook, owner, "off", 0, 1)
        hook, owner = self.fixture("off", "Debug")
        owner.conf.values["tools.build:defines"].append("BGFX_CONFIG_DEBUG_ANNOTATION=0")
        with self.assertRaisesRegex(ValueError, "BGFX_CONFIG_DEBUG_ANNOTATION"):
            hook.post_package_info(owner)
        hook, owner = self.fixture("off", "Debug")
        owner.conf.values["tools.build:cxxflags"].extend(
            ["/D", "BGFX_CONFIG_DEBUG_ANNOTATION=1", "-DNOT_BGFX_CONFIG_DEBUG_ANNOTATION=0",
             "-DBGFX_CONFIG_DEBUG_ANNOTATION_EXTRA=0", '-I"C:\\BGFX_CONFIG_DEBUG_ANNOTATION\\include"'])
        self.check_selection(hook, owner, "off", 0, 1)

    def test_missing_owner_wires_and_old_debug_rule_fail_ordinary_assertions(self):
        path = ROOT / "conan/hooks/hook_bgfx_wasm_fix.py"
        source = path.read_text(encoding="utf-8")
        for wire, replacement, request, config, option, compiled in (
                ('conanfile.info.conf.define(_ANNOTATION_CONFIG_CONF, _ANNOTATION_CONFIG)', 'pass', "on", "Release", 1, 1),
                ('_ANNOTATION_OPTION_CONF,', '"user.jce:omitted_annotation_option",', "on", "Release", 1, 1),
                ('conanfile.info.conf.define(_ANNOTATION_COMPILED_CONF, str(annotation[2]))', 'pass', "on", "Release", 1, 1),
                ('variables["BGFX_CONFIG_DEBUG_ANNOTATION"] = annotation[1]', 'pass', "on", "Release", 1, 1),
                ('"JCE_BGFX_DEBUG_ANNOTATION_COMPILED=%d" % annotation[2],', '', "on", "Release", 1, 1),
                ('compiled = 1 if build_type == "Debug" or option == 1 else option', 'compiled = option', "off", "Debug", 0, 1),
                ('if annotation[2] < 0 or annotation[2] != annotation[4]:', 'if True:', "off", "Release", 0, 0)):
            with self.subTest(wire=wire):
                self.assertEqual(source.count(wire), 1)
                namespace = {}
                exec(compile(source.replace(wire, replacement), str(path), "exec"), namespace)
                _, owner = self.fixture(request, config)
                with self.assertRaises(AssertionError):
                    self.check_selection(SimpleNamespace(**namespace), owner, request, option, compiled)




class BgfxConanLifecycleTests(unittest.TestCase):
    """Use Conan's real objects and forbidden-attribute lifecycle boundary."""
    @classmethod
    def setUpClass(cls):
        try:
            from conan import ConanFile
            from conan.errors import ConanException
            from conan.internal.errors import conanfile_remove_attr
            from conan.internal.model.settings import Settings
            from conan.internal.model.info import ConanInfo
            from conan.internal.model.conf import Conf as ConanConf
        except ModuleNotFoundError:
            raise unittest.SkipTest("installed Conan lifecycle subject unavailable")
        cls.ConanFile, cls.Settings, cls.ConanInfo, cls.ConanConf = ConanFile, Settings, ConanInfo, ConanConf
        cls.ConanException = ConanException
        cls.remove_attr = staticmethod(conanfile_remove_attr)

    def check_lifecycle(self, hook, request, config, system, override_key, override, compiled):
        owner = self.ConanFile("annotation-lifecycle-fixture")
        owner.name = "bgfx"
        owner.settings = self.Settings({"os": ["Windows", "WindowsStore"],
                                        "build_type": ["Release", "Debug"]})
        owner.settings.update_values([("os", system), ("build_type", config)])
        actual_settings = owner.settings
        owner.conf = self.ConanConf()
        owner.conf.define("user.jce:bgfx_debug_annotation", request)
        if override_key == "extra":
            owner.conf.define("tools.cmake.cmaketoolchain:extra_variables",
                              {"CMAKE_SYSTEM_NAME": override})
        elif override_key == "conf":
            owner.conf.define("tools.cmake.cmaketoolchain:system_name", override)
        owner.info = self.ConanInfo(settings=owner.settings.copy_conaninfo_settings(),
            options=owner.options.copy_conaninfo_options(), conf=self.ConanConf())
        owner.cpp_info = SimpleNamespace(defines=[], components={"bgfx": SimpleNamespace(defines=[])})
        # This is the installed Conan guard used for the actual
        # post_package_id hook, not a fabricated AttributeError or dict port.
        with self.remove_attr(owner, ["cpp_info", "settings", "options"], "package_id"):
            with self.assertRaisesRegex(self.ConanException, "self.settings.*forbidden"):
                _ = owner.settings
            try:
                hook.post_package_id(owner)
            except self.ConanException as error:
                self.fail("actual Conan lifecycle refused package_id settings access: " + str(error))
        self.assertIs(owner.settings, actual_settings)
        default = int(system != "WindowsStore" and config == "Debug")
        if compiled == default:
            self.assertIsNone(owner.info.conf.get("user.jce:bgfx_debug_annotation_config"))
        else:
            self.assertEqual(owner.info.conf.get("user.jce:bgfx_debug_annotation_compiled"), str(compiled))
            self.assertEqual(owner.info.conf.get("user.jce:bgfx_debug_annotation_config"), "upstream-cmake-v1")
        identity_before = owner.info.conf.dumps()
        hook.pre_generate(owner)
        hook.post_package_info(owner)
        for target in (owner.cpp_info, *owner.cpp_info.components.values()):
            for name, value in (("REQUEST", -1 if request == "auto" else int(request == "on")),
                                ("OPTION", int(request == "on")), ("COMPILED", compiled)):
                self.assertIn(f"JCE_BGFX_DEBUG_ANNOTATION_{name}={value}", target.defines)
        self.assertEqual(owner.info.conf.dumps(), identity_before,
                         "later legal stages changed the package identity")

    def test_actual_conan_forbidden_boundary_and_legal_later_stages(self):
        hook, _ = ConanConfigurationTests.bgfx_fixture()
        for request, config, system, key, override, compiled in (
                ("auto", "Release", "Windows", None, None, 0),
                ("on", "Release", "Windows", None, None, 1),
                ("off", "Debug", "Windows", None, None, 1),
                ("auto", "Debug", "Windows", "extra", "WindowsStore", 0),
                ("auto", "Debug", "WindowsStore", "conf", "Windows", 1)):
            with self.subTest(request=request, config=config, system=system, key=key):
                self.check_lifecycle(hook, request, config, system, key, override, compiled)

    def test_old_live_settings_read_fails_ordinary_actual_lifecycle_assertion(self):
        path = ROOT / "conan/hooks/hook_bgfx_wasm_fix.py"
        source = path.read_text(encoding="utf-8")
        wire = "annotation = _annotation_configuration(conanfile, conanfile.info.settings)"
        self.assertEqual(source.count(wire), 1)
        namespace = {}
        exec(compile(source.replace(wire,
            "annotation = _annotation_configuration(conanfile, conanfile.settings)"),
            str(path), "exec"), namespace)
        with self.assertRaisesRegex(AssertionError, "actual Conan lifecycle refused"):
            self.check_lifecycle(SimpleNamespace(**namespace), "on", "Release", "Windows",
                                 None, None, 1)


class BgfxAnnotationCMakeConfigurationTests(unittest.TestCase):
    """Actual Conan variable rendering and pinned upstream expression, no compiler."""
    @classmethod
    def setUpClass(cls):
        selected = os.environ.get("JCE_TEST_BGFX_CMAKE_SOURCE")
        if not selected:
            raise unittest.SkipTest("actual bgfx CMakeLists input not selected")
        path = Path(selected).parent / "cmake/bgfx/bgfx.cmake"
        text = path.read_text(encoding="utf-8")
        match = re.search(r'"(BGFX_CONFIG_DEBUG_ANNOTATION=\$<AND:[^\n"]+)"', text)
        if not match:
            raise AssertionError("actual upstream annotation expression changed")
        cls.expression = match.group(1)
        if not shutil.which("cmake") or not shutil.which("ninja"):
            raise unittest.SkipTest("CMake/Ninja configuration subject unavailable")
        try:
            from conan.tools.cmake.toolchain.blocks import ExtraVariablesBlock
        except ModuleNotFoundError:
            raise unittest.SkipTest("installed Conan ExtraVariablesBlock unavailable")
        cls.variables_block = ExtraVariablesBlock

    def check_configuration(self, config, system, request, expression=None, option=None, system_override=None):
        hook, owner = BgfxAnnotationConfigurationTests.fixture(request, config, system, option)
        if system_override is not None:
            owner.conf.values["tools.cmake.cmaketoolchain:extra_variables"]["CMAKE_SYSTEM_NAME"] = system_override
        hook.pre_generate(owner); hook.post_package_info(owner)
        rendered = self.variables_block(owner, SimpleNamespace(), "extra_variables").get_rendered_content()
        parent = ROOT / "build/tests/bgfx-annotation-configuration"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as temporary:
            folder = Path(temporary)
            # Only the bgfx target's actual public generator expression is
            # extracted; Conan owns rendering the unmodified caller option.
            (folder / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(BgfxAnnotation NONE)\n'
                + 'set(CMAKE_SYSTEM_NAME "' + system + '")\n' + rendered
                + '\noption(BGFX_CONFIG_DEBUG_ANNOTATION "upstream default" OFF)\n'
                + 'add_library(bgfx INTERFACE)\n'
                + 'target_compile_definitions(bgfx INTERFACE "' + (expression or self.expression) + '")\n'
                + 'file(GENERATE OUTPUT "' + folder.as_posix() + '/annotation.txt" CONTENT "$<TARGET_PROPERTY:bgfx,INTERFACE_COMPILE_DEFINITIONS>")\n',
                encoding="utf-8", newline="\n")
            completed = subprocess.run(["cmake", "-S", str(folder), "-B", str(folder / "configured"),
                "-G", "Ninja", "-DCMAKE_BUILD_TYPE=" + config],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            actual = (folder / "annotation.txt").read_text().strip()
            metadata = next(value for value in owner.cpp_info.defines
                            if value.startswith("JCE_BGFX_DEBUG_ANNOTATION_COMPILED=")).split("=")[1]
            self.assertEqual(actual, "BGFX_CONFIG_DEBUG_ANNOTATION=" + metadata,
                             "compiled annotation metadata disagrees with actual upstream expression")

    def test_actual_upstream_expression_matches_hook_and_real_conan_variables(self):
        for system in ("Windows", "WindowsStore"):
            for config in ("Release", "Debug"):
                for request in ("auto", "off", "on"):
                    with self.subTest(system=system, config=config, request=request):
                        self.check_configuration(config, system, request)
        self.check_configuration("Release", "Windows", "auto",
            option={"value": "ON", "cache": True, "type": "BOOL", "force": True})

    def test_actual_upstream_metadata_uses_resolved_bidirectional_system_override(self):
        self.check_configuration("Debug", "Windows", "auto", system_override="WindowsStore")
        self.check_configuration("Debug", "WindowsStore", "auto", system_override="Windows")

    def test_removing_debug_forcing_fails_ordinary_actual_expression_assertion(self):
        self.assertEqual(self.expression.count("$<CONFIG:Debug>"), 1)
        with self.assertRaisesRegex(AssertionError, "compiled annotation metadata disagrees"):
            self.check_configuration("Debug", "Windows", "off",
                                     self.expression.replace("$<CONFIG:Debug>", "0"))


class BgfxAppleProfilerConfigurationTests(unittest.TestCase):
    """Explicit upstream-body/installed-Conan check; no dependency compilation."""
    @classmethod
    def setUpClass(cls):
        selected = os.environ.get("JCE_TEST_BGFX_CMAKE_SOURCE")
        if not selected:
            raise unittest.SkipTest("actual bgfx CMakeLists input not selected")
        path = Path(selected)
        if not path.is_file():
            raise AssertionError("selected actual bgfx CMakeLists is missing")
        text = path.read_text(encoding="utf-8")
        match = re.search(
            r'(?m)^if\(APPLE AND NOT XCODE\)\s*\n[ \t]*set\(CMAKE_CXX_FLAGS "-ObjC\+\+"\)\s*\nendif\(\)', text)
        if not match:
            raise AssertionError("selected actual recipe no longer has the reviewed Apple flag-reset body")
        cls.apple_body = match.group(0)
        if not shutil.which("cmake") or not shutil.which("ninja"):
            raise unittest.SkipTest("CMake/Ninja configuration subject unavailable")
        try:
            from conan.tools.cmake.toolchain.blocks import ExtraFlagsBlock, CMakeFlagsInitBlock
        except ModuleNotFoundError:
            raise unittest.SkipTest("installed Conan CMakeToolchain subject unavailable")
        cls.extra_flags = ExtraFlagsBlock
        cls.flags_init = CMakeFlagsInitBlock

    def check_apple_directory_definition(self, hook, expected):
        _, owner = ConanConfigurationTests.bgfx_fixture("on" if expected else "off", "Macos")
        owner.settings = SimpleNamespace(os="Macos", get_safe=lambda key: "Release" if key == "build_type" else None)
        toolchain = SimpleNamespace(generator="Ninja", extra_cxxflags=[], extra_cflags=[],
                                    extra_sharedlinkflags=[], extra_exelinkflags=[])
        hook.pre_generate(owner)
        rendered = self.extra_flags(owner, toolchain, "extra_flags").get_rendered_content()
        rendered += self.flags_init(owner, toolchain, "cmake_flags_init").get_rendered_content()
        parent = ROOT / "build/tests/bgfx-apple-profiler-configuration"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as temporary:
            root = Path(temporary)
            (root / "child").mkdir()
            (root / "child/CMakeLists.txt").write_text(
                'get_directory_property(_child_defs COMPILE_DEFINITIONS)\n'
                'file(WRITE "${CMAKE_BINARY_DIR}/child-defines.txt" "${_child_defs}")\n', encoding="utf-8")
            # LANGUAGES NONE never configures or invokes a C/C++ compiler.
            # CMAKE_CXX_FLAGS initialization is explicit for that no-language
            # port; both generated flag blocks and the Apple if-body are real.
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.20)\nproject(BgfxFlags NONE)\n'
                + rendered + '\nset(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS_INIT}")\n'
                'set(APPLE TRUE)\nset(XCODE FALSE)\n' + self.apple_body + '\n'
                'get_directory_property(_defs COMPILE_DEFINITIONS)\n'
                'file(WRITE "${CMAKE_BINARY_DIR}/defines.txt" "${_defs}")\n'
                'file(WRITE "${CMAKE_BINARY_DIR}/flags.txt" "${CMAKE_CXX_FLAGS}")\n'
                'add_subdirectory(child)\n', encoding="utf-8")
            build = root / "configured"
            result = subprocess.run(["cmake", "-S", str(root), "-B", str(build), "-G", "Ninja"],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((build / "flags.txt").read_text(), "-ObjC++")
            for name in ("defines.txt", "child-defines.txt"):
                definitions = (build / name).read_text().split(";")
                self.assertIn("CALLER_DEFINE=1", definitions)
                self.assertIn(f"BGFX_CONFIG_PROFILER={expected}", definitions,
                              "required bgfx directory macro absent")

    def test_actual_apple_flag_reset_keeps_directory_definition(self):
        hook, _ = ConanConfigurationTests.bgfx_fixture()
        for expected in (0, 1):
            with self.subTest(expected=expected):
                self.check_apple_directory_definition(hook, expected)

    def test_old_cxxflags_only_owner_fails_ordinary_directory_assertion(self):
        path = ROOT / "conan/hooks/hook_bgfx_wasm_fix.py"
        source = path.read_text(encoding="utf-8")
        wire = "defines.append(profiler_definition)"
        self.assertEqual(source.count(wire), 1)
        namespace = {}
        exec(compile(source.replace(wire, "pass"), str(path), "exec"), namespace)
        with self.assertRaisesRegex(AssertionError, "required bgfx directory macro absent"):
            self.check_apple_directory_definition(SimpleNamespace(**namespace), 1)


class PhysFsWindowsFloorTests(unittest.TestCase):
    @staticmethod
    def fixture(name="physfs", system="Windows"):
        path = ROOT / "conan/hooks/hook_cmake_policy_fix.py"
        spec = importlib.util.spec_from_file_location("physfs_floor_hook", path)
        hook = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(hook)
        settings = SimpleNamespace(get_safe=lambda key: system if key == "os" else None)
        owner = SimpleNamespace(name=name, settings=settings, conf=Conf(),
                                info=SimpleNamespace(settings=settings, conf=Conf()))
        return hook, owner

    def check_floor(self, hook, owner):
        self.assertTrue(callable(getattr(hook, "post_package_id", None)),
                        "PhysFS binary identity hook missing")
        hook.post_package_id(owner)
        self.assertEqual(owner.info.conf.get("user.jce:physfs_win32_floor"), "0x0601")
        self.assertEqual(owner.info.conf.get("user.jce:physfs_win32_floor_config"),
                         "directory-flags-v1")
        before = {key: list(owner.conf.get(key, default=[])) for key in
                  ("tools.build:cflags", "tools.build:cxxflags", "tools.build:defines")}
        hook.pre_generate(owner)
        hook.pre_generate(owner)
        for key, prefix in (("tools.build:cflags", "-D"),
                            ("tools.build:cxxflags", "-D"), ("tools.build:defines", "")):
            values = owner.conf.get(key)
            for original in before[key]:
                self.assertIn(original, values)
            for name in ("_WIN32_WINNT", "WINVER"):
                self.assertEqual(values.count(prefix + name + "=0x0601"), 1)
        self.assertEqual(owner.conf.get("tools.cmake.cmaketoolchain:extra_variables")[
            "CMAKE_POLICY_VERSION_MINIMUM"], "3.5")

    def test_physfs_floor_owns_identity_and_actual_compile_channels(self):
        hook, owner = self.fixture()
        owner.conf.define("tools.build:cflags", ["-O2", "-DUSER_C=3"])
        self.check_floor(hook, owner)
        try:
            from conan.tools.cmake.toolchain.blocks import ExtraFlagsBlock
        except ModuleNotFoundError:
            raise unittest.SkipTest("installed Conan flag block unavailable")
        toolchain = SimpleNamespace(generator="Ninja", extra_cxxflags=[], extra_cflags=[],
                                    extra_asmflags=[], extra_sharedlinkflags=[], extra_exelinkflags=[])
        rendered = ExtraFlagsBlock(owner, toolchain, "extra_flags").get_rendered_content()
        self.assertIn('add_compile_definitions(', rendered)
        self.assertIn('_WIN32_WINNT=0x0601', rendered)
        self.assertIn('WINVER=0x0601', rendered)
        self.assertIn('-DUSER_C=3', rendered)

    def test_other_dependencies_platforms_and_store_keep_identity_and_flags(self):
        for name, system, override in (("bgfx", "Windows", None),
                ("physfs", "WindowsStore", None), ("physfs", "Linux", None),
                ("physfs", "Emscripten", None), ("physfs", "Macos", None),
                ("physfs", "Windows", "WindowsStore")):
            with self.subTest(name=name, system=system, override=override):
                hook, owner = self.fixture(name, system)
                if override:
                    owner.conf.define("tools.cmake.cmaketoolchain:extra_variables",
                                      {"CMAKE_SYSTEM_NAME": {"value": override, "type": "STRING"}})
                old_info = dict(owner.info.conf.values)
                old_flags = {key: list(owner.conf.get(key, default=[])) for key in
                             ("tools.build:cflags", "tools.build:cxxflags", "tools.build:defines")}
                self.assertTrue(callable(getattr(hook, "post_package_id", None)))
                hook.post_package_id(owner)
                hook.pre_generate(owner)
                self.assertEqual(owner.info.conf.values, old_info)
                for key, value in old_flags.items():
                    self.assertEqual(owner.conf.get(key, default=[]), value)

    def test_conflicting_reserved_macros_refuse_before_identity_or_generation(self):
        for key, value in (("tools.build:cflags", ["/D_WIN32_WINNT=0x0A00"]),
                ("tools.build:cxxflags", ["-U", "WINVER"]),
                ("tools.build:defines", ["WINVER=0x0602"]),
                ("tools.build:cflags", ['-D', '"_WIN32_WINNT=0x0602"']),
                ("tools.cmake.cmaketoolchain:extra_variables", {"CMAKE_C_FLAGS": "-DWINVER=0x0A00"}),
                ("tools.cmake.cmaketoolchain:extra_variables", {"CMAKE_CXX_FLAGS_RELEASE": {"value": "/U_WIN32_WINNT"}})):
            for entry in ("post_package_id", "pre_generate"):
                with self.subTest(key=key, value=value, entry=entry):
                    hook, owner = self.fixture()
                    owner.conf.define(key, value)
                    self.assertTrue(callable(getattr(hook, entry, None)))
                    with self.assertRaisesRegex(ValueError, "PhysFS Windows floor"):
                        getattr(hook, entry)(owner)

    def test_exact_macro_boundaries_preserve_unrelated_inputs(self):
        hook, owner = self.fixture()
        flags = ["-DNOT_WINVER=9", "-D_WIN32_WINNT_EXTRA=9", "/UWINVER_EXTRA",
                 '-I"include/_WIN32_WINNT/name"']
        owner.conf.define("tools.build:cflags", flags[:])
        self.check_floor(hook, owner)
        for original in flags:
            self.assertIn(original, owner.conf.get("tools.build:cflags"))

    def test_matching_native_defines_are_preserved_without_extra_flag_copies(self):
        hook, owner = self.fixture()
        flags = ["/D", "_WIN32_WINNT=0x0601", "/DWINVER=0x0601", "-O2"]
        owner.conf.define("tools.build:cflags", flags[:])
        hook.post_package_id(owner)
        hook.pre_generate(owner)
        hook.pre_generate(owner)
        self.assertEqual(owner.conf.get("tools.build:cflags"), flags)
        self.assertEqual(owner.conf.get("tools.build:defines").count("WINVER=0x0601"), 1)

    def test_missing_real_owner_wires_fail_ordinary_assertions(self):
        path = ROOT / "conan/hooks/hook_cmake_policy_fix.py"
        source = path.read_text(encoding="utf-8")
        for wire in ('conanfile.info.conf.define(_PHYSFS_FLOOR_CONF, floor)',
                     'conanfile.info.conf.define(_PHYSFS_FLOOR_CONFIG_CONF, config)',
                     'flags.append(definition)', 'defines.append(definition)'):
            with self.subTest(wire=wire):
                self.assertEqual(source.count(wire), 1, "one actual production owner wire required")
                namespace = {}
                exec(compile(source.replace(wire, "pass"), str(path), "exec"), namespace)
                _, owner = self.fixture()
                with self.assertRaises(AssertionError):
                    self.check_floor(SimpleNamespace(**namespace), owner)

    def test_actual_conan_package_id_guard_and_binary_identity_partition(self):
        try:
            from conan import ConanFile
            from conan.internal.model.conf import Conf
            from conan.internal.model.info import ConanInfo, RequirementsInfo
            from conan.internal.model.settings import Settings
            from conan.internal.errors import conanfile_remove_attr
        except ModuleNotFoundError:
            raise unittest.SkipTest("installed Conan lifecycle unavailable")
        hook, _ = self.fixture()
        owner = ConanFile("physfs-floor-lifecycle")
        owner.name = "physfs"
        owner.settings = Settings({"os": ["Windows", "WindowsStore"],
                                   "build_type": ["Release", "Debug"]})
        owner.settings.update_values([("os", "Windows"), ("build_type", "Release")])
        owner.conf = Conf()
        owner.info = ConanInfo(settings=owner.settings.copy_conaninfo_settings(),
            options=owner.options.copy_conaninfo_options(), reqs_info=RequirementsInfo({}),
            build_requires_info=RequirementsInfo({}), conf=Conf())
        old_id = owner.info.package_id()
        self.assertTrue(callable(getattr(hook, "post_package_id", None)))
        with conanfile_remove_attr(owner, ["cpp_info", "settings", "options"], "package_id"):
            hook.post_package_id(owner)
        self.assertNotEqual(owner.info.package_id(), old_id,
                            "unqualified cached PhysFS package was not partitioned")
        identity = owner.info.conf.dumps()
        hook.pre_generate(owner)
        self.assertEqual(owner.info.conf.dumps(), identity)


if __name__ == "__main__": unittest.main()
