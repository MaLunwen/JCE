"""Pure command/receipt controls for the real XP mimalloc producer.

All compiler/build/run commands use counted ports and synthetic PE bytes.
These controls prove driver admission and failure handling, never native or
XP execution, C compilation, allocator operation or graphical support.
"""
import contextlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools.build import xp_mimalloc as subject
from tools.tests.test_xp_native_probe import sample_pe, write_export_receipt

ROOT = Path(__file__).resolve().parents[2]


class XpMimallocDriverTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "build/validation/xp-mimalloc/cpu-driver-tests"
        parent.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "repo with spaces"
        self.assertTrue(self.root.resolve().is_relative_to(parent.resolve()))
        relatives = list(subject.INPUTS) + ["cmake/toolchains/windows-" + arch + "-xp-llvm.cmake"
                    for arch in ("x86", "x64")] + ["cmake/toolchains/windows-xp-llvm-common.cmake"]
        for relative in relatives:
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, target)
        self.include = self.root / "unchanged include"
        (self.include / "mimalloc").mkdir(parents=True)
        (self.include / "mimalloc/atomic.h").write_bytes(b"counted complete header; never compiled\n")
        self.tc = self.root / "llvm-mingw-xp-msvcrt"
        (self.tc / "bin").mkdir(parents=True)
        for arch, triple in (("x86", "i686-w64-mingw32"), ("x64", "x86_64-w64-mingw32")):
            for name in ("clang", "clang++"):
                (self.tc / "bin" / (triple + "-" + name + ".exe")).write_bytes((arch + name).encode())
            stem = "i686" if arch == "x86" else "x86_64"
            (self.tc / "bin" / (stem + "-w64-windows-gnu.cfg")).write_bytes(b"counted compiler config")
        self.receipts = {}
        for arch in ("x86", "x64"):
            seed = self.root / ("synthetic-" + arch + ".exe")
            seed.write_bytes(self.pe(arch))
            self.receipts[arch] = write_export_receipt(self.root / ("exports-" + arch), seed, arch)
        self.programs = {}
        for name in ("cmake", "ninja"):
            path = self.root / (name + ".exe")
            path.write_bytes(name.encode())
            self.programs[name] = str(path)
        self.calls, self.private, self.leases = [], [], []
        self.bad_marker = self.bad_exit = self.mutation = self.pe_arch = None
        self.fail_build = False
        self.negative = False
        self.original_shared_execute = subject.shared.compat._execute
        factory = subject._compat
        def private_factory():
            module = factory()
            self.private.append(module)
            return module
        @contextlib.contextmanager
        def lease(resources, directory):
            self.leases.append((resources, directory))
            yield
        self.addCleanup(patch.stopall)
        patch.object(subject, "_compat", side_effect=private_factory).start()
        patch.object(subject.shutil, "which", side_effect=self.programs.get).start()
        patch.object(subject.subprocess, "run", side_effect=self.execute).start()
        patch.object(subject.cache_lock, "cache_locks", side_effect=lease).start()

    @staticmethod
    def pe(arch):
        # PSAPI is accepted only by the collector-bound full import owner.
        return sample_pe(arch, dlls={"kernel32.dll": ["GetProcAddress"],
            "msvcrt.dll": ["printf"], "psapi.dll": ["GetProcessMemoryInfo"]})

    def output(self, arch="x64"):
        return self.root / "build/compat" / ("windows-" + arch + "-xp-mimalloc-llvm-msvcrt")

    def execute(self, command, **options):
        self.calls.append(command)
        self.assertIsInstance(command, list)
        self.assertNotIn("shell", options)
        if "--version" in command:
            stem = "i686" if "i686" in command[0] else "x86_64"
            return subprocess.CompletedProcess(command, 0,
                "clang version 22.1.7\nTarget: " + stem + "-w64-windows-gnu\n", "")
        if "-dM" in command:
            macros = "#define __clang__ 1\n#define __MINGW32__ 1\n#define _WIN32 1\n"
            if "x86_64" in command[0]:
                macros += "#define _WIN64 1\n"
            return subprocess.CompletedProcess(command, 0, macros, "")
        if "-S" in command:
            output = Path(command[command.index("-B") + 1])
            source = command[command.index("-S") + 1]
            definitions = dict(value[2:].split("=", 1) for value in command if value.startswith("-D"))
            self.negative = bool(definitions["JCE_XP_MIMALLOC_MUTANT_DIR:PATH"])
            arch = "x86" if "windows-x86-" in output.name else "x64"
            triple = "i686-w64-mingw32" if arch == "x86" else "x86_64-w64-mingw32"
            entries = {"CMAKE_HOME_DIRECTORY": source,
                "CMAKE_TOOLCHAIN_FILE": definitions["CMAKE_TOOLCHAIN_FILE:FILEPATH"],
                "CMAKE_C_COMPILER": (self.tc / "bin" / (triple + "-clang.exe")).as_posix()}
            (output / "CMakeCache.txt").write_text("".join(key + ":PATH=" + value + "\n"
                for key, value in entries.items()), encoding="ascii", newline="\n")
            return subprocess.CompletedProcess(command, 0, "counted configure\n", "")
        if "--build" in command:
            if self.fail_build:
                return subprocess.CompletedProcess(command, 9, "counted stdout\n", "counted build failure\n")
            output = Path(command[command.index("--build") + 1])
            (output / "out").mkdir(exist_ok=True)
            arch = "x86" if "windows-x86-" in output.name else "x64"
            for name in (subject.TARGET, subject.MUTANT) if self.negative else (subject.TARGET,):
                (output / "out" / (name + ".exe")).write_bytes(self.pe(self.pe_arch or arch))
            if self.mutation == "header":
                with (self.include / "mimalloc/atomic.h").open("ab") as stream:
                    stream.write(b" changed")
            if self.mutation == "source":
                with (self.root / subject.INPUTS[1]).open("ab") as stream:
                    stream.write(b" changed")
            return subprocess.CompletedProcess(command, 0, "counted build\n", "")
        target, case = Path(command[0]).stem, command[1]
        if target == subject.MUTANT:
            marker, code = "JCE_XP_MIMALLOC_ASSERT_FAILED:try-publishes-held-state", 1
        elif case in subject.FATAL_CASES:
            marker, code = "JCE_XP_MIMALLOC_FAIL_STOP:" + case, 91
        else:
            marker, code = "JCE_XP_MIMALLOC_OK:" + case, 0
        if self.bad_marker in (case, target):
            marker += "-wrong"
        if self.bad_exit in (case, target):
            code = 91 if target == subject.MUTANT else 1
        if self.mutation == "executable":
            with Path(command[0]).open("ab") as stream:
                stream.write(b" changed")
        return subprocess.CompletedProcess(command, code, marker + "\n", "")

    def run_build(self, arch="x64", **options):
        with contextlib.redirect_stdout(io.StringIO()):
            return subject.build(self.root, arch, self.tc, self.include, self.receipts[arch], **options)

    def receipt(self, arch="x64"):
        directory = sorted((self.root / "build/validation/xp-mimalloc" / arch).iterdir())[-1]
        return json.loads((directory / "receipt.json").read_text()), directory

    def native_commands(self):
        return [row for row in self.calls if Path(row[0]).stem in (subject.TARGET, subject.MUTANT)]

    def test_cli_required_inputs_default_budget_and_closed_execution(self):
        argv = ["--repo", str(self.root), "--arch", "x86", "--toolchain-root", str(self.tc),
                "--mimalloc-include", str(self.include), "--export-receipt", str(self.receipts["x86"])]
        with patch.object(subject, "build") as build:
            self.assertEqual(subject.main(argv), 0)
        build.assert_called_once_with(self.root, "x86", self.tc, self.include,
                                     self.receipts["x86"], 1, False, False)
        for flag, value in (("--arch", "x86"), ("--export-receipt", str(self.receipts["x86"]))):
            reduced = argv[:]
            index = reduced.index(flag)
            del reduced[index:index + 2]
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                subject.build_parser().parse_args(reduced)
        with self.assertRaisesRegex(ValueError, "require explicit --run-local"):
            self.run_build(negative_controls=True)
        self.assertFalse(self.calls)
        self.assertEqual(self.receipt()[0]["state"], "FAILED")

    def test_arch_specific_cache_paths_jobs_and_no_local_execution_by_default(self):
        for arch in ("x86", "x64"):
            self.calls.clear()
            self.assertEqual(self.run_build(arch, jobs=2), self.output(arch))
            configure = next(row for row in self.calls if "-S" in row)
            for key, value in (("JCE_XP_MIMALLOC_INCLUDE:PATH", self.include),
                               ("JCE_XP_MIMALLOC_OUTPUT:PATH", self.output(arch) / "out"),
                               ("JCE_XP_TOOLCHAIN_ROOT:PATH", self.tc)):
                self.assertIn("-D" + key + "=" + value.as_posix(), configure)
            self.assertEqual(next(row for row in self.calls if "--build" in row)[-2:], ["--parallel", "2"])
            self.assertFalse(self.native_commands())
            self.assertEqual(self.leases[-1], ([self.output(arch)], self.root / "build/.locks"))
            receipt, _ = self.receipt(arch)
            self.assertEqual(receipt["executables"][subject.TARGET]["pe"]["state"], "STATIC_IMPORT_AUDIT_PASSED")
            self.assertFalse(receipt["local_execution_verified"] or receipt["xp_execution_verified"]
                             or receipt["editor_or_graphics_verified"])

    def test_private_preflight_never_replaces_shared_global(self):
        self.run_build()
        self.run_build()
        self.assertEqual(len(self.private), 2)
        self.assertIsNot(self.private[0], self.private[1])
        self.assertIs(subject.shared.compat._execute, self.original_shared_execute)

    def test_matching_owner_and_changed_sources_or_headers_incrementally_reconfigure(self):
        output = self.run_build()
        marker = output / "xp-mimalloc-owner.json"
        original = marker.read_bytes()
        for path in (self.include / "mimalloc/atomic.h", self.root / subject.INPUTS[1]):
            with path.open("ab") as stream:
                stream.write(b" ordinary changed input")
            self.calls.clear()
            self.run_build()
            self.assertTrue(any("-S" in row for row in self.calls))
            self.assertEqual(marker.read_bytes(), original)
        self.assertEqual(json.loads(original)["arch"], "x64")

    def test_changed_toolchain_or_wrong_arch_owner_refuses_reuse(self):
        output = self.run_build()
        marker = output / "xp-mimalloc-owner.json"
        original = marker.read_bytes()
        value = json.loads(original)
        value["arch"] = "x86"
        marker.write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError, "ownership mismatch"):
            self.run_build()
        marker.write_bytes(original)
        with (self.tc / "bin/x86_64-w64-mingw32-clang.exe").open("ab") as stream:
            stream.write(b" changed compiler")
        with self.assertRaisesRegex(ValueError, "ownership mismatch"):
            self.run_build()

    def test_unowned_or_wrong_cmake_cache_is_preserved_and_not_configured(self):
        output = self.output()
        output.mkdir(parents=True)
        foreign = output / "foreign"
        foreign.write_bytes(b"owned elsewhere")
        with self.assertRaisesRegex(ValueError, "nonempty and unowned"):
            self.run_build()
        self.assertEqual(foreign.read_bytes(), b"owned elsewhere")
        foreign.unlink()
        self.run_build()
        cache = output / "CMakeCache.txt"
        with cache.open("a") as stream:
            stream.write("CMAKE_C_COMPILER:FILEPATH=wrong-arch-compiler\n")
        self.calls.clear()
        with self.assertRaisesRegex(ValueError, "CMake cache mismatch"):
            self.run_build()
        self.assertFalse(any("-S" in row for row in self.calls))

    def test_local_positive_cases_have_exact_markers_and_no_guest_claim(self):
        self.run_build(run_local=True)
        receipt, _ = self.receipt()
        self.assertEqual([row["case"] for row in receipt["local_results"]], list(subject.CASES))
        self.assertTrue(receipt["local_execution_verified"])
        self.assertEqual(len(self.native_commands()), 7)
        self.assertFalse(receipt["negative_results"] or receipt["fail_stop_results"])
        self.assertFalse(receipt["xp_execution_verified"] or receipt["editor_or_graphics_verified"])

    def test_ordinary_assertion_and_fatal_cases_are_distinct_exact_exit_contracts(self):
        self.run_build(run_local=True, negative_controls=True)
        receipt, directory = self.receipt()
        self.assertEqual(len(receipt["local_results"]), 7)
        self.assertEqual(receipt["negative_results"][0]["exit_code"], 1)
        self.assertEqual(receipt["negative_results"][0]["kind"], "EXPECTED_ORDINARY_ASSERTION_FAILURE")
        self.assertEqual([row["exit_code"] for row in receipt["fail_stop_results"]], [91, 91, 91])
        self.assertEqual({row["kind"] for row in receipt["fail_stop_results"]}, {"EXPECTED_FAIL_STOP"})
        self.assertTrue((directory / "mutants/manifest.json").is_file())

    def test_wrong_marker_or_exit_cannot_claim_local_completion(self):
        for field, value in (("bad_marker", "exports-missing"), ("bad_exit", "ordinals"),
                             ("bad_marker", "fail-lock"), ("bad_exit", subject.MUTANT)):
            setattr(self, field, value)
            with self.subTest(field=field, value=value), self.assertRaisesRegex(ValueError, "unexpected exit or exact marker"):
                self.run_build(run_local=True, negative_controls=True)
            self.assertFalse(self.receipt()[0]["local_execution_verified"])
            setattr(self, field, None)

    def test_wrong_pe_arch_or_invalid_export_receipt_closes_execution_gate(self):
        self.pe_arch = "x86"
        with self.assertRaises(ValueError):
            self.run_build(run_local=True)
        self.assertFalse(self.native_commands())
        self.pe_arch = None
        value = json.loads(self.receipts["x64"].read_text())
        value["collected"] = False
        self.receipts["x64"].write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError, "successful collected"):
            self.run_build(run_local=True)
        self.assertFalse(self.native_commands())

    def test_input_changes_and_executable_mutation_fail_closed(self):
        for mutation in ("source", "header", "executable"):
            self.mutation = mutation
            with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, "changed"):
                self.run_build(run_local=True)
            self.assertFalse(self.receipt()[0]["local_execution_verified"])
            if mutation == "executable":
                self.assertEqual(len(self.native_commands()), 1,
                                 "changed executable reached a second scenario")
        self.mutation = None

    def test_build_failure_keeps_real_logs_and_failed_receipt(self):
        self.fail_build = True
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_build()
        receipt, directory = self.receipt()
        self.assertEqual(receipt["state"], "FAILED")
        self.assertEqual(receipt["commands"][-1]["returncode"], 9)
        self.assertIn("counted build failure", (directory / "03-stderr.log").read_text())
        self.assertFalse(self.native_commands())

    def test_input_and_marker_size_budgets_and_redirected_paths_refuse(self):
        with patch.object(subject, "MAX_TOTAL_BYTES", 1), self.assertRaisesRegex(ValueError, "total budget"):
            self.run_build()
        with patch.object(subject, "MAX_INPUT_BYTES", 1), self.assertRaisesRegex(ValueError, "size budget"):
            subject._identity(self.include / "mimalloc/atomic.h", subject.MAX_INPUT_BYTES)
        self.run_build()
        marker = self.output() / "xp-mimalloc-owner.json"
        marker.write_bytes(b"x" * (64 * 1024 + 1))
        with self.assertRaisesRegex(ValueError, "size budget"):
            self.run_build()
        real = Path.lstat
        def alias(path):
            value = real(path)
            if path == self.include.absolute():
                return type("Alias", (), {"st_mode": value.st_mode, "st_file_attributes": 0x400})()
            return value
        with patch.object(Path, "lstat", alias), self.assertRaisesRegex(ValueError, "redirected|alias"):
            self.run_build()

    def test_bad_arch_and_bool_or_out_of_budget_jobs_are_rejected(self):
        for arch, jobs in (("arm64", 1), ("x64", True), ("x64", 0), ("x64", 257)):
            with self.subTest(arch=arch, jobs=jobs), self.assertRaises(ValueError):
                subject.build(self.root, arch, self.tc, self.include, self.receipts["x64"], jobs)
        self.assertFalse(self.calls)


if __name__ == "__main__":
    unittest.main()
