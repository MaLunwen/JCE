"""Exercise the real XP Vulkan preflight against a mock loader and driver."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == "nt", "Windows host fixture")
class XpVulkanProbeTests(unittest.TestCase):
    def test_admission_and_cleanup(self):
        toolchain = os.environ.get("JCE_XP_TOOLCHAIN_ROOT")
        compiler = (Path(toolchain) / "bin/x86_64-w64-mingw32-clang++.exe" if toolchain
                    else shutil.which("x86_64-w64-mingw32-clang++.exe"))
        if not compiler or not Path(compiler).is_file():
            self.skipTest("set JCE_XP_TOOLCHAIN_ROOT to the Windows XP toolchain")
        headers = os.environ.get("JCE_XP_BGFX_VULKAN_HEADERS")
        if not headers or not (Path(headers) / "vulkan-local/vulkan.h").is_file():
            self.skipTest("set JCE_XP_BGFX_VULKAN_HEADERS to the pinned bgfx 3rdparty/khronos directory")

        output = ROOT / "build/tests/xp-vulkan-probe"
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as temporary:
            exe = Path(temporary) / "xp-vulkan-probe.exe"
            fixture = ROOT / "tools/tests/fixtures/xp_vulkan_probe.cpp"
            self.run_checked([str(compiler), "-std=c++20", "-msse2", "-static",
                              "-D_WIN32_WINNT=0x0502", "-DWINVER=0x0502",
                              "-I", str(Path(headers).resolve()), str(fixture), "-o", str(exe)])
            self.run_checked([str(exe)])

    def run_checked(self, argv):
        result = subprocess.run(argv, capture_output=True, text=True, timeout=60,
                                creationflags=subprocess.CREATE_NO_WINDOW, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
