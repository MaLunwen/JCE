"""IPO success-cache controls using tiny CMake and counted compiler ports.

The production SDK wrapper computes real file/input identities. Only the enabled
language query and native IPO compile/link invocation are typed counting ports;
no compiler, upstream patch, build, application or GPU is used by these tests.
--old-repeat is an ordinary source mutant; unchanged warm assertions must fail.
"""
from __future__ import annotations
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HELPERS = ROOT / "cmake/JCESDKHelpers.cmake"
OLD_REPEAT = "--old-repeat" in sys.argv
if OLD_REPEAT:
    sys.argv.remove("--old-repeat")


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"),
                     "CMake/Ninja unavailable; IPO CPU cache controls unavailable")
class IpoProbeCacheControls(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="jce-ipo-cache-control-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.build = self.root / "generated"
        cmake = self.root / "cmake"
        cmake.mkdir()
        source = HELPERS.read_text(encoding="utf-8")
        if OLD_REPEAT:
            before = 'if(_fingerprint AND DEFINED CACHE{${_cache}} AND "${${_cache}}" STREQUAL "YES")'
            self.assertEqual(source.count(before), 1)
            source = source.replace(before, 'if(FALSE)')
        self.helper = cmake / HELPERS.name
        self.helper.write_text(source, encoding="utf-8")
        shutil.copyfile(ROOT / "cmake/JCESDKTarget.cmake", cmake / "JCESDKTarget.cmake")
        self.compiler = self.root / "counted-compiler.tool"
        self.compiler.write_bytes(b"compiler identity one")
        self.linker = self.root / "counted-linker.tool"
        self.linker.write_bytes(b"linker identity one")
        self.auxiliary = {}
        for language in ("RC", "ASM"):
            self.auxiliary[language] = self.root / ("counted-" + language + ".tool")
            self.auxiliary[language].write_bytes(b"auxiliary identity one")
        preamble = f"""cmake_minimum_required(VERSION 3.23)
project(IpoCacheControl LANGUAGES NONE)
set(MSVC TRUE)
if(IPO_TEST_NEW_POLICY)
    cmake_policy(SET CMP0138 NEW)
endif()
if(IPO_TEST_NEW_POLICY137)
    cmake_policy(SET CMP0137 NEW)
endif()
include("{self.helper.as_posix()}")
# Model the native compiler metadata without enabling a compiler in project().
foreach(_lang C CXX)
    set(CMAKE_${{_lang}}_COMPILER_LOADED TRUE)
    if(NOT DEFINED CMAKE_${{_lang}}_COMPILER)
        set(CMAKE_${{_lang}}_COMPILER "{self.compiler.as_posix()}")
    endif()
    if(NOT DEFINED CMAKE_${{_lang}}_COMPILER_ID)
        set(CMAKE_${{_lang}}_COMPILER_ID MSVC)
    endif()
    if(NOT DEFINED CMAKE_${{_lang}}_COMPILER_VERSION)
        set(CMAKE_${{_lang}}_COMPILER_VERSION 19.43)
    endif()
endforeach()
foreach(_lang RC ASM)
    if(NOT DEFINED CMAKE_${{_lang}}_COMPILER_LOADED)
        set(CMAKE_${{_lang}}_COMPILER_LOADED TRUE)
    endif()
    if(NOT DEFINED CMAKE_${{_lang}}_COMPILER)
        set(CMAKE_${{_lang}}_COMPILER "{self.root.as_posix()}/counted-${{_lang}}.tool")
    endif()
endforeach()
set(CMAKE_LINKER "{self.linker.as_posix()}")
set(CMAKE_AR "{self.linker.as_posix()}")
set(CMAKE_RANLIB ":")
if(NOT DEFINED IPO_TEST_LANGUAGES)
    set(IPO_TEST_LANGUAGES "C;CXX;RC")
endif()
# CMake's real ENABLED_LANGUAGES is read-only in project(NONE). This single
# metadata query is a typed port; global memoization uses the real property API.
function(get_property OUT)
    if("${{ARGN}}" STREQUAL "GLOBAL;PROPERTY;ENABLED_LANGUAGES")
        set(${{OUT}} "${{IPO_TEST_LANGUAGES}}" PARENT_SCOPE)
    else()
        _get_property(${{ARGV}})
        set(${{OUT}} "${{${{OUT}}}}" PARENT_SCOPE)
    endif()
endfunction()
function(_jce_run_ipo_probe OUT_RESULT OUT_ERROR)
    file(APPEND "${{CMAKE_BINARY_DIR}}/counted-probe-calls.txt" "CALL\n")
    if(IPO_TEST_FAIL)
        set(${{OUT_RESULT}} NO PARENT_SCOPE)
        set(${{OUT_ERROR}} "counted compile/link failure" PARENT_SCOPE)
    else()
        set(${{OUT_RESULT}} YES PARENT_SCOPE)
        set(${{OUT_ERROR}} "" PARENT_SCOPE)
    endif()
endfunction()
if(IPO_TEST_OPAQUE)
    set(${{IPO_TEST_OPAQUE}} "opaque-file-or-setting")
endif()
if(IPO_TEST_SINGLE_OUTPUT)
    _jce_ipo_fingerprint(_identity_only)
endif()
foreach(_call RANGE 1 2)
    _jce_check_ipo_cached(_ok _error)
    if(IPO_TEST_FAIL)
        if(NOT _ok STREQUAL "NO" OR NOT _error STREQUAL "counted compile/link failure")
            message(FATAL_ERROR "real failure/diagnostic was changed")
        endif()
    elseif(NOT _ok STREQUAL "YES" OR NOT _error STREQUAL "")
        message(FATAL_ERROR "successful IPO semantics changed")
    endif()
endforeach()
"""
        (self.root / "CMakeLists.txt").write_text(preamble, encoding="utf-8")

    def configure(self, *arguments, env=None):
        result = subprocess.run([shutil.which("cmake"), "-S", str(self.root),
            "-B", str(self.build), "-G", "Ninja", *arguments], env=env,
            text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def calls(self):
        return (self.build / "counted-probe-calls.txt").read_text().splitlines()

    def test_same_inputs_reuse_success_within_and_across_configure(self):
        first = self.configure()
        self.assertEqual(self.calls(), ["CALL"])
        witness = self.build / "counted-probe-calls.txt"
        before = (hashlib.sha256(witness.read_bytes()).hexdigest(), witness.stat().st_mtime_ns)
        second = self.configure()
        self.assertEqual(self.calls(), ["CALL"])
        self.assertEqual((hashlib.sha256(witness.read_bytes()).hexdigest(), witness.stat().st_mtime_ns), before)
        # Fingerprint cost is observable: content hashing runs once per real
        # configure, then repeated targets reuse only that configure's digest.
        self.assertEqual(first.count("JCE dist IPO: fingerprinted"), 1)
        self.assertEqual(second.count("JCE dist IPO: fingerprinted"), 1)

    def test_changed_effective_inputs_each_require_a_real_probe(self):
        self.configure()
        changes = ["-DCMAKE_C_FLAGS=/changed-c", "-DCMAKE_CXX_FLAGS_RELEASE=/changed-cxx",
            "-DCMAKE_C_FLAGS_DEBUG=/changed-debug", "-DCMAKE_EXE_LINKER_FLAGS=/changed-link",
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL", "-DCMAKE_C_COMPILER_TARGET=arm64",
            "-DCMAKE_CXX_COMPILER_ARCHITECTURE_ID=ARM64", "-DCMAKE_C_COMPILER_VERSION=20.1",
            "-DCMAKE_CXX_COMPILER_FRONTEND_VARIANT=GNU", "-DCMAKE_C_STANDARD=11",
            "-DCMAKE_TRY_COMPILE_CONFIGURATION=Release", "-DCMAKE_OSX_ARCHITECTURES=arm64",
            "-DCMAKE_SYSROOT=/modeled-sysroot", "-DIPO_TEST_NEW_POLICY=ON",
            "-DIPO_TEST_NEW_POLICY137=ON",
            "-DIPO_TEST_LANGUAGES=CXX"]
        for index, argument in enumerate(changes, 2):
            with self.subTest(input=argument):
                self.configure(argument)
                self.assertEqual(len(self.calls()), index)

    def test_compiler_id_scratch_output_is_not_a_probe_input(self):
        self.configure("-DCMAKE_C_COMPILER_PRODUCED_OUTPUT=first-configure-log")
        self.configure("-U", "CMAKE_C_COMPILER_PRODUCED_OUTPUT")
        self.assertEqual(self.calls(), ["CALL"])

    def test_compiler_bytes_changed_at_same_size_and_mtime_are_rehashed(self):
        self.configure()
        before = self.compiler.stat()
        self.compiler.write_bytes(b"compiler identity two")
        os.utime(self.compiler, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.configure()
        self.assertEqual(len(self.calls()), 2)

    def test_probe_owner_change_invalidates_success(self):
        self.configure()
        with self.helper.open("a", encoding="utf-8") as stream:
            stream.write("\n# changed first-party probe owner\n")
        self.configure()
        self.assertEqual(len(self.calls()), 2)

    def test_environment_change_invalidates_success(self):
        self.configure()
        environment = dict(os.environ)
        for index, variable in enumerate(("CL", "LINK", "_LINK_"), 2):
            with self.subTest(environment=variable):
                environment[variable] = "/counted-env-input"
                self.configure(env=environment)
                self.assertEqual(len(self.calls()), index)

    def test_failure_is_never_persistently_cached_or_relabelled(self):
        self.configure("-DIPO_TEST_FAIL=ON")
        self.assertEqual(len(self.calls()), 2)
        self.configure("-DIPO_TEST_FAIL=ON")
        self.assertEqual(len(self.calls()), 4)
        cache = (self.build / "CMakeCache.txt").read_text(encoding="utf-8")
        self.assertNotIn("JCE_IPO_SUCCESS_", cache)
        self.configure("-DIPO_TEST_FAIL=OFF")
        self.assertEqual(len(self.calls()), 5)

    def test_opaque_toolchain_rules_launchers_and_unknown_languages_keep_probing(self):
        for index, variable in enumerate(("CMAKE_TOOLCHAIN_FILE", "CMAKE_MODULE_PATH",
                "CMAKE_USER_MAKE_RULES_OVERRIDE", "CMAKE_C_COMPILER_LAUNCHER",
                "CMAKE_TRY_COMPILE_PLATFORM_VARIABLES"), 1):
            with self.subTest(opaque=variable):
                log = self.configure("-DIPO_TEST_OPAQUE=" + variable)
                message = "real probe; cache unavailable reason=opaque_input key=" + variable
                self.assertEqual(log.count(message), 1)
                self.assertEqual(len(self.calls()), 2 * index)
        for count, language in enumerate(("Fortran", "CUDA", "Swift"), 6):
            log = self.configure("-DIPO_TEST_OPAQUE=", "-DIPO_TEST_LANGUAGES=C;" + language)
            self.assertEqual(log.count("reason=language_or_compiler key=ENABLED_LANGUAGES"), 1)
            self.assertEqual(len(self.calls()), count * 2)

    def test_missing_compiler_identity_keeps_probing(self):
        for count in (2, 4):
            log = self.configure("-DCMAKE_C_COMPILER_ID=")
            self.assertEqual(log.count("reason=language_or_compiler key=CMAKE_C_COMPILER_ID"), 1)
            self.assertEqual(len(self.calls()), count)

    def test_no_languages_reason_is_reported_once_per_configure(self):
        log = self.configure("-DIPO_TEST_LANGUAGES=")
        self.assertEqual(log.count("reason=languages_or_version key=ENABLED_LANGUAGES"), 1)
        self.assertEqual(len(self.calls()), 2)

    def test_relative_tool_reason_is_exact_and_does_not_expand_admission(self):
        for count in (2, 4):
            log = self.configure("-DCMAKE_C_COMPILER=cl")
            self.assertEqual(log.count("reason=non_absolute_tool key=CMAKE_C_COMPILER"), 1)
            self.assertEqual(len(self.calls()), count)

    def test_missing_file_reason_keeps_the_real_probe(self):
        missing = self.root / "absent-compiler.tool"
        log = self.configure("-DCMAKE_C_COMPILER=" + missing.as_posix())
        self.assertEqual(log.count("reason=missing_input_file key=" + missing.as_posix()), 1)
        self.assertEqual(len(self.calls()), 2)

    def test_fingerprint_optional_reason_preserves_single_output_call(self):
        self.configure("-DIPO_TEST_SINGLE_OUTPUT=ON")
        self.assertEqual(self.calls(), ["CALL"])

    def test_ignored_rc_asm_preserve_full_enabled_set_identity(self):
        self.configure()
        self.assertEqual(self.calls(), ["CALL"])
        self.configure()
        self.assertEqual(self.calls(), ["CALL"])
        self.configure("-DIPO_TEST_LANGUAGES=C;CXX;RC;ASM")
        self.assertEqual(len(self.calls()), 2)
        self.configure("-DIPO_TEST_LANGUAGES=C;CXX;RC;ASM")
        self.assertEqual(len(self.calls()), 2)
        self.configure("-DIPO_TEST_LANGUAGES=C;CXX;ASM")
        self.assertEqual(len(self.calls()), 3)

    def test_auxiliary_tool_bytes_and_flags_changes_require_real_probe(self):
        args = ["-DIPO_TEST_LANGUAGES=C;CXX;RC;ASM"]
        self.configure(*args)
        for count, language in enumerate(("RC", "ASM"), 2):
            tool = self.auxiliary[language]
            before = tool.stat()
            tool.write_bytes(b"auxiliary identity two")
            os.utime(tool, ns=(before.st_atime_ns, before.st_mtime_ns))
            self.configure(*args)
            self.assertEqual(len(self.calls()), count)
        for count, argument in enumerate(("-DCMAKE_RC_FLAGS=/changed-rc",
                "-DCMAKE_RC_FLAGS_DEBUG=/changed-debug", "-DCMAKE_ASM_FLAGS=/changed-asm",
                "-DCMAKE_ASM_FLAGS_RELEASE=/changed-release"), 4):
            args.append(argument)
            self.configure(*args)
            self.assertEqual(len(self.calls()), count)

    def test_incomplete_auxiliary_identity_and_auxiliary_only_set_keep_probing(self):
        log = self.configure("-DIPO_TEST_LANGUAGES=C;ASM", "-DCMAKE_ASM_COMPILER_LOADED=")
        self.assertEqual(log.count("reason=language_or_compiler key=CMAKE_ASM_COMPILER_LOADED"), 1)
        self.assertEqual(len(self.calls()), 2)
        log = self.configure("-DIPO_TEST_LANGUAGES=RC;ASM")
        self.assertEqual(log.count("reason=languages_or_version key=ENABLED_LANGUAGES"), 1)
        self.assertEqual(len(self.calls()), 4)

    def test_actual_application_owner_calls_the_wrapper(self):
        source = HELPERS.read_text(encoding="utf-8")
        start = source.index("function(jce_configure_application_target TARGET)")
        end = source.index("endfunction()", start)
        owner = source[start:end]
        self.assertEqual(owner.count("_jce_check_ipo_cached(_jce_ipo_ok _jce_ipo_error)"), 1)
        self.assertNotIn("check_ipo_supported(", owner)
        self.assertIn("INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE", owner)
        self.assertIn("INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE", owner)
        self.assertIn("JCE dist: IPO/LTO unavailable", owner)
        begin = source.index("function(_jce_run_ipo_probe ")
        end = source.index("endfunction()", begin)
        native = source[begin:end]
        self.assertIn("check_ipo_supported(RESULT _ok OUTPUT _error)", native)
        self.assertNotIn("LANGUAGES", native)


if __name__ == "__main__":
    unittest.main()
