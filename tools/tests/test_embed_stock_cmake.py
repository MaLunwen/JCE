"""Actual configure/build controls for explicit native stock wrappers in cross builds."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
EXE = None
class EmbedStockTests(unittest.TestCase):
    def setUp(self):
        if EXE is None or not shutil.which("cmake"):
            self.skipTest("explicit native --exe and CMake required")
    def exercise(self, *, cross, explicit, target, missing=False):
        with tempfile.TemporaryDirectory(prefix="stock cross ", dir=ROOT / "build") as temporary:
            source = Path(temporary)
            output = source / "build"
            blob = source / "stock.bin"
            blob.write_bytes(bytes(range(256)))
            lines = ['cmake_minimum_required(VERSION 3.20)', 'project(stock_control NONE)',
                     'set(EMSCRIPTEN TRUE)', 'add_custom_target(owner)']
            if target:
                lines += ['add_executable(jce_bin2obj IMPORTED)',
                    'set_target_properties(jce_bin2obj PROPERTIES IMPORTED_LOCATION "' + EXE.as_posix() + '")']
            if explicit:
                wrapper = source / "missing.exe" if missing else EXE
                lines += ['set(JCE_BIN2OBJ_EXECUTABLE "' + wrapper.as_posix() + '")']
            lines += ['include("' + (ROOT / "cmake/JCEEmbedStock.cmake").as_posix() + '")',
                      'jce_target_embed_stock(owner "' + blob.as_posix() + '" test_stock)',
                      'add_custom_target(emit DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/generated/test_stock.c")']
            (source / "CMakeLists.txt").write_text("\n".join(lines) + "\n")
            command = ["cmake", "-S", str(source), "-B", str(output), "-G", "Ninja"]
            if cross:
                toolchain = source / "cross.cmake"
                toolchain.write_text('set(CMAKE_SYSTEM_NAME Generic)\n')
                command += ["-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain)]
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                return result.returncode, result.stderr, None
            built = subprocess.run(["cmake", "--build", str(output), "--target", "emit"],
                                   capture_output=True, text=True)
            generated = output / "generated/test_stock.c"
            return built.returncode, built.stderr, generated.read_text() if generated.exists() else None
    def test_cross_explicit_native_tool_generates_exact_payload(self):
        code, error, source = self.exercise(cross=True, explicit=True, target=False)
        self.assertEqual(code, 0, error)
        self.assertIsNotNone(source)
        self.assertEqual(bytes(int(n, 16) for n in re.findall(r"0x([0-9a-f]{2})", source)), bytes(range(256)))
    def test_cross_target_never_substitutes_for_missing_native_wrapper(self):
        for target in (False, True):
            code, error, source = self.exercise(cross=True, explicit=False, target=target)
            self.assertNotEqual(code, 0)
            self.assertIn("explicit native", error)
            self.assertIsNone(source)
    def test_invalid_explicit_path_never_falls_back_to_target(self):
        code, error, source = self.exercise(cross=False, explicit=True, target=True, missing=True)
        self.assertNotEqual(code, 0)
        self.assertIn("does not exist", error)
        self.assertIsNone(source)
    def test_native_target_fallback_remains_supported(self):
        code, error, source = self.exercise(cross=False, explicit=False, target=True)
        self.assertEqual(code, 0, error)
        self.assertIsNotNone(source)
if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    args, remaining = parser.parse_known_args()
    EXE = args.exe.resolve(strict=True)
    unittest.main(argv=[__file__] + remaining)
