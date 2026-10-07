"""Prove configuration hooks preserve upstream input and caller options."""
import ast
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
class Conf:
    def __init__(self): self.values = {"tools.cmake.cmaketoolchain:extra_variables": {"CALLER_SETTING": 42}, "tools.build:cxxflags": ["-DCALLER_FLAG=1"]}
    def get(self, key, default=None, check_type=None): return self.values.get(key, default)
    def define(self, key, value): self.values[key] = value
class ConanConfigurationTests(unittest.TestCase):
    def test_every_hook_leaves_dependency_files_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "CMakeLists.txt"
            source.write_bytes(b"cmake_minimum_required(VERSION 2.8)\n# upstream\n")
            before = source.read_bytes()
            c = SimpleNamespace(name="bgfx", settings=SimpleNamespace(os="Windows"), conf=Conf(), source_folder=str(source.parent))
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
        namespace["generate"](SimpleNamespace(settings=SimpleNamespace(os="Windows")))
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

if __name__ == "__main__": unittest.main()
