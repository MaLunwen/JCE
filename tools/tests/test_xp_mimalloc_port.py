"""Pure first-party XP mimalloc port controls; never launches a compiler.

The isolated C99 fixture consumes complete production headers and unchanged
mimalloc/atomic.h. These source/preparation tests do not certify concurrency,
generated instructions/imports, native execution or XP guest execution.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[2]
SYNC = Path("engine/src/os/platform/compatibility/jce_windows_xp_sync_internal.h")
PORT = SYNC.with_name("jce_windows_xp_mimalloc_internal.h")
FIXTURE = Path("tests/compatibility/xp_mimalloc/main.c")
CASES = ("locks", "concurrency", "exports-present", "exports-missing",
         "kernel-missing", "ordinals", "forwarding")
FAIL_STOP_CASES = ("fail-current", "fail-lock", "fail-release")
MUTATION = ("if (jce_xp_sync_cas(&lock->Ptr, &expected, 1)) return TRUE;",
            "(void)lock; (void)expected; return TRUE;")
# Exact function-body SHA256 values from the approved frozen first-party trial:
# sync_port.h SHA256 4b9e0a5b0e2e23ac656e3318833c59bb3a27190053d842450d31a85bc84c984d.
FROZEN_BODIES = {
    "jce_xp_sync_fatal": "df7daca6a5e7aab6e6fa05bb4e971eb18944318120e0d38ab9558f2fc3b3c17a",
    "jce_xp_sync_load": "7770ac5553773044c0a8454e2abea7a3d4d658dd53369d7ea6cfa56bce045863",
    "jce_xp_sync_store": "0f2688208e6fa903ae0a2fb61a65d5593f594411dc0ab456124088061938c10e",
    "jce_xp_sync_cas": "71828cd836e030958c97655b2519ee3a914bbf752beb3b33cfc30212905c33e8",
    "jce_xp_sync_backoff": "db2aaae13052587534cd1709f919ea5e26fcf979ab3785102cf7542c7428cca9",
    "jce_xp_InitializeSRWLock": "af37c44cc342f3cf9f7ad429418622c5b4def68e9bd571acd1f5baaf9e35f09b",
    "jce_xp_TryAcquireSRWLockExclusive": "bf7d68b2eabf627d018e5f46445e61e79dba72f4adacaecc40f6f7c4e1357d9d",
    "jce_xp_AcquireSRWLockExclusive": "879cfc5f1e9caffc7217aa82b83faed183e84450aff08b7cdd433b681bcbe314",
    "jce_xp_ReleaseSRWLockExclusive": "3da194b70aa48d7bc17ab600681a40486918237e188e26800f063e8cd7ff515a",
}
NUMA_NAMES = {"GetNumaProcessorNodeEx", "GetNumaNodeProcessorMaskEx",
              "GetNumaProcessorNode", "GetNumaNodeProcessorMask", "GetNumaHighestNodeNumber"}


def function_body(source, name):
    found = re.search(r"\b" + re.escape(name) + r"\([^{};]*\)\s*\{", source)
    if not found:
        raise AssertionError("actual function missing: " + name)
    start, depth = found.end(), 1
    for offset in range(start, len(source)):
        depth += (source[offset] == "{") - (source[offset] == "}")
        if not depth:
            return source[start:offset]
    raise AssertionError("unterminated actual function: " + name)


def check_frozen_algorithms(source):
    for name, expected in FROZEN_BODIES.items():
        actual = hashlib.sha256(function_body(source, name).encode("ascii")).hexdigest()
        assert actual == expected, "frozen pointer-typed algorithm changed: " + name


def _sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_mutants(output, *, repo=ROOT):
    """Prepare complete first-party headers only in fresh build/validation work."""
    repo = Path(repo).resolve()
    output = Path(output).absolute()
    for path in (output, *output.parents):
        if path.exists() and (path.is_symlink() or getattr(path.lstat(), "st_file_attributes", 0) & 0x400):
            raise ValueError("fixture output may not traverse a reparse/link path")
    output = output.resolve()
    owned = repo / "build/validation"
    if output == owned or not output.is_relative_to(owned) or output.exists():
        raise ValueError("fixture output must be a fresh strict child of build/validation/")
    sync = (repo / SYNC).read_text(encoding="ascii")
    port = (repo / PORT).read_text(encoding="ascii")
    check_frozen_algorithms(sync)
    before, after = MUTATION
    if sync.count(before) != 1:
        raise ValueError("omitted-try mutation anchor changed")
    mutated = sync.replace(before, after, 1)
    directory = output / "omitted_try"
    directory.mkdir(parents=True, exist_ok=False)
    sync_path, port_path = directory / SYNC.name, directory / PORT.name
    sync_path.write_text(mutated, encoding="ascii", newline="\n")
    port_path.write_text(port, encoding="ascii", newline="\n")
    manifest = {
        "schema": "jce.xp.mimalloc-port-controls.v1",
        "sync_sha256": _sha(repo / SYNC), "port_sha256": _sha(repo / PORT),
        "fixture_sha256": _sha(repo / FIXTURE),
        "cases": list(CASES), "fail_stop_cases": list(FAIL_STOP_CASES),
        "fail_stop_exit": 91, "native_execution": "NOT_EXECUTED",
        "scope": "source and typed CPU controls; no graphical or full allocator acceptance",
        "mutants": {"omitted_try": {
            "path": str(port_path), "sync_path": str(sync_path),
            "port_sha256": _sha(port_path), "sync_sha256": _sha(sync_path),
            "case": "locks", "expected_exit": 1,
            "marker": "JCE_XP_MIMALLOC_ASSERT_FAILED:try-publishes-held-state",
        }},
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                        encoding="ascii", newline="\n")
    return manifest


class XpMimallocPortTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sync = (ROOT / SYNC).read_text(encoding="ascii")
        cls.port = (ROOT / PORT).read_text(encoding="ascii")
        cls.fixture = (ROOT / FIXTURE).read_text(encoding="ascii")
        cls.cmake = (ROOT / FIXTURE.parent / "CMakeLists.txt").read_text(encoding="ascii")
        # Keep small generated controls for later native review; no recursive delete.
        cls.work = ROOT / "build/validation" / ("xp-mimalloc-pure-" + uuid.uuid4().hex)
        cls.work.mkdir(parents=True, exist_ok=False)

    def test_exact_frozen_pointer_typed_algorithms(self):
        check_frozen_algorithms(self.sync)
        for name in ("jce_xp_sync_load", "jce_xp_sync_store", "jce_xp_sync_cas"):
            self.assertRegex(self.sync, r"\b" + name + r"\(PVOID \*word")
        self.assertNotIn("jce_native_atomic_", self.sync)
        self.assertNotIn("size_t", self.sync)

    def test_explicit_compiler_arch_floor_layout_alignment_and_lockfree(self):
        for text in ("JCE_WINDOWS_XP_SYNC_PORT != 1", "!defined(_WIN32)",
                     "!defined(__clang__)", "!defined(__MINGW32__)", "defined(_MSC_VER)",
                     "defined(__x86_64__)", "defined(__i386__)",
                     "_WIN32_WINNT != 0x0502", "WINVER != 0x0502",
                     "_WIN32_WINNT != 0x0501", "WINVER != 0x0501",
                     "__atomic_always_lock_free(sizeof(PVOID), 0)",
                     "offsetof(SRWLOCK, Ptr) == 0",
                     "__alignof__(SRWLOCK) >= __alignof__(PVOID)",
                     "sizeof(uintptr_t) == sizeof(PVOID)"):
            self.assertIn(text, self.sync)
        self.assertIn("JCE_WINDOWS_XP_MIMALLOC_PORT != 1", self.port)

    def test_common_header_is_only_exclusive_srw_and_has_no_aliases(self):
        for name in ("CONDITION_VARIABLE", "INIT_ONCE", "GetThreadGroupAffinity",
                     "CreateEvent", "WakeCondition", "SleepCondition"):
            self.assertNotIn(name, self.sync)
        self.assertNotRegex(self.sync, r"#define (InitializeSRW|TryAcquireSRW|AcquireSRW|ReleaseSRW)")
        for text in ("TerminateProcess(GetCurrentProcess(), 91);", "for (;;) Sleep(INFINITE);"):
            self.assertIn(text, function_body(self.sync, "jce_xp_sync_fatal"))

    def test_only_five_exact_optional_numa_names_are_refused(self):
        body = function_body(self.port, "jce_xp_mimalloc_is_numa_probe")
        self.assertEqual(set(re.findall(r'strcmp\(name, "([^"]+)"\)', body)), NUMA_NAMES)
        self.assertLess(body.index("(uintptr_t)name <= UINT16_MAX"), body.index("strcmp("))
        lookup = function_body(self.port, "jce_xp_mimalloc_GetProcAddress")
        self.assertIn("jce_xp_mimalloc_is_numa_probe(name) && !jce_xp_mimalloc_current_processor_export()", lookup)
        self.assertIn("return JCE_XP_MIMALLOC_GET_PROC_ADDRESS(module, name);", lookup)

    def test_cpu_export_is_real_borrowed_uncached_and_missing_fails_closed(self):
        probe = function_body(self.port, "jce_xp_mimalloc_current_processor_export")
        self.assertIn('JCE_XP_MIMALLOC_GET_MODULE_HANDLE_W(L"kernel32.dll")', probe)
        self.assertIn('JCE_XP_MIMALLOC_GET_PROC_ADDRESS(kernel32, "GetCurrentProcessorNumber")', probe)
        cpu = function_body(self.port, "jce_xp_mimalloc_GetCurrentProcessorNumber")
        self.assertIn("if (!query) jce_xp_sync_fatal();", cpu)
        self.assertIn("return query();", cpu)
        self.assertNotIn("return 0", cpu)
        for source in (self.sync, self.port):
            self.assertNotRegex(source, r"\b(?:LoadLibrary\w*|FreeLibrary|malloc|calloc|realloc|free)\s*\(")
            self.assertNotRegex(source, r"static\s+(?!inline)")

    def test_consumer_aliases_follow_all_native_helper_definitions(self):
        aliases = re.findall(r"^#define (\w+) (jce_xp_\w+)$", self.port, re.M)
        self.assertEqual({name for name, _value in aliases}, {
            "InitializeSRWLock", "TryAcquireSRWLockExclusive", "AcquireSRWLockExclusive",
            "ReleaseSRWLockExclusive", "GetCurrentProcessorNumber", "GetProcAddress"})
        first_alias = self.port.index("#define InitializeSRWLock ")
        self.assertLess(self.port.index('#include "jce_windows_xp_sync_internal.h"'), first_alias)
        for name in ("jce_xp_mimalloc_current_processor_export", "jce_xp_mimalloc_is_numa_probe",
                     "jce_xp_mimalloc_GetProcAddress", "jce_xp_mimalloc_GetCurrentProcessorNumber"):
            self.assertLess(self.port.index(function_body(self.port, name)), first_alias)
        self.assertGreater(self.port.index("#define GetProcAddress "), self.port.index("return query();"))

    def test_complete_c99_fixture_and_explicit_build_inputs(self):
        self.assertIn("#include JCE_XP_MIMALLOC_PORT_HEADER", self.fixture)
        self.assertIn("#include <mimalloc/atomic.h>", self.fixture)
        for name in (*CASES, *FAIL_STOP_CASES):
            self.assertIn('"' + name + '"', self.fixture)
        self.assertIn("CreateThread(NULL, 0, fixture_worker, &run, 0, NULL)", self.fixture)
        self.assertIn("run.counter == THREAD_COUNT * ITERATIONS", self.fixture)
        self.assertIn('CHECK("try-publishes-held-state"', self.fixture)
        self.assertLess(self.fixture.index('CHECK("try-publishes-held-state"'),
                        self.fixture.index("static int concurrency_case"))
        for text in ("CMAKE_TOOLCHAIN_FILE", "JCE_XP_MIMALLOC_INCLUDE", "JCE_XP_MIMALLOC_OUTPUT",
                     "CMAKE_C_STANDARD 99", "CMAKE_C_EXTENSIONS OFF",
                     "JCE_XP_MIMALLOC_OMITTED_TRY_CONTROL=1"):
            self.assertIn(text, self.cmake)
        self.assertNotIn("WILL_FAIL", self.cmake.split("# fail-current", 1)[0].split(
            "set_tests_properties(xp_mimalloc_omitted_try PROPERTIES WILL_FAIL TRUE)", 1)[-1])

    def test_guest_defaults_aggregate_existing_cases_and_flush_fatal_markers(self):
        aggregate = function_body(self.fixture, "all_case")
        for call in ("lock_case()", "concurrency_case()", "exports_case(TRUE, TRUE)",
                     "exports_case(FALSE, TRUE)", "exports_case(TRUE, FALSE)",
                     "ordinal_case()", "forwarding_case()"):
            self.assertIn(call, aggregate)
        self.assertIn('const char *name = argc == 2 ? argv[1] : "all";', self.fixture)
        self.assertIn('const char *name = argc == 2 ? argv[1] : "locks";', self.fixture)
        marker = function_body(self.fixture, "fixture_fail_stop_marker")
        self.assertIn("JCE_XP_MIMALLOC_FAIL_STOP:%s", marker)
        self.assertIn("fflush(stdout);", marker)
        self.assertEqual(self.fixture.count("fixture_fail_stop_marker(name);"), 3)

    def test_fresh_complete_header_control_and_ordinary_failure_marker(self):
        before = {_path: _sha(ROOT / _path) for _path in (SYNC, PORT, FIXTURE)}
        manifest = write_mutants(self.work / "prepared")
        row = manifest["mutants"]["omitted_try"]
        mutated = Path(row["sync_path"]).read_text(encoding="ascii")
        self.assertIn(MUTATION[1], function_body(mutated, "jce_xp_TryAcquireSRWLockExclusive"))
        with self.assertRaises(AssertionError):
            check_frozen_algorithms(mutated)
        self.assertEqual(Path(row["path"]).read_bytes(), (ROOT / PORT).read_bytes())
        self.assertEqual(before, {_path: _sha(ROOT / _path) for _path in before})
        self.assertEqual(row["expected_exit"], 1)
        self.assertEqual(row["marker"], "JCE_XP_MIMALLOC_ASSERT_FAILED:try-publishes-held-state")
        self.assertEqual(manifest["native_execution"], "NOT_EXECUTED")
        self.assertEqual(manifest["fail_stop_exit"], 91)

    def test_existing_or_outside_validation_outputs_are_refused(self):
        for output in (self.work, ROOT / "build/validation", ROOT / "build/xp-mimalloc-refused",
                       ROOT / "engine/src/os/platform/compatibility", ROOT.parent / "xp-mimalloc-refused"):
            with self.subTest(output=output), self.assertRaises(ValueError):
                write_mutants(output)

    def test_relaxed_cas_and_omitted_try_mutants_fail_source_assertions(self):
        for old, new in ((MUTATION[0], MUTATION[1]),
                         ("__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE", "__ATOMIC_RELAXED, __ATOMIC_RELAXED"),
                         ("__ATOMIC_RELEASE", "__ATOMIC_RELAXED")):
            with self.subTest(old=old):
                self.assertIn(old, self.sync)
                with self.assertRaises(AssertionError):
                    check_frozen_algorithms(self.sync.replace(old, new, 1))

    def test_owned_code_ascii_lf_and_final_newline(self):
        for path in (SYNC, PORT, FIXTURE, FIXTURE.parent / "CMakeLists.txt",
                     Path(__file__).resolve().relative_to(ROOT)):
            with self.subTest(path=path):
                data = (ROOT / path).read_bytes()
                data.decode("ascii")
                self.assertNotIn(b"\r", data)
                self.assertTrue(data.endswith(b"\n"))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--prepare-controls":
        parser = argparse.ArgumentParser()
        parser.add_argument("--prepare-controls", required=True, type=Path)
        args = parser.parse_args()
        print(json.dumps(write_mutants(args.prepare_controls), indent=2))
    else:
        unittest.main()
