"""CMake-executed About Platform identity controls (no native C/C++ build)."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("cmake"), "cmake unavailable")
class XPAboutPlatformV441(unittest.TestCase):
    def test_actual_cmake_generator_preserves_modern_platform_and_marks_xp(self):
        source = (ROOT / "editor/CMakeLists.txt").read_text(encoding="utf-8")
        start = source.index("# Resolve a human-readable OS + arch string for the About dialog.")
        end = source.index("target_compile_definitions(JCE_Editor PRIVATE", start)
        logic = source[start:end]
        self.assertIn('set(_jce_os_str "Windows XP+")', logic)
        self.assertIn('set(_jce_platform_str "${_jce_os_str} ${_jce_arch_str} (${_jce_bits_str})")', logic)
        self.assertIn('JCE_PLATFORM_STR="${_jce_platform_str}"', source[end:end+200])
        panel = (ROOT / "editor/src/ui/jce_editor_panels.cpp").read_text(encoding="utf-8")
        self.assertIn('build_row("Platform", JCE_PLATFORM_STR);', panel)
        cases = (
            ("Windows", True, True, "x86_64", 8, "Windows XP+ x64 (64-bit)"),
            ("Windows", True, True, "i686", 4, "Windows XP+ x86 (32-bit)"),
            ("Windows", True, False, "x86_64", 8, "Windows x64 (64-bit)"),
            ("Linux", False, False, "x86_64", 8, "Linux x64 (64-bit)"),
        )
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "about-platform.cmake"
            for sysname, win, xp, cpu, width, expected in cases:
                with self.subTest(expected=expected):
                    setup = (f"set(CMAKE_SYSTEM_NAME {sysname})\n"
                             f"set(CMAKE_SYSTEM_PROCESSOR {cpu})\n"
                             f"set(CMAKE_SIZEOF_VOID_P {width})\n"
                             f"set(JCE_PLATFORM_WINDOWS {'TRUE' if win else 'FALSE'})\n"
                             f"set(JCE_PLATFORM_LINUX {'FALSE' if win else 'TRUE'})\n"
                             f"set(JCE_WINXP_LEGACY {'ON' if xp else 'OFF'})\n")
                    script.write_text(setup + logic + '\nmessage(STATUS "ABOUT_RESULT=${_jce_platform_str}")\n', encoding="utf-8")
                    proc = subprocess.run(["cmake", "-P", str(script)],
                                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                          text=True, encoding="utf-8", errors="replace")
                    self.assertEqual(proc.returncode, 0, proc.stdout)
                    self.assertIn("ABOUT_RESULT=" + expected, proc.stdout)


if __name__ == "__main__":
    unittest.main()
