"""Temporary-filesystem controls; no shaderc, Conan, CMake or VS Code runs."""
from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import tempfile
import unittest


_OWNER = Path(__file__).resolve().parents[1] / "build" / "ide_modkit.py"
_SPEC = importlib.util.spec_from_file_location("jce_ide_modkit_controls", _OWNER)
modkit = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(modkit)


class ModkitSettingsControls(unittest.TestCase):
    def setUp(self):
        scratch = _OWNER.parents[2] / "build" / "tests" / "ide-modkit"
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve() / "source with spaces (test)"
        self.build = self.root / "build" / "desktop" / "windows-x64" / "obj" / "release"
        self.generators = self.root / "build" / "desktop" / "windows-x64" / "conan" / "release" / "generators"
        self.package = Path(self.temp.name).resolve() / "conan package (test)"
        self.include = self.package / "include" / "bgfx"
        self.compiler = self.package / "bin" / ("shaderc.exe" if os.name == "nt" else "shaderc")
        self.toolchain = self.generators / "conan_toolchain.cmake"
        for directory in (self.build, self.generators, self.include, self.compiler.parent,
                          self.root / "engine" / "shaders" / "include"):
            directory.mkdir(parents=True, exist_ok=True)
        self.toolchain.write_text("# synthetic configured toolchain marker\n")
        (self.include / "bgfx_shader.sh").write_text("// synthetic presence marker, not a vendor header\n")
        self.compiler.write_text("not executed by the read-only resolver\n")
        self.data = self.generators / "bgfx-release-x86_64-data.cmake"
        self.data.write_text(f'set(bgfx_PACKAGE_FOLDER_RELEASE "{self.package.as_posix()}")\n')
        self.config = self.generators / "bgfx-config.cmake"
        self.config.write_text('set(BGFX_SHADER_INCLUDE_PATH "${bgfx_PACKAGE_FOLDER_RELEASE}/include/bgfx")\n')
        self.values = {
            "CMAKE_HOME_DIRECTORY:INTERNAL": str(self.root),
            "CMAKE_BUILD_TYPE:STRING": "Release",
            "CMAKE_TOOLCHAIN_FILE:FILEPATH": str(self.toolchain),
            "bgfx_DIR:PATH": str(self.generators),
            "JCE_SHADERC_EXECUTABLE:FILEPATH": str(self.compiler),
        }
        self.write_cache()

    def write_cache(self):
        (self.build / "CMakeCache.txt").write_text(
            "# synthetic configured cache\n" + "\n".join(f"{k}={v}" for k, v in self.values.items()) + "\n")

    def resolve(self):
        return modkit.resolve_settings(self.root, self.build)

    def test_exact_selected_build_and_quoted_spaces_preserve_inputs(self):
        before = {p: p.read_bytes() for p in Path(self.temp.name).rglob("*") if p.is_file()}
        result = self.resolve()
        self.assertEqual(result, {
            "modkit.shaderc.path": self.compiler.as_posix(),
            "modkit.shaderc.includeDirs": [self.include.as_posix(),
                (self.root / "engine" / "shaders").as_posix(),
                (self.root / "engine" / "shaders" / "include").as_posix()],
            "modkit.shaderc.useBuildDir": True,
            "modkit.shaderc.buildDir": self.build.as_posix(),
        })
        self.assertEqual(before, {p: p.read_bytes() for p in Path(self.temp.name).rglob("*") if p.is_file()})

    def test_explicit_compiler_overrides_package_candidate(self):
        compiler = self.root / "approved host shaderc"
        compiler.write_text("not executed\n")
        self.values["JCE_SHADERC_EXECUTABLE:FILEPATH"] = str(compiler)
        self.write_cache()
        self.assertEqual(self.resolve()["modkit.shaderc.path"], compiler.as_posix())

    def test_missing_explicit_compiler_does_not_fall_back(self):
        self.values["JCE_SHADERC_EXECUTABLE:FILEPATH"] = str(self.root / "absent compiler")
        self.write_cache()
        with self.assertRaisesRegex(ValueError, "JCE_SHADERC_EXECUTABLE"):
            self.resolve()

    def test_package_fallback_is_selected_configuration_only(self):
        self.values.pop("JCE_SHADERC_EXECUTABLE:FILEPATH")
        self.write_cache()
        (self.generators / "bgfx-debug-x86_64-data.cmake").write_text('set(bgfx_PACKAGE_FOLDER_DEBUG "/unrelated/missing")\n')
        self.assertEqual(self.resolve()["modkit.shaderc.path"], self.compiler.as_posix())

    def test_explicit_include_and_compiler_do_not_require_package_guess(self):
        self.data.unlink()
        self.config.write_text("# no extra variables\n")
        self.values["BGFX_SHADER_INCLUDE_PATH:PATH"] = str(self.include)
        self.write_cache()
        self.assertEqual(self.resolve()["modkit.shaderc.includeDirs"][0], self.include.as_posix())

    def test_missing_package_metadata_cannot_search_unrelated_cache(self):
        self.data.unlink()
        self.config.write_text("# no extra variables\n")
        # The usable package and header still exist; they are no longer referenced.
        with self.assertRaisesRegex(ValueError, "no verified bgfx_shader.sh"):
            self.resolve()

    def test_foreign_source_build_refuses(self):
        self.values["CMAKE_HOME_DIRECTORY:INTERNAL"] = str(self.package)
        self.write_cache()
        with self.assertRaisesRegex(ValueError, "another source root"):
            self.resolve()

    def test_foreign_cmakedeps_directory_refuses(self):
        self.values["bgfx_DIR:PATH"] = str(self.package)
        self.write_cache()
        with self.assertRaisesRegex(ValueError, "generator directory"):
            self.resolve()

    def test_ambiguous_current_configuration_packages_refuse(self):
        second = Path(self.temp.name) / "second package"
        second.mkdir()
        (self.generators / "bgfx-release-x86-data.cmake").write_text(
            f'set(bgfx_PACKAGE_FOLDER_RELEASE "{second.as_posix()}")\n')
        with self.assertRaisesRegex(ValueError, "Ambiguous bgfx_PACKAGE_FOLDER_RELEASE"):
            self.resolve()

    def test_disagreeing_explicit_and_generated_includes_refuse(self):
        self.values["BGFX_SHADER_INCLUDE_PATH:PATH"] = str(self.root)
        self.write_cache()
        with self.assertRaisesRegex(ValueError, "Ambiguous BGFX_SHADER_INCLUDE_PATH"):
            self.resolve()

    def test_missing_header_refuses(self):
        (self.include / "bgfx_shader.sh").unlink()
        with self.assertRaisesRegex(ValueError, "bgfx_shader.sh"):
            self.resolve()

    def test_unresolved_generated_path_refuses(self):
        self.config.write_text('set(BGFX_SHADER_INCLUDE_PATH "${UNKNOWN_PACKAGE}/include/bgfx")\n')
        with self.assertRaisesRegex(ValueError, "Unresolved CMake path"):
            self.resolve()

    def test_non_scalar_generated_path_refuses(self):
        self.config.write_text('set(BGFX_SHADER_INCLUDE_PATH "a" "b")\n')
        with self.assertRaisesRegex(ValueError, "non-scalar"):
            self.resolve()

    def test_missing_configuration_and_duplicate_cache_refuse(self):
        self.values["CMAKE_BUILD_TYPE:STRING"] = ""
        self.write_cache()
        with self.assertRaisesRegex(ValueError, "CMAKE_BUILD_TYPE"):
            self.resolve()
        self.values["CMAKE_BUILD_TYPE:STRING"] = "Release"
        self.write_cache()
        with (self.build / "CMakeCache.txt").open("a") as stream:
            stream.write("CMAKE_BUILD_TYPE:STRING=Release\n")
        with self.assertRaisesRegex(ValueError, "Duplicate CMAKE_BUILD_TYPE"):
            self.resolve()

    def test_oversized_metadata_refuses(self):
        self.config.write_text("#" * (1024 * 1024 + 1))
        with self.assertRaisesRegex(ValueError, "exceeds 1 MiB"):
            self.resolve()

    def test_missing_project_include_directory_is_not_invented(self):
        (self.root / "engine" / "shaders" / "include").rmdir()
        (self.root / "engine" / "shaders").rmdir()
        self.assertEqual(self.resolve()["modkit.shaderc.includeDirs"], [self.include.as_posix()])


if __name__ == "__main__":
    unittest.main()
