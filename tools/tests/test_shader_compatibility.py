"""Configure/command controls; no shader compilation, renderer or GPU execution."""
import ast
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ConanShaderSelectionControls(unittest.TestCase):
    def selection(self, tier):
        source = ast.parse((ROOT / "conanfile.py").read_text(encoding="utf-8"))
        owner = next(node for node in source.body if isinstance(node, ast.ClassDef))
        method = next(node for node in owner.body if isinstance(node, ast.FunctionDef) and node.name == "generate")
        snapshots = []

        class Deps:
            def __init__(self, unused):
                pass
            def set_property(self, *args, **kwargs):
                pass
            def generate(self):
                pass

        class Toolchain:
            def __init__(self, owner):
                self.owner = owner
            def generate(self):
                snapshots.append(dict(self.owner.conf.values["tools.cmake.cmaketoolchain:extra_variables"]))

        class Conf:
            def __init__(self):
                self.values = {"tools.cmake.cmaketoolchain:extra_variables": {"CALLER_OPTION": "retained"}}
                if tier is not None:
                    self.values["user.jce:bgfx_graphics_tier"] = tier
            def get(self, key, default=None, **kwargs):
                return self.values.get(key, default)
            def define(self, key, value):
                self.values[key] = value

        namespace = {"CMakeDeps": Deps, "CMakeToolchain": Toolchain}
        exec(compile(ast.Module(body=[method], type_ignores=[]), "actual-conan-generator", "exec"), namespace)
        conf = Conf()
        namespace["generate"](SimpleNamespace(settings=SimpleNamespace(os="Windows"), dependencies={}, conf=conf))
        return snapshots[0]

    def test_generated_selection_tracks_actual_requested_tier_and_resets_default(self):
        for tier in (None, "stable", "modern", "current", "compatibility", " COMPATIBILITY "):
            with self.subTest(tier=tier):
                selection = self.selection(tier)
                expected = tier is not None and tier.strip().lower() == "compatibility"
                self.assertEqual(selection.get("JCE_SHADER_COMPATIBILITY"), {
                    "value": "ON" if expected else "OFF", "cache": True, "type": "BOOL", "force": True})
                self.assertEqual(selection["CALLER_OPTION"], "retained")

    @unittest.skipUnless(shutil.which("cmake"), "CMake cache subject unavailable")
    def test_real_conan_emission_resets_an_existing_cmake_cache_both_directions(self):
        try:
            from conan.tools.cmake.utils import parse_extra_variable
        except ImportError:
            self.skipTest("Conan CMake emitter unavailable")
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            root = Path(temporary)
            for tier, previous, expected in (("compatibility", "OFF", "ON"), ("stable", "ON", "OFF")):
                selected = self.selection(tier)["JCE_SHADER_COMPATIBILITY"]
                emitted = parse_extra_variable("tools.cmake.cmaketoolchain:extra_variables",
                                               "JCE_SHADER_COMPATIBILITY", selected)
                observation = root / "observed.txt"
                script = root / "cache.cmake"
                script.write_text(
                    f'set(JCE_SHADER_COMPATIBILITY {previous} CACHE BOOL "old selection")\n'
                    + f'set(JCE_SHADER_COMPATIBILITY {emitted})\n'
                    + f'file(WRITE "{observation.as_posix()}" "${{JCE_SHADER_COMPATIBILITY}}")\n',
                    encoding="utf-8", newline="\n")
                result = subprocess.run([shutil.which("cmake"), "-P", str(script)],
                                        capture_output=True, text=True, timeout=30,
                                        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(observation.read_text(), expected)



@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake/Ninja configure subject unavailable")
class ShaderCommandSelectionControls(unittest.TestCase):
    def configure(self, compatibility, stock=True):
        temporary = tempfile.TemporaryDirectory(dir=ROOT / "build")
        self.addCleanup(temporary.cleanup)
        base = Path(temporary.name)
        source = base / "source"
        source.mkdir()
        for name in ("vs_mesh.sc", "fs_pbr.sc", "fs_pbr_noshadow.sc",
                     "fs_composite.sc", "fs_sky.sc", "vs_foliage.sc", "vs_foliage_shadow.sc",
                     "vs_water.sc", "fs_water.sc", "vs_particle_expanded.sc",
                     "vs_water_physical.sc", "fs_water_physical.sc", "fs_sky_physical_compat.sc",
                     "vs_foliage_cards.sc", "vs_foliage_cards_shadow.sc",
                     "cs_fixture.sc", "varying.def.sc"):
            (source / name).write_text("// command-only fixture\n", encoding="ascii")
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\nproject(ShaderControl LANGUAGES NONE)\n"
            + f'set(BGFX_SHADER_INCLUDE_PATH "{source.as_posix()}")\n'
            + f'set(JCE_SHADER_COMPATIBILITY {compatibility})\n'
            + 'set(JCE_SHADER_PROFILES "dx11;spv;glsl;essl;mtl")\n'
            + f'include("{(ROOT / "tools/compile_shaders.cmake").as_posix()}")\n'
            + ('jce_compile_shaders(STOCK ' if stock else 'jce_compile_shaders(')
            + 'TARGET Control SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}" '
              'OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")\n', encoding="utf-8", newline="\n")
        environment = dict(os.environ, JCE_SHADERC_EXECUTABLE=sys.executable)
        environment.pop("BGFX_SHADERC", None)
        result = subprocess.run([shutil.which("cmake"), "-S", str(source), "-B", str(base / "out"),
                                 "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + shutil.which("ninja")],
                                env=environment, capture_output=True, text=True, timeout=45,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return (base / "out/build.ninja").read_text(encoding="utf-8")

    @staticmethod
    def commands(generated):
        return [line for line in generated.splitlines() if line.strip().startswith("COMMAND =") and " --type " in line]

    def test_compatibility_define_is_limited_to_gl_graphics_commands(self):
        commands = self.commands(self.configure("ON"))
        self.assertTrue(commands)
        covered = set()
        for command in commands:
            graphics = " --type vertex " in command or " --type fragment " in command
            gl = "_glsl.bin" in command or "_essl.bin" in command
            expected = graphics and gl
            self.assertEqual("JCE_SHADER_COMPATIBILITY=1" in command, expected, command)
            self.assertEqual(command.count(" --define "), 1)
            if expected:
                self.assertIn("BGFX_CONFIG_MAX_BONES=128;JCE_SHADER_COMPATIBILITY=1", command)
            if expected:
                covered.add("glsl" if "_glsl.bin" in command else "essl")
        self.assertEqual(covered, {"glsl", "essl"})

    def test_reduced_shaders_are_limited_to_desktop_gl21_stock_fragments(self):
        for compatibility, stock in (("ON", True), ("ON", False), ("OFF", True)):
            with self.subTest(compatibility=compatibility, stock=stock):
                commands = self.commands(self.configure(compatibility, stock))
                selected = [line for line in commands if "JCE_PBR_GL21_STOCK=1" in line]
                expected = 2 if compatibility == "ON" and stock else 0
                self.assertEqual(len(selected), expected)
                composite = [line for line in commands if "JCE_POSTFX_GL21_STOCK=1" in line]
                self.assertEqual(len(composite), 1 if expected else 0)
                sky = [line for line in commands if "JCE_SKY_GL21_STOCK=1" in line]
                self.assertEqual(len(sky), 1 if expected else 0)
                if expected:
                    self.assertIn("fs_sky_glsl.bin", sky[0])
                    self.assertIn("fs_composite_glsl.bin", composite[0])
                    for name in ("fs_pbr", "fs_pbr_noshadow"):
                        self.assertEqual(sum(f"{name}_glsl.bin" in line for line in selected), 1)

    def test_new_legacy_stock_families_cook_only_for_desktop_gl(self):
        commands = self.commands(self.configure("ON"))
        for name in ("vs_water", "fs_water", "vs_foliage",
                     "vs_foliage_shadow", "vs_particle_expanded",
                     "vs_foliage_cards", "vs_foliage_cards_shadow",
                     "vs_water_physical", "fs_water_physical", "fs_sky_physical_compat"):
            self.assertEqual(sum(f"{name}_glsl.bin" in line for line in commands), 1)
            self.assertFalse(any(f"{name}_essl" in line for line in commands))

    def test_other_tiers_keep_existing_command_defines(self):
        commands = self.commands(self.configure("OFF"))
        self.assertTrue(commands)
        for command in commands:
            self.assertNotIn("JCE_SHADER_COMPATIBILITY", command)
            self.assertIn("BGFX_CONFIG_MAX_BONES=128", command)


if __name__ == "__main__":
    unittest.main()
