"""Prove configuration hooks preserve upstream input and caller options."""
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
if __name__ == "__main__": unittest.main()
