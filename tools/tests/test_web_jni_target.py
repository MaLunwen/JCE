"""Exercise target-aware JNI probing with an available but incompatible host JDK."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

class WebJNITargetTests(unittest.TestCase):
    def test_web_never_probes_host_jdk(self):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("CMake unavailable")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            headers = root / "host-jdk/include/win32"
            headers.mkdir(parents=True)
            (headers.parent / "jni.h").write_text("#error host JNI must never compile on Web")
            (headers / "jni_md.h").write_text("#error incompatible host platform")
            source = ROOT / "scripting/java"
            (root / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(WebJNIProbe LANGUAGES NONE)
set(EMSCRIPTEN ON)
add_library(jce_script_api INTERFACE)
add_subdirectory("{source.as_posix()}" java)
if(TARGET jce_script_java OR TARGET jce_script_vm_java)
  message(FATAL_ERROR "Web acquired host JNI targets")
endif()
if(DEFINED JCE_JNI_INCLUDE_DIR OR DEFINED JCE_JNI_MD_INCLUDE_DIR)
  message(FATAL_ERROR "Web probed host JNI headers")
endif()
''')
            env = dict(os.environ, JAVA_HOME=str(headers.parent.parent))
            result = subprocess.run([cmake, "-S", str(root), "-B", str(root / "out")],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("native JVM unavailable on Web", result.stdout)
