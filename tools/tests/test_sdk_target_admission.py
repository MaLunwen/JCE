"""SDK metadata/schema and dispatch controls; no compiler, link or GPU proof."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "cmake/JCESDKTarget.cmake"
HELPERS = ROOT / "cmake/JCESDKHelpers.cmake"


def metadata(**changes):
    result = dict(schema=1, system="Windows", arch="x86_64", pointer_bits=64,
                  config="Release", variant="dist", delivery="embedded",
                  crt="MultiThreaded", archive="jce_engine_core.lib")
    result.update(changes)
    return result


@unittest.skipUnless(shutil.which("cmake"), "CMake unavailable")
class CMakeSdkTargetTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="jce-sdk-target-cmake-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.file = self.root / "arbitrary-name/lib/Release/jce-sdk-target.json"
        self.file.parent.mkdir(parents=True)
        self.archive = self.file.parent / "jce_engine_core.lib"
        self.archive.write_bytes(b"PRESENCE_ONLY_NOT_ABI")

    def run_script(self, text):
        script = self.root / "control.cmake"
        script.write_text(f'include("{MODULE.as_posix()}")\n' + text, encoding="utf-8")
        return subprocess.run(["cmake", "-P", str(script)], capture_output=True,
                              text=True, timeout=30)

    def admit(self, data=None, raw=None, arch="x86_64", config="Release", variant="dist"):
        self.file.write_text(raw if raw is not None else json.dumps(data or metadata()), encoding="utf-8")
        return self.run_script(f'jce_sdk_read_target("{self.file.as_posix()}" '
            f'Windows {arch} 64 {config} {variant} embedded crt TRUE)\n'
            'if(NOT crt STREQUAL "MultiThreaded")\nmessage(FATAL_ERROR "CRT result wrong")\nendif()\n')

    def test_exact_module_admits_real_file_after_directory_rename(self):
        self.assertEqual(self.admit().returncode, 0)
        moved = self.root / "moved-product"
        self.file.parents[2].rename(moved)
        self.file = moved / "lib/Release/jce-sdk-target.json"
        self.assertEqual(self.admit().returncode, 0)

    def test_same_pointer_width_does_not_admit_other_architecture(self):
        result = self.admit(arch="aarch64")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("target mismatch", result.stderr)

    def test_missing_archive_or_metadata_requires_refresh(self):
        self.archive.unlink()
        result = self.admit()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("archive missing", result.stderr)
        self.file.unlink()
        result = self.run_script(f'jce_sdk_read_target("{self.file.as_posix()}" Windows x86_64 64 Release dist embedded crt TRUE)')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("refresh", result.stderr)

    def test_config_schema_crt_duplicates_and_size_fail_closed(self):
        controls = (json.dumps(metadata(config="Debug")), json.dumps(metadata(schema=True)),
            json.dumps(metadata(crt="MultiThreadedDLL")), json.dumps(metadata(pointer_bits=32)),
            json.dumps(metadata(archive="../jce_engine_core.lib")),
            json.dumps(metadata())[:-1] + ',"schema":1}', json.dumps(metadata(extra=1)),
            json.dumps(metadata(arch="aarch64"))[:-1] + ',"ar\\u0063h":"x86_64"}',
            json.dumps(metadata()).replace('MultiThreaded', 'MultiThreaded\\u0000DLL'),
            "null", "{", " " * 16385)
        for raw in controls:
            with self.subTest(raw=raw[:90]):
                result = self.admit(raw=raw)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("refresh", result.stderr)

    def test_consumer_uses_actual_compiler_identity_and_exact_configuration(self):
        self.file.write_text(json.dumps(metadata()), encoding="utf-8")
        prefix = f'set(MSVC TRUE)\ninclude("{HELPERS.as_posix()}")\n' + \
            'set(CMAKE_SYSTEM_NAME Windows)\nset(CMAKE_SIZEOF_VOID_P 8)\n' + \
            'set(JCE_BUILD_VARIANT dist)\nset(JCE_DELIVERY_PROFILE embedded)\n'
        for arch, config, passed in (("x64", "Release", True), ("ARM64", "Release", False),
                                    ("x64", "Debug", False)):
            with self.subTest(arch=arch, config=config):
                result = self.run_script(prefix + f'set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID {arch})\n'
                    f'set(CMAKE_BUILD_TYPE {config})\n'
                    f'jce_sdk_admit_consumer("{self.file.parents[1].as_posix()}")\n')
                self.assertEqual(result.returncode == 0, passed, result.stdout + result.stderr)

    def test_consumer_crt_intent_is_checked_before_override(self):
        for value, passed in (("MultiThreaded", True), ("MultiThreadedDLL", False),
                ("MultiThreaded$<$<CONFIG:Debug>:Debug>", True),
                ("MultiThreaded$<$<CONFIG:Debug>:Debug>DLL", False)):
            result = self.run_script(f'set(CMAKE_MSVC_RUNTIME_LIBRARY "{value}")\n'
                'jce_sdk_check_consumer_crt(Release MultiThreaded)\n')
            self.assertEqual(result.returncode == 0, passed, result.stdout + result.stderr)
        result = self.run_script('set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")\n'
                                'jce_sdk_check_consumer_crt(Debug MultiThreadedDebug)\n')
        self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(shutil.which("ninja"), "Ninja unavailable for compiler-free generation fixture")
    def test_actual_generation_resolves_owner_crt_and_rejects_mixed_owners(self):
        # project(NONE): modeled CMake compiler identity only. No language is
        # enabled and no compiler runs; this tests serialization and CRT checks.
        source = self.root / "producer"
        (source / "cmake").mkdir(parents=True)
        shutil.copyfile(MODULE, source / "cmake/JCESDKTarget.cmake")
        (source / "CMakeLists.txt").write_text(f"""cmake_minimum_required(VERSION 3.20)
project(SdkMetadata NONE)
set(MSVC TRUE)
include("{HELPERS.as_posix()}")
include("{MODULE.as_posix()}")
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER_ARCHITECTURE_ID x64)
set(CMAKE_SIZEOF_VOID_P 8)
set(CMAKE_STATIC_LIBRARY_SUFFIX .lib)
set(JCE_BUILD_VARIANT dist)
set(JCE_DELIVERY_PROFILE embedded)
add_library(first INTERFACE)
add_library(second INTERFACE)
set_property(TARGET first PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_property(TARGET second PROPERTY MSVC_RUNTIME_LIBRARY "${{OTHER_CRT}}")
jce_sdk_generate_target_metadata("${{CMAKE_BINARY_DIR}}/metadata" "first;second" file check)
""", encoding="utf-8")
        for runtime, passed in (("MultiThreaded", True), ("MultiThreadedDLL", False)):
            build = source / runtime
            result = subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Release", "-DOTHER_CRT=" + runtime],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            data = json.loads((build / "metadata/Release/jce-sdk-target.json").read_text())
            self.assertEqual(data, metadata())
            check = subprocess.run(["cmake", "-P", str(build / "metadata/Release/jce-sdk-target-check.cmake")],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(check.returncode == 0, passed, check.stdout + check.stderr)
            if not passed: self.assertIn("mixed owner CRT", check.stderr)


    def test_consumer_windows_schema1_frontend_identity_model(self):
        # Script-mode identity models only: no cl, clang-cl or GNU compiler runs.
        self.file.write_text(json.dumps(metadata()), encoding="utf-8")
        for compiler, compatible in (("MSVC", True), ("Clang", True),
                                     ("GNU", False), ("Clang", False)):
            with self.subTest(compiler=compiler, msvc_compatible=compatible):
                result = self.run_script(f"""set(CMAKE_SYSTEM_NAME Windows)
set(MSVC {'TRUE' if compatible else 'FALSE'})
set(CMAKE_C_COMPILER_ID {compiler})
set(CMAKE_CXX_COMPILER_ID {compiler})
set(CMAKE_C_COMPILER_ARCHITECTURE_ID x64)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID x64)
set(CMAKE_SIZEOF_VOID_P 8)
set(CMAKE_BUILD_TYPE Release)
set(JCE_BUILD_VARIANT dist)
set(JCE_DELIVERY_PROFILE embedded)
jce_sdk_admit_consumer("{self.file.parents[1].as_posix()}")
if(NOT JCE_SDK_TARGET_SYSTEM STREQUAL "Windows" OR
   NOT JCE_SDK_TARGET_ARCH STREQUAL "x86_64" OR
   NOT JCE_MSVC_RUNTIME_LIBRARY STREQUAL "$<$<CONFIG:Release>:MultiThreaded>")
    message(FATAL_ERROR "Admitted consumer outputs do not match the SDK")
endif()
""")
                if compatible:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                else:
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("SDK_ABI_MISMATCH: consumer", result.stderr)
                    self.assertIn("MSVC-compatible", result.stderr)

    @unittest.skipUnless(shutil.which("ninja"), "Ninja unavailable for compiler-free generation fixture")
    def test_producer_windows_schema1_frontend_identity_model(self):
        # project(NONE) consumes the real producer, with interface archive owners.
        # The frontend state is modeled; generated files are not real ABI proof.
        source = self.root / "abi-producer"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(f"""cmake_minimum_required(VERSION 3.20)
project(SdkFrontendIdentity NONE)
include("{MODULE.as_posix()}")
set(CMAKE_SYSTEM_NAME Windows)
set(MSVC "${{MODEL_MSVC}}")
set(CMAKE_C_COMPILER_ID "${{MODEL_COMPILER}}")
set(CMAKE_CXX_COMPILER_ID "${{MODEL_COMPILER}}")
set(CMAKE_C_COMPILER_ARCHITECTURE_ID x64)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID x64)
set(CMAKE_SIZEOF_VOID_P 8)
set(CMAKE_STATIC_LIBRARY_SUFFIX .lib)
set(JCE_BUILD_VARIANT dist)
set(JCE_DELIVERY_PROFILE embedded)
add_library(owner INTERFACE)
set_property(TARGET owner PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
jce_sdk_generate_target_metadata("${{CMAKE_BINARY_DIR}}/metadata" owner file check)
""", encoding="utf-8")
        for index, (compiler, compatible) in enumerate((("MSVC", True), ("Clang", True),
                                                        ("GNU", False), ("Clang", False))):
            with self.subTest(compiler=compiler, msvc_compatible=compatible):
                build = self.root / f"abi-producer-{index}"
                result = subprocess.run(["cmake", "-S", str(source), "-B", str(build),
                    "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DMODEL_COMPILER={compiler}",
                    f"-DMODEL_MSVC={'TRUE' if compatible else 'FALSE'}"],
                    capture_output=True, text=True, timeout=30)
                generated = build / "metadata/Release/jce-sdk-target.json"
                if compatible:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(json.loads(generated.read_text()), metadata())
                else:
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("SDK_ABI_MISMATCH: producer", result.stderr)
                    self.assertFalse(generated.exists(), "Incompatible producer published SDK metadata")

    def test_schema1_frontend_guard_does_not_restrict_non_windows_models(self):
        for system in ("Linux", "Darwin", "Android", "iOS", "Emscripten"):
            with self.subTest(system=system):
                result = self.run_script(f'set(CMAKE_SYSTEM_NAME {system})\n'
                    'set(MSVC FALSE)\nset(CMAKE_C_COMPILER_ID GNU)\n'
                    '_jce_sdk_require_schema1_compiler_abi(producer)\n'
                    '_jce_sdk_require_schema1_compiler_abi(consumer)\n')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class SdkDispatchTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="jce-sdk-target-dispatch-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.project = self.root / "project"
        self.project.mkdir()
        (self.project / "jce_project.json").write_text('{"name":"Probe"}')
        self.sdk = self.root / "renamed-sdk"
        (self.sdk / "lib/cmake/JCE").mkdir(parents=True)
        (self.sdk / "lib/cmake/JCE/JCEConfig.cmake").write_text("fixture config")
        (self.sdk / "lib/Release").mkdir()
        (self.sdk / "lib/Release/jce-sdk-target.json").write_text(json.dumps(metadata(arch="aarch64")))
        (self.sdk / "lib/Release/jce_engine_core.lib").write_bytes(b"PRESENCE_ONLY")
        spec = importlib.util.spec_from_file_location("jce_sdk_dispatch_control", ROOT / "tools/build/jce.py")
        self.driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.driver)
        self.target = dict(host="windows", arch="x86_64", key="windows-x64", bits=64, cross=False)
        self.args = SimpleNamespace(project=str(self.project), sdk=str(self.sdk), arch="x64", variant="dist",
            delivery_profile="embedded", clean=True, configure_only=False, registry_proof=None,
            target="Probe", exe=None, name=None, version=None)

    def test_application_mismatch_cannot_clean_or_dispatch_tools(self):
        marker = self.project / "build/windows-x86_64-dist/keep.dat"
        marker.parent.mkdir(parents=True); marker.write_bytes(b"KEEP")
        with patch.object(self.driver, "HOST", "windows"), \
                patch.object(self.driver, "resolve_target", return_value=self.target), \
                patch.object(self.driver, "run") as run, \
                patch.object(self.driver, "_confirmed_app_sdk_tools") as tools, \
                patch.object(self.driver.shutil, "rmtree") as remove:
            with self.assertRaises(SystemExit): self.driver.cmd_app.__wrapped__(self.args)
            run.assert_not_called(); tools.assert_not_called(); remove.assert_not_called()
            self.assertEqual(marker.read_bytes(), b"KEEP")

    def test_package_mismatch_is_rejected_before_cooking(self):
        with patch.object(self.driver, "resolve_target", return_value=self.target), \
                patch.object(self.driver, "cmd_cook") as cook, \
                patch.object(self.driver, "cmd_app") as app:
            with self.assertRaises(SystemExit): self.driver.cmd_package_game(self.args)
            cook.assert_not_called(); app.assert_not_called()

    def test_current_metadata_admits_after_renaming_and_old_host_stamp_is_ignored(self):
        (self.sdk / "lib/Release/jce-sdk-target.json").write_text(json.dumps(metadata()))
        (self.sdk / "VERSION.txt").write_text("variant: release\nhost: alien\n")
        self.assertEqual(self.driver.resolve_sdk(self.args, self.target), self.sdk)
        moved = self.root / "another-sdk-name"
        self.sdk.rename(moved); self.args.sdk = str(moved)
        self.assertEqual(self.driver.resolve_sdk(self.args, self.target), moved)

    def test_build_project_mismatch_preserves_prior_receipt(self):
        marker = self.project / "build/prior-result.json"
        marker.parent.mkdir(); marker.write_bytes(b"KEEP_OLD_RECEIPT")
        with patch.object(self.driver, "HOST", "windows"), \
                patch.object(self.driver, "resolve_target", return_value=self.target), \
                patch.object(self.driver, "run") as run:
            with self.assertRaises(SystemExit): self.driver.cmd_build_project(self.args)
            run.assert_not_called()
            self.assertEqual(marker.read_bytes(), b"KEEP_OLD_RECEIPT")


if __name__ == "__main__":
    unittest.main()
