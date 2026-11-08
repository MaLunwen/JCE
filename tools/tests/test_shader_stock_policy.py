"""Actual CMake configure/script controls; never compile shaders or run a renderer."""
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / "engine/shaders/stock_profiles.json"
HELPER = ROOT / "tools/shader_stock_policy.cmake"
COOK = json.loads(CATALOG.read_text(encoding="utf-8"))["profiles"]["compatibility"]["cook"]


@unittest.skipUnless(shutil.which("cmake"), "CMake script subject unavailable")
class StockPolicyIdentityControls(unittest.TestCase):
    def script(self, compatibility="ON", catalog=None, extra="", helper=HELPER):
        temporary = tempfile.TemporaryDirectory(dir=ROOT / "build")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        selection = ""
        if catalog is not None:
            if isinstance(catalog, (dict, list)):
                path = root / "catalog.json"
                path.write_text(json.dumps(catalog), encoding="utf-8", newline="\n")
            elif isinstance(catalog, tuple):
                path = root / "catalog.json"
                path.write_text(catalog[0], encoding="utf-8", newline="\n")
            else:
                path = Path(catalog)
            selection = 'set(JCE_SHADER_STOCK_CATALOG "' + path.as_posix() + '")\n'
        result_path = root / "identity.txt"
        script = root / "control.cmake"
        script.write_text('cmake_minimum_required(VERSION 3.24)\n'
            + 'set(JCE_SHADER_COMPATIBILITY ' + compatibility + ')\n' + selection
            + 'include("' + helper.as_posix() + '")\n'
            + 'jce_shader_stock_policy_identity(policy digest)\n'
            + extra + '\nfile(WRITE "' + result_path.as_posix() + '" "${policy}|${digest}")\n',
            encoding="utf-8", newline="\n")
        result = subprocess.run([shutil.which("cmake"), "-P", str(script)], capture_output=True,
            text=True, timeout=30, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return result, result_path

    def test_default_profile_and_digest_come_from_the_actual_catalogue(self):
        result, path = self.script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(path.read_text(), "compatibility|" + hashlib.sha256(CATALOG.read_bytes()).hexdigest())

    def test_full_profile_does_not_read_or_allowlist_a_catalogue(self):
        result, path = self.script("OFF", ROOT / "build/intentionally-absent-stock-policy.json",
            'jce_shader_stock_admit(admitted "vs_arbitrary_user_name" vertex glsl)\n'
            'if(NOT admitted)\n message(FATAL_ERROR "full stock was allowlisted")\nendif()')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(path.read_text(), "full|")

    def test_malformed_catalogues_refuse_instead_of_falling_back(self):
        original = json.loads(CATALOG.read_text(encoding="utf-8"))
        cases = [("invalid-json", ("{",))]
        for name, change in (
            ("schema", lambda value: value.update(schema=2)),
            ("authority", lambda value: value.update(source_authority="unrelated.json")),
            ("full-restricted", lambda value: value["profiles"]["full"]["cook"].update(unrestricted=False)),
            ("compat-unrestricted", lambda value: value["profiles"]["compatibility"]["cook"].update(unrestricted=True)),
            ("string-bool", lambda value: value["profiles"]["compatibility"]["cook"].update(unrestricted="false")),
            ("stage-not-array", lambda value: value["profiles"]["compatibility"]["cook"].update(vertex="vs_mesh")),
            ("duplicate-name", lambda value: value["profiles"]["compatibility"]["cook"]["vertex"].append("vs_mesh")),
            ("wrong-stage-prefix", lambda value: value["profiles"]["compatibility"]["cook"]["fragment"].append("vs_wrong")),
            ("compat-compute", lambda value: value["profiles"]["compatibility"]["cook"]["compute"].append("cs_unsupported")),
            ("full-list", lambda value: value["profiles"]["full"]["cook"]["vertex"].append("vs_custom")),
            ("missing-stage", lambda value: value["profiles"]["compatibility"]["cook"].pop("fragment")),
            ("extra-cook-key", lambda value: value["profiles"]["compatibility"]["cook"].update(unknown=[])),
        ):
            value = copy.deepcopy(original)
            change(value)
            cases.append((name, value))
        for name, value in cases:
            with self.subTest(name=name):
                result, path = self.script(catalog=value)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("Shader stock catalogue", result.stdout + result.stderr)
                self.assertFalse(path.exists())

    def test_catalogue_bytes_change_identity_even_when_the_cook_subset_is_equal(self):
        changed = json.loads(CATALOG.read_text(encoding="utf-8"))
        changed["scope"] += "; content identity control"
        result, path = self.script(catalog=changed)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotEqual(path.read_text(), "compatibility|" + hashlib.sha256(CATALOG.read_bytes()).hexdigest())

    def test_admission_rejects_unknown_stage_and_relative_catalogue_path(self):
        for catalog, extra in ((None, "jce_shader_stock_admit(admitted vs_mesh pixel glsl)"),
                               (Path("relative/catalog.json"), "")):
            with self.subTest(catalog=catalog):
                result, _ = self.script(catalog=catalog, extra=extra)
                self.assertNotEqual(result.returncode, 0)


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake/Ninja configure subject unavailable")
class StockCommandControls(unittest.TestCase):
    def configure(self, compatibility, names, *, stock=True, profiles="glsl;essl;essl1;spv;mtl;dx11", relocated=False):
        temporary = tempfile.TemporaryDirectory(dir=ROOT / "build")
        self.addCleanup(temporary.cleanup)
        base = Path(temporary.name)
        source = base / "source"
        source.mkdir()
        for name in (*names, "varying.def"):
            (source / (name + ".sc")).write_text("// Configure-only command subject; never compiled.\n", encoding="ascii")
        include = ROOT / "tools/compile_shaders.cmake"
        selection = ""
        if relocated:
            sdk = base / "relocated-sdk/share/jce"
            tools = sdk / "cmake"
            tools.mkdir(parents=True)
            for name in ("compile_shaders.cmake", "shader_stock_policy.cmake"):
                shutil.copyfile(ROOT / "tools" / name, tools / name)
            catalog = sdk / "shaders/stock_profiles.json"
            catalog.parent.mkdir()
            shutil.copyfile(CATALOG, catalog)
            include = tools / "compile_shaders.cmake"
            selection = 'set(JCE_SHADER_STOCK_CATALOG "' + catalog.as_posix() + '")\n'
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.24)\nproject(StockControl LANGUAGES NONE)\n'
            + f'set(BGFX_SHADER_INCLUDE_PATH "{source.as_posix()}")\n'
            + f'set(JCE_SHADER_COMPATIBILITY {compatibility})\nset(JCE_SHADER_PROFILES "{profiles}")\n'
            + selection + f'include("{include.as_posix()}")\n'
            + 'jce_compile_shaders(' + ('STOCK ' if stock else '')
            + 'TARGET Control SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders" OUT_FILES_VAR compiled)\n'
            + 'file(WRITE "${CMAKE_BINARY_DIR}/outputs.txt" "${compiled}")\n', encoding="utf-8", newline="\n")
        # Python --version is only the existing executable-admission port.
        # No generated custom command, shaderc, compiler or renderer is run.
        environment = dict(os.environ, JCE_SHADERC_EXECUTABLE=sys.executable)
        environment.pop("BGFX_SHADERC", None)
        result = subprocess.run([shutil.which("cmake"), "-S", str(source), "-B", str(base / "out"),
            "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + shutil.which("ninja")],
            env=environment, capture_output=True, text=True, timeout=45,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        outputs = (base / "out/outputs.txt").read_text().split(";")
        pairs = {tuple(Path(value).stem.rsplit("_", 1)) for value in outputs if value}
        ninja = (base / "out/build.ninja").read_text(encoding="utf-8")
        commands = [line for line in ninja.splitlines() if line.strip().startswith("COMMAND =") and " --type " in line]
        return pairs, commands

    def test_compatibility_stock_uses_exact_catalogue_and_preserves_non_gl_commands(self):
        allowed = set(COOK["vertex"] + COOK["fragment"])
        names = allowed | {"vs_pbr_inst", "vs_pbr_skinned", "vs_custom", "fs_custom", "cs_custom"}
        pairs, commands = self.configure("ON", sorted(names))
        desktop = {"vs_water", "fs_water", "vs_foliage", "fs_foliage",
                   "vs_foliage_shadow", "fs_foliage_shadow", "vs_foliage_cards",
                   "vs_foliage_cards_shadow", "vs_particle",
                   "vs_particle_expanded", "fs_particle", "vs_grass",
                   "vs_grass_expanded", "fs_grass", "fs_grass_shadow"}
        compatibility_only = {"fs_sky_physical_compat", "vs_water_physical", "fs_water_physical"}
        self.assertTrue((desktop | compatibility_only).issubset(allowed))
        for suffix in ("glsl", "essl", "essl1"):
            expected = allowed if suffix == "glsl" else allowed - desktop - compatibility_only
            self.assertEqual({stem for stem, actual in pairs if actual == suffix}, expected)
        for suffix in ("spv", "mtl", *(("dx11",) if os.name == "nt" else ())):
            self.assertEqual({stem for stem, actual in pairs if actual == suffix}, names - compatibility_only)
        for command in commands:
            self.assertEqual(command.count(" --define "), 1)
            self.assertIn("BGFX_CONFIG_MAX_BONES=128", command)

    def test_compatibility_custom_shaders_are_unrestricted_without_stock_opt_in(self):
        names = {"vs_custom", "fs_custom", "cs_custom"}
        pairs, commands = self.configure("ON", sorted(names), stock=False, profiles="glsl;essl;essl1")
        for suffix in ("glsl", "essl"):
            self.assertEqual({stem for stem, actual in pairs if actual == suffix}, names)
        self.assertEqual({stem for stem, suffix in pairs if suffix == "essl1"}, {"vs_custom", "fs_custom"})
        for command in commands:
            graphics = " --type compute " not in command
            self.assertEqual("JCE_SHADER_COMPATIBILITY=1" in command, graphics)
        stock_pairs, _ = self.configure("ON", sorted(names), profiles="glsl;essl;essl1")
        self.assertEqual(stock_pairs, set())

    def test_full_stock_retains_custom_names_compute_and_legacy_gles2_policy(self):
        names = {"vs_custom", "fs_custom", "cs_custom", "vs_mesh", "fs_mesh"}
        compatibility_only = {"fs_sky_physical_compat", "vs_water_physical", "fs_water_physical"}
        pairs, commands = self.configure("OFF", sorted(names | compatibility_only))
        self.assertFalse(any(stem in compatibility_only for stem, _ in pairs))
        for suffix in ("glsl", "essl", "spv", "mtl"):
            self.assertEqual({stem for stem, actual in pairs if actual == suffix}, names)
        self.assertEqual({stem for stem, suffix in pairs if suffix == "essl1"}, {"vs_mesh", "fs_mesh"})
        self.assertTrue(any(" -p 430 " in line and "cs_custom_glsl.bin" in line for line in commands))
        self.assertTrue(any(" -p 310_es " in line and "cs_custom_essl.bin" in line for line in commands))
        self.assertFalse(any("JCE_SHADER_COMPATIBILITY" in line for line in commands))

    def test_installed_relocated_helper_uses_the_explicit_catalogue_path(self):
        pairs, _ = self.configure("ON", ["vs_mesh", "fs_mesh", "vs_pbr_skinned", "cs_custom"],
                                  profiles="glsl;essl", relocated=True)
        self.assertEqual(pairs, {(stem, suffix) for stem in ("vs_mesh", "fs_mesh") for suffix in ("glsl", "essl")})


if __name__ == "__main__":
    unittest.main()
