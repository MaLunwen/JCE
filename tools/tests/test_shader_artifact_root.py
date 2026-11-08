"""Real CMake stock-root selection controls; no packer, compiler or GPU."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cmake"), "CMake subject unavailable")
class ShaderArtifactRootControls(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(dir=ROOT / "build")
        self.addCleanup(temporary.cleanup)
        self.cache = Path(temporary.name)
        source = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        begin = source.index("# JCE shader stock artifact root: begin")
        end = source.index("# JCE shader stock artifact root: end", begin)
        self.block = source[begin:end]
        self.catalog = ROOT / "engine/shaders/stock_profiles.json"
        self.digest = hashlib.sha256(self.catalog.read_bytes()).hexdigest()

    def select(self, compatibility):
        observation = self.cache / "observation.txt"
        driver = self.cache / "select.cmake"
        driver.write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            + f'set(CMAKE_CURRENT_SOURCE_DIR "{ROOT.as_posix()}")\n'
            + f'set(CMAKE_CURRENT_BINARY_DIR "{self.cache.as_posix()}")\n'
            + f'set(JCE_SHADER_COMPATIBILITY {"ON" if compatibility else "OFF"})\n'
            # Poison prior cached metadata; actual owner must force its value.
            + 'set(JCE_SHADER_OUTPUT_DIR "old/stale" CACHE INTERNAL "old path")\n'
            + self.block + '\n'
            + f'file(WRITE "{observation.as_posix()}" "${{JCE_ENGINE_ASSET_BUILD_DIR}}\\n'
              '${JCE_SHADER_OUTPUT_DIR}\\n${JCE_SHADER_STOCK_POLICY_NAME}\\n'
              '${JCE_SHADER_STOCK_POLICY_SHA256}\\n")\n',
            encoding="utf-8", newline="\n")
        result = subprocess.run([shutil.which("cmake"), "-P", str(driver)],
                                capture_output=True, text=True, timeout=30,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return observation.read_text().splitlines()

    def test_compatibility_root_is_sibling_bound_to_exact_catalogue(self):
        legacy = self.cache / "generated/engine_assets/shaders/legacy_glsl.bin"
        legacy.parent.mkdir(parents=True)
        legacy.write_bytes(b"retained full stock")
        asset_root, shaders, profile, digest = self.select(True)
        expected = self.cache / ("generated/engine_assets-compat-" + self.digest[:32])
        self.assertEqual(Path(asset_root), expected)
        self.assertEqual(Path(shaders), expected / "shaders")
        self.assertEqual(profile, "compatibility")
        self.assertEqual(digest, self.digest)
        self.assertFalse(legacy.is_relative_to(Path(asset_root)))
        self.assertEqual(legacy.read_bytes(), b"retained full stock")

    def test_switching_back_preserves_full_path_and_excludes_retained_compat_root(self):
        selected = self.select(True)
        old = Path(selected[1]) / "old_compat_glsl.bin"
        old.parent.mkdir(parents=True)
        old.write_bytes(b"retained compatibility stock")
        full = self.select(False)
        self.assertEqual(Path(full[0]), self.cache / "generated/engine_assets")
        self.assertEqual(Path(full[1]), self.cache / "generated/engine_assets/shaders")
        self.assertEqual(full[2], "full")
        self.assertEqual(full[3:], [""])
        self.assertFalse(old.is_relative_to(Path(full[0])))
        self.assertEqual(old.read_bytes(), b"retained compatibility stock")


if __name__ == "__main__":
    unittest.main()
