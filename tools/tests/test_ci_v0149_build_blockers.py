"""CPU-only controls for v0.14.9 CI #61 compiler/build-output regressions.

These exercise the real C boot wire using a host C99 compiler when available,
and mock only the .NET invocation/output location. They do not claim full JCE
builds, runtime C# loading, or native GUI execution.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
from types import SimpleNamespace
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def _managed_driver():
    path = ROOT / "scripting/csharp/build_csharp.py"
    spec = importlib.util.spec_from_file_location("jce_ci61_managed", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ManagedOutputDirectoryControls(unittest.TestCase):
    def _case(self, provide_runtimeconfig):
        module = _managed_driver()
        with tempfile.TemporaryDirectory(prefix="jce-ci61-dotnet-") as tmp:
            base = Path(tmp)
            project_dir = base / "managed/JceScript"
            project_dir.mkdir(parents=True)
            project = project_dir / "JceScript.csproj"
            project.write_text('<Project Sdk="Microsoft.NET.Sdk"/>\n', encoding="utf-8")
            outputs = base / "managed/artifacts"
            outputs.mkdir(parents=True)
            (outputs / "JceScript.dll").write_bytes(b"prior DLL")
            (outputs / "JceScript.runtimeconfig.json").write_bytes(b"prior runtimeconfig")
            seen = []

            def fake_dotnet(argv, cwd):
                self.assertEqual(argv[:3], ["dotnet", "build", str(project)])
                self.assertEqual(argv[argv.index("-c") + 1], "Release")
                self.assertIn("--output", argv)
                directory = Path(argv[argv.index("--output") + 1])
                self.assertTrue(directory.is_relative_to(project_dir / "obj"))
                self.assertEqual(Path(cwd), base)
                seen.append(directory)
                directory.mkdir(parents=True, exist_ok=True)
                (directory / "JceScript.dll").write_bytes(b"fresh DLL")
                if provide_runtimeconfig:
                    (directory / "JceScript.runtimeconfig.json").write_bytes(b"fresh runtimeconfig")
                return SimpleNamespace(returncode=0)

            with patch.object(module, "HERE", base), \
                 patch.object(module, "PROJECT", project), \
                 patch.object(module, "ARTIFACTS", outputs), \
                 patch.object(module, "have_dotnet", return_value=True), \
                 patch.object(module.subprocess, "run", side_effect=fake_dotnet):
                code = module.build("Release")
            self.assertEqual(len(seen), 1)
            self.assertFalse(seen[0].exists(), "temporary dotnet outputs must be cleaned")
            if provide_runtimeconfig:
                self.assertEqual(code, 0)
                self.assertEqual((outputs / "JceScript.dll").read_bytes(), b"fresh DLL")
                self.assertEqual((outputs / "JceScript.runtimeconfig.json").read_bytes(), b"fresh runtimeconfig")
            else:
                self.assertNotEqual(code, 0)
                # A partially successful invocation must not bless an old DLL.
                self.assertEqual((outputs / "JceScript.dll").read_bytes(), b"prior DLL")
                self.assertEqual((outputs / "JceScript.runtimeconfig.json").read_bytes(), b"prior runtimeconfig")

    def test_explicit_dotnet_output_is_independent_of_msbuild_platform(self):
        self._case(provide_runtimeconfig=True)

    def test_missing_runtimeconfig_fails_without_partial_publication(self):
        self._case(provide_runtimeconfig=False)


class ResourceObservationWireControls(unittest.TestCase):
    BOOT = "    JceAsyncExecutor *boot_executor = jce_async_default_executor();"
    END = "    /* Force landscape on mobile"
    ERROR = "Structured async executor initialization failed"

    def _boot_wire(self):
        engine = (ROOT / "engine/src/application/jce_engine.c").read_text(encoding="utf-8")
        self.assertEqual(engine.count(self.BOOT), 1)
        self.assertEqual(engine.count(self.END), 1)
        start = engine.index(self.BOOT)
        stop = engine.index(self.END, start)
        fragment = engine[start:stop]
        self.assertIn('fatal_msg(allow_error_dialog, "' + self.ERROR + '")', fragment)
        return fragment

    def test_generated_fixture_preserves_real_engine_boot_wire_and_argument(self):
        cmake = (ROOT / "tests/application/CMakeLists.txt").read_text(encoding="utf-8")
        generated = ("static bool actual_resource_bind(struct JceEngine *e)\\n{\\n"
                     "    const bool allow_error_dialog = false;"
                     " /* headless fixture: never open an SDL modal */\\n"
                     "${_resource_observation_bind}")
        self.assertIn(generated, cmake)
        fixture = (ROOT / "tests/application/test_jce_engine_resource_observation.c").read_text(encoding="utf-8")
        self.assertIn("static void fatal_msg(bool allow_error_dialog, const char *format, ...)", fixture)
        self.assertIn("TEST_ASSERT_FALSE(allow_error_dialog);", fixture)
        self.assertIn("RUN_TEST(test_failed_async_boot_uses_nonmodal_fatal_path);", fixture)

    def _compile(self, *, omit_dialog=False):
        cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
        if cc is None:
            self.skipTest("native C99 compiler not available; source contract still checked")
        boot = self._boot_wire()
        declaration = "" if omit_dialog else "    const bool allow_error_dialog = false;\n"
        src = f'''#include <stdbool.h>
#include <stddef.h>
#include <string.h>
typedef struct JceAsyncExecutor {{int sentinel;}} JceAsyncExecutor;
struct JceEngine {{ int sentinel; }};
static int calls;
static JceAsyncExecutor *jce_async_default_executor(void) {{ return NULL; }}
static void fatal_msg(bool allowed, const char *fmt, ...) {{
    if (allowed) calls = -100;
    else if (strcmp(fmt, "{self.ERROR}") == 0) ++calls;
}}
static bool actual_resource_bind(struct JceEngine *e) {{
{declaration}    (void)e;
{boot}    return true;
fail: return false;
}}
int main(void) {{
    struct JceEngine engine = {{0}};
    if (actual_resource_bind(&engine)) return 1;
    return calls == 1 ? 0 : 2;
}}
'''
        with tempfile.TemporaryDirectory(prefix="jce-ci61-c99-") as temp:
            test_source = Path(temp) / "wire.c"
            executable = Path(temp) / "wire"
            test_source.write_text(src, encoding="utf-8")
            compile_result = subprocess.run(
                [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", str(test_source), "-o", str(executable)],
                capture_output=True, text=True)
            if not omit_dialog:
                self.assertEqual(compile_result.returncode, 0,
                                 compile_result.stdout + compile_result.stderr)
                ran = subprocess.run([str(executable)], capture_output=True, text=True)
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            else:
                self.assertNotEqual(compile_result.returncode, 0,
                                    "negative control must reject omitted C99 parameter")
                self.assertIn("allow_error_dialog", compile_result.stderr)

    def test_actual_boot_wire_compiles_and_exercises_fatal_branch_in_c99(self):
        self._compile()

    def test_omitting_dialog_value_is_a_failing_negative_control(self):
        self._compile(omit_dialog=True)


class CiExitCodeControls(unittest.TestCase):
    def test_csharp_and_java_builds_have_independent_failing_steps(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertIn("      - name: Build C# managed scripting artifacts\n"
                      "        run: python scripting/csharp/build_csharp.py", workflow)
        self.assertIn("      - name: Build Java managed scripting artifacts\n"
                      "        run: python scripting/java/build_java.py", workflow)
        self.assertNotIn("      - name: Build managed scripting artifacts\n        run: |", workflow)


if __name__ == "__main__":
    unittest.main()
