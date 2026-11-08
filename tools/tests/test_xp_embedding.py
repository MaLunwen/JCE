"""CPU command/ownership/receipt controls; never invokes a native probe tool."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import struct
import tempfile
import unittest
from unittest.mock import patch

from tools.build import xp_embedding as subject
from tools.tests.test_xp_compat import sample_pe

ROOT = Path(__file__).resolve().parents[2]
CRYPTO = ["CryptAcquireContextW", "CryptGenRandom", "CryptReleaseContext"]


class XpEmbeddingTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "build/validation/xp-embedding/cpu-tests"
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "repo with spaces"
        for relative in (
                "tests/compatibility/xp_embedding/CMakeLists.txt",
                "tests/compatibility/xp_embedding/main.cpp",
                "tests/compatibility/xp_embedding/resource.rc.in",
                "tests/compatibility/xp_core/main.cpp", "engine/src/os/platform/jce_entropy.c",
                "engine/include/jce/os/platform/jce_entropy.h", "engine/include/jce/jce_compat.h",
                "cmake/JCESDKHelpers.cmake", "cmake/JCESDKTarget.cmake", "cmake/JCEWindowsTarget.cmake",
                "cmake/toolchains/windows-xp-llvm-common.cmake",
                "cmake/toolchains/windows-x64-xp-llvm.cmake",
                "cmake/toolchains/windows-x86-xp-llvm.cmake", "tools/jce_bin2obj.c",
                "tools/build/xp_embedding.py", "tools/build/xp_compat.py"):
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((ROOT / relative).read_bytes())
        self.tc = self.root / "llvm-mingw-xp-22.1.7-msvcrt"
        for arch, info in subject.compat.ARCH.items():
            for name in ("clang", "clang++", "windres"):
                target = self.tc / "bin" / (info["triple"] + "-" + name + ".exe")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((arch + name).encode("ascii"))
            config = "x86_64" if arch == "x64" else "i686"
            (self.tc / "bin" / (config + "-w64-windows-gnu.cfg")).write_text("-rtlib=compiler-rt")
        self.host = self.root / "host tools/bin2obj.exe"
        self.host.parent.mkdir(parents=True, exist_ok=True)
        self.host.write_bytes(b"counted host tool; never executed")
        self.programs = {}
        for name in ("cmake", "ninja"):
            path = self.root / "host tools" / (name + ".exe")
            path.write_bytes(name.encode("ascii"))
            self.programs[name] = str(path)
        self.calls = []
        self.fail_build = False
        self.local_stdout = subject.MARKER + "\n"
        self.wrong_target = False
        self.pe_arch = None
        self.pe_subsystem = None
        self.real_audit = subject.compat.audit_pe
        self.addCleanup(patch.stopall)
        patch.object(subject.shutil, "which", side_effect=self.programs.get).start()
        patch.object(subject.subprocess, "run", side_effect=self.execute).start()
        self.audit = patch.object(subject.compat, "audit_pe", return_value={
            "imports": {"advapi32.dll": CRYPTO}, "import_audit_scope": "counted CPU audit port"}).start()

    def output(self, arch="x64"):
        return self.root / "build/compat" / f"windows-{arch}-xp-embedding-llvm-msvcrt"

    def execute(self, command, **options):
        self.calls.append((command, options))
        self.assertIsInstance(command, list)
        self.assertNotIn("shell", options)
        if "--version" in command:
            arch = "x86" if "i686" in command[0] else "x64"
            target = "i686" if arch == "x86" else "x86_64"
            if self.wrong_target:
                target = "aarch64"
            stdout = "clang version 22.1.7\nTarget: " + target + "-w64-windows-gnu\n"
        elif "-dM" in command:
            stdout = "#define __clang__ 1\n#define __MINGW32__ 1\n#define _WIN32 1\n"
            if "x86_64" in command[0]:
                stdout += "#define _WIN64 1\n"
        elif "-S" in command:
            output = Path(command[command.index("-B") + 1])
            source = command[command.index("-S") + 1]
            values = {item[2:].split("=", 1)[0].split(":", 1)[0]: item.split("=", 1)[1]
                      for item in command if item.startswith("-D")}
            triple = subject.compat.ARCH[values["JCE_XP_EXPECTED_ARCH"]]["triple"]
            values.update(CMAKE_HOME_DIRECTORY=source, CMAKE_GENERATOR="Ninja",
                CMAKE_C_COMPILER=str(self.tc / "bin" / (triple + "-clang.exe")),
                CMAKE_CXX_COMPILER=str(self.tc / "bin" / (triple + "-clang++.exe")))
            (output / "CMakeCache.txt").write_text("".join(
                key + ":STRING=" + value + "\n" for key, value in values.items()))
            (output / "payload.S").write_text("counted generated assembly port\n")
            (output / "probe.rc").write_text("counted generated RC port\n")
            (output / "compile_commands.json").write_text("[]\n")
            stdout = "configured by counted port\n"
        elif "--build" in command:
            if self.fail_build:
                return subprocess.CompletedProcess(command, 7, "build stdout\n", "build failure\n")
            output = Path(command[command.index("--build") + 1])
            arch = "x86" if "windows-x86-" in output.name else "x64"
            target = command[command.index("--target") + 1]
            subsystem = 2 if target == "jce_xp_embedding_gui_probe" else 3
            exe = output / "out" / (target + ".exe")
            exe.parent.mkdir(parents=True, exist_ok=True)
            raw = bytearray(sample_pe(self.pe_arch or arch))
            struct.pack_into("<H", raw, 0x98 + 68, self.pe_subsystem or subsystem)
            exe.write_bytes(raw)
            stdout = "built by counted port\n"
        else:
            self.assertIn(Path(command[0]).name, ("jce_xp_embedding_probe.exe", "jce_xp_embedding_gui_probe.exe"))
            stdout = self.local_stdout
        return subprocess.CompletedProcess(command, 0, stdout, "separate stderr\n")

    def run_build(self, arch="x64", **options):
        with contextlib.redirect_stdout(io.StringIO()):
            return subject.build(self.root, arch, self.tc, self.host, **options)

    def receipt(self, arch="x64"):
        attempts = sorted((self.root / "build/validation/xp-embedding" / arch).iterdir())
        return json.loads((attempts[-1] / "receipt.json").read_text()), attempts[-1]

    def test_both_archs_use_separate_argv_target_rc_asm_and_explicit_budget(self):
        for arch in ("x86", "x64"):
            self.calls.clear()
            exe = self.run_build(arch, jobs=2)
            configure = next(argv for argv, _ in self.calls if "-S" in argv)
            expected_rc = (self.tc / "bin" / (subject.compat.ARCH[arch]["triple"] + "-windres.exe")).as_posix()
            self.assertIn("-DCMAKE_RC_COMPILER:FILEPATH=" + expected_rc, configure)
            self.assertIn("-DJCE_BIN2OBJ_EXECUTABLE:FILEPATH=" + self.host.as_posix(), configure)
            self.assertIn("-DJCE_XP_EXPECTED_ARCH=" + arch, configure)
            self.assertEqual(configure[configure.index("-S") + 1], (self.root / "tests/compatibility/xp_embedding").as_posix())
            build = next(argv for argv, _ in self.calls if "--build" in argv)
            self.assertEqual(build[-2:], ["--parallel", "2"])
            self.assertEqual(exe.parent.parent, self.output(arch))
            self.assertEqual(len(self.calls), 4, "unexpected native tool or local execution")
            receipt, attempt = self.receipt(arch)
            self.assertEqual(receipt["state"], "BUILD_AND_STATIC_PE_AUDIT_PASSED")
            self.assertFalse(receipt["xp_execution_verified"])
            self.assertFalse(receipt["local_execution"]["verified"])
            self.assertEqual(receipt["executable"]["sha256"], subject.compat.sha256_file(exe))
            self.assertEqual((attempt / "03-stderr.log").read_text(), "separate stderr\n")
            self.assertTrue(receipt["sources"])

    def test_all_actual_cmake_path_cache_arguments_use_forward_slashes_and_types(self):
        self.run_build()
        configure = next(argv for argv, _ in self.calls if "-S" in argv)
        expected = {
            "CMAKE_MAKE_PROGRAM:FILEPATH": Path(self.programs["ninja"]),
            "CMAKE_TOOLCHAIN_FILE:FILEPATH": self.root / "cmake/toolchains/windows-x64-xp-llvm.cmake",
            "JCE_XP_TOOLCHAIN_ROOT:PATH": self.tc,
            "JCE_BIN2OBJ_EXECUTABLE:FILEPATH": self.host,
            "CMAKE_ASM_COMPILER:FILEPATH": self.tc / "bin/x86_64-w64-mingw32-clang.exe",
            "CMAKE_RC_COMPILER:FILEPATH": self.tc / "bin/x86_64-w64-mingw32-windres.exe",
            "JCE_XP_EMBED_PAYLOAD:FILEPATH": self.output() / "payload.bin"}
        for key, path in expected.items():
            value = next((arg.split("=", 1)[1] for arg in configure
                          if arg.startswith("-D" + key + "=")), None)
            self.assertEqual(value, path.as_posix(), "typed CMake path argument missing or not normalized: " + key)
            self.assertNotIn(chr(92), value, "native path escape could corrupt generated CMake compiler files")
        for flag in ("-S", "-B"):
            self.assertNotIn(chr(92), configure[configure.index(flag) + 1])

    def test_existing_native_path_owner_remains_unchanged_when_semantically_equal(self):
        self.run_build()
        owner = self.output() / "jce-xp-embedding-owner.json"
        request = json.loads(owner.read_text())
        for key in subject.CMAKE_PATH_KEYS:
            if key in request["cache"]:
                request["cache"][key] = request["cache"][key].replace("/", chr(92))
        owner.write_text(json.dumps(request))
        before = owner.read_bytes()
        self.run_build(gui=True)
        self.assertEqual(owner.read_bytes(), before, "existing equivalent owner marker was rewritten")

    def test_gui_target_reuses_console_cache_and_checks_actual_gui_subsystem(self):
        self.run_build()
        owner = self.output() / "jce-xp-embedding-owner.json"
        before = owner.read_bytes()
        self.calls.clear()
        exe = self.run_build(gui=True)
        build = next(argv for argv, _ in self.calls if "--build" in argv)
        self.assertEqual(build[build.index("--target") + 1], "jce_xp_embedding_gui_probe")
        self.assertEqual(exe.name, "jce_xp_embedding_gui_probe.exe")
        self.assertEqual(owner.read_bytes(), before)
        receipt, _ = self.receipt()
        self.assertEqual(receipt["requested_target"], "jce_xp_embedding_gui_probe")
        self.assertEqual(receipt["requested_subsystem"], 2)
        self.assertEqual(receipt["pe"]["subsystem"], 2)
        self.pe_subsystem = 3
        with self.assertRaisesRegex(ValueError, "PE subsystem"):
            self.run_build(gui=True)
        self.pe_subsystem = 2
        with self.assertRaisesRegex(ValueError, "PE subsystem"):
            self.run_build()

    def test_gui_cli_selects_shared_cache_target_without_local_execution(self):
        with patch.object(subject, "build") as build:
            self.assertEqual(subject.main(["--repo", str(self.root), "--arch", "x64",
                "--toolchain-root", str(self.tc), "--host-bin2obj", str(self.host), "--gui"]), 0)
        build.assert_called_once_with(self.root, "x64", self.tc, self.host, 1, False, True)

    def test_matching_cache_reuses_same_owner_and_never_cleans(self):
        self.run_build()
        owner = self.output() / "jce-xp-embedding-owner.json"
        before = owner.read_bytes()
        self.run_build()
        self.assertEqual(owner.read_bytes(), before)
        self.assertEqual(len(list((self.root / "build/validation/xp-embedding/x64").iterdir())), 2)

    def test_wrong_cached_arch_or_rc_refuses_before_configure(self):
        self.run_build()
        cache = self.output() / "CMakeCache.txt"
        body = cache.read_text()
        cache.write_text(body.replace("JCE_XP_EXPECTED_ARCH:STRING=x64", "JCE_XP_EXPECTED_ARCH:STRING=x86"))
        self.calls.clear()
        with self.assertRaisesRegex(ValueError, "cache mismatch"):
            self.run_build()
        self.assertFalse(any("-S" in argv for argv, _ in self.calls))

    def test_changed_windows_floor_authority_refuses_cached_owner(self):
        self.run_build()
        baseline = self.root / "cmake/JCEWindowsTarget.cmake"
        baseline.write_text(baseline.read_text() + "# changed authority\n")
        self.calls.clear()
        with self.assertRaisesRegex(ValueError, "ownership tuple"):
            self.run_build(gui=True)
        self.assertFalse(any("-S" in argv for argv, _ in self.calls))

    def test_changed_host_tool_or_unowned_cache_refuses_before_configure(self):
        self.run_build()
        self.host.write_bytes(b"changed host tool")
        self.calls.clear()
        with self.assertRaisesRegex(ValueError, "ownership tuple"):
            self.run_build()
        self.assertFalse(any("-S" in argv for argv, _ in self.calls))
        output = self.output("x86")
        output.mkdir(parents=True)
        (output / "foreign.txt").write_text("foreign owner")
        with self.assertRaisesRegex(ValueError, "without ownership"):
            self.run_build("x86")

    def test_bad_payload_refuses_instead_of_rewriting_before_configure(self):
        self.run_build()
        payload = self.output() / "payload.bin"
        payload.write_bytes(b"wrong bytes")
        self.calls.clear()
        with self.assertRaisesRegex(ValueError, "payload differs"):
            self.run_build()
        self.assertEqual(payload.read_bytes(), b"wrong bytes")
        self.assertFalse(any("-S" in argv for argv, _ in self.calls))

    def test_missing_host_or_target_resource_tool_is_rejected(self):
        self.host.unlink()
        with self.assertRaisesRegex(ValueError, "input is missing"):
            self.run_build()
        self.assertEqual(self.calls, [])
        self.host.write_bytes(b"host tool")
        (self.tc / "bin/x86_64-w64-mingw32-windres.exe").unlink()
        with self.assertRaisesRegex(ValueError, "input is missing"):
            self.run_build()
        self.assertEqual(self.calls, [])

    def test_wrong_preflight_target_refuses_before_cmake(self):
        self.wrong_target = True
        with self.assertRaisesRegex(ValueError, "wrong ABI/arch"):
            self.run_build()
        self.assertFalse(any("-S" in argv for argv, _ in self.calls))

    def test_actual_pe_auditor_rejects_wrong_architecture(self):
        self.audit.side_effect = self.real_audit
        self.pe_arch = "x86"
        with self.assertRaisesRegex(ValueError, "PE machine"):
            self.run_build()
        self.assertEqual(self.receipt()[0]["state"], "FAILED")

    def test_missing_entropy_import_refuses_static_success(self):
        self.audit.return_value = {"imports": {"advapi32.dll": ["CryptGenRandom"]}}
        with self.assertRaisesRegex(ValueError, "real entropy CryptoAPI"):
            self.run_build()
        self.assertEqual(self.receipt()[0]["state"], "FAILED")

    def test_failure_retains_stdout_stderr_command_and_failure_receipt(self):
        self.fail_build = True
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_build()
        receipt, attempt = self.receipt()
        self.assertEqual(receipt["state"], "FAILED")
        self.assertEqual(receipt["commands"][-1]["returncode"], 7)
        self.assertEqual((attempt / "03-stdout.log").read_text(), "build stdout\n")
        self.assertEqual((attempt / "03-stderr.log").read_text(), "build failure\n")
        self.assertFalse((self.output() / ".xp-embedding.lock").exists())

    def test_existing_lock_is_never_removed_by_failed_contender(self):
        self.run_build()
        lock = self.output() / ".xp-embedding.lock"
        lock.write_text("other producer")
        with self.assertRaises(FileExistsError):
            self.run_build()
        self.assertEqual(lock.read_text(), "other producer")

    def test_local_run_requires_final_embedding_marker_not_core_marker(self):
        self.local_stdout = "JCE_XP_A1_PROBE_OK\n"
        with self.assertRaisesRegex(ValueError, "final embedding marker"):
            self.run_build(run_local=True)
        self.assertFalse(self.receipt()[0]["local_execution"]["verified"])
        self.local_stdout = "JCE_XP_A1_PROBE_OK\n" + subject.MARKER + "\n"
        self.run_build(run_local=True)
        self.assertTrue(self.receipt()[0]["local_execution"]["verified"])
        self.assertFalse(self.receipt()[0]["xp_execution_verified"])

    def test_invalid_jobs_do_not_launch_or_mutate_probe_cache(self):
        for jobs in (0, 257, True, "2"):
            with self.assertRaisesRegex(ValueError, "jobs"):
                self.run_build(jobs=jobs)
        self.assertFalse(self.output().exists())
        self.assertEqual(self.calls, [])

    def test_cli_preserves_argument_boundaries_and_defaults_to_one_job(self):
        with patch.object(subject, "build") as build:
            self.assertEqual(subject.main(["--repo", str(self.root), "--arch", "x86",
                "--toolchain-root", str(self.tc), "--host-bin2obj", str(self.host)]), 0)
        build.assert_called_once_with(self.root, "x86", self.tc, self.host, 1, False, False)

    @staticmethod
    def check_fixture_contract(cmake, main, resource):
        for wire in ("_jce_target_embed_binary(xp_embedding_assets", "assets_pak_data",
                     "JCEWindowsTarget.cmake", "set(JCE_WINXP_LEGACY ON",
                     "main=jce_xp_core_main", "/xp_core/main.cpp", "-static -pthread",
                     "${JCE_XP_TRIPLE}-windres.exe", "jce_entropy.c", "_payload_hex STREQUAL",
                     "add_executable(${_target} WIN32", "xp_embedding_body OBJECT"):
            assert wire in cmake, "actual fixture CMake wire absent: " + wire
        for wire in ("assets_pak_data_size != sizeof(expected)",
                     "std::memcmp(assets_pak_data, expected, sizeof(expected))",
                     "SizeofResource(module, resource) != sizeof(expected)",
                     "std::memcmp(bytes, expected, sizeof(expected))", "return 20;", "return 21;", "return 22;",
                     "JCE_XP_EMBEDDING_PROBE_OK", "const int core = jce_xp_core_main();",
                     "int WINAPI WinMain(", "return run_probe();"):
            assert wire in main, "actual fixture byte/return wire absent: " + wire
        assert '101 RCDATA "@JCE_XP_EMBED_PAYLOAD_RC@"' in resource

    def test_fixture_contract_and_ordinary_disconnected_byte_check_controls(self):
        fixture = ROOT / "tests/compatibility/xp_embedding"
        cmake = (fixture / "CMakeLists.txt").read_text()
        main = (fixture / "main.cpp").read_text()
        resource = (fixture / "resource.rc.in").read_text()
        self.check_fixture_contract(cmake, main, resource)
        for wire in ("assets_pak_data_size != sizeof(expected)",
                     "std::memcmp(bytes, expected, sizeof(expected))", "return 22;"):
            with self.subTest(wire=wire), self.assertRaises(AssertionError):
                self.check_fixture_contract(cmake, main.replace(wire, ""), resource)
        with self.assertRaises(AssertionError):
            self.check_fixture_contract(cmake.replace("_jce_target_embed_binary(xp_embedding_assets", ""), main, resource)


if __name__ == "__main__":
    unittest.main()
