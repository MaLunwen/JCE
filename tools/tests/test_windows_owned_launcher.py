"""Pure Python plan/protocol/source controls; no C compilation or native execution."""
import contextlib
import json
import importlib.util
import io
from pathlib import Path
import subprocess
import types
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/build"))
SPEC = importlib.util.spec_from_file_location("jce_owned_launcher_controls", ROOT / "tools/build/windows_owned_launcher.py")
LAUNCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LAUNCHER)
NATIVE = ROOT / "tools/build/native/windows_owned_launcher.c"


def function(source, name):
    start = source.index("static BOOL " + name + "(") if name != "wmain" else source.index("int wmain(")
    opening = source.index("{", start)
    depth = 1
    for index in range(opening + 1, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if not depth:
            return source[start:index + 1]
    raise AssertionError("unterminated source function")


class OwnedLauncherPlanControls(unittest.TestCase):
    def test_explicit_cli_and_closed_gate_without_native_or_output_mutation(self):
        args = LAUNCHER.build_parser().parse_args([
            "--arch", "x86", "--toolchain-root", str(ROOT / "not-present-msvcrt"),
            "--out", str(ROOT / "build/not-created-owned-launcher")])
        self.assertFalse(args.native_go)
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary) / "absent"
            with mock.patch.object(LAUNCHER.xp_editor, "_preflight", side_effect=AssertionError("native preflight")), \
                 mock.patch.object(LAUNCHER.xp_editor, "_Stages", side_effect=AssertionError("native stage created")):
                plan = LAUNCHER.build(ROOT, args.arch, args.toolchain_root, out)
            self.assertFalse(out.exists())
            self.assertEqual(plan["state"], "PLANNED_NATIVE_GATE_CLOSED")
            self.assertEqual(plan["runtime_execution"], "NOT_EXECUTED")
            self.assertEqual(plan["graphics_execution"], "NOT_EXECUTED")

    def test_architecture_and_serial_explicit_toolchain_commands(self):
        for arch in ("x86", "x64"):
            with self.subTest(arch=arch):
                plan = LAUNCHER.make_plan(ROOT, arch, ROOT / "explicit-msvcrt", ROOT / "build/owned-control")
                self.assertIn("windows-" + arch + "-xp-llvm.cmake", " ".join(plan["commands"][0]))
                self.assertIn("-DJCE_XP_TOOLCHAIN_ROOT:PATH=" + str(ROOT / "explicit-msvcrt"), plan["commands"][0])
                self.assertEqual(plan["commands"][1][-2:], ["--parallel", "1"])
                self.assertEqual(plan["jobs"], 1)
                self.assertNotIn(plan["helper"], [item for command in plan["commands"] for item in command])

    def test_invalid_plan_inputs_refuse_before_touch(self):
        cases = [("arm64", ROOT / "msvcrt", ROOT / "build/owned"),
                 ("x86", Path("relative-msvcrt"), ROOT / "build/owned"),
                 ("x86", ROOT / "msvcrt", Path("relative-out")),
                 ("x86", ROOT / "msvcrt", ROOT),
                 ("x86", ROOT / "msvcrt", ROOT / "tools/build/forbidden-output")]
        for arch, toolchain, out in cases:
            with self.subTest(arch=arch, out=out), self.assertRaises(ValueError):
                LAUNCHER.make_plan(ROOT, arch, toolchain, out)
        for native_go in (0, 1, "yes", None):
            with self.subTest(native_go=native_go), self.assertRaisesRegex(ValueError, "strict boolean"):
                LAUNCHER.build(ROOT, "x86", ROOT / "msvcrt", ROOT / "build/owned", native_go=native_go)

    def test_shared_default_stage_runner_records_hidden_timeout_and_raw_logs(self):
        # Counted Python port for the actual shared stage owner; no native tool runs.
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            receipt = {"stages": []}
            completed = subprocess.CompletedProcess(["declared-tool"], 0, "raw stdout\n", "raw stderr\n")
            with mock.patch.object(LAUNCHER.subprocess, "run", return_value=completed) as execute:
                stages = LAUNCHER.xp_editor._Stages(directory, receipt)
                with contextlib.redirect_stdout(io.StringIO()):
                    stages.run("counted", ["declared-tool"], directory, {}, timeout=3)
            execute.assert_called_once()
            self.assertEqual(execute.call_args.kwargs["timeout"], 3)
            self.assertEqual(execute.call_args.kwargs["creationflags"], getattr(subprocess, "CREATE_NO_WINDOW", 0))
            self.assertTrue(execute.call_args.kwargs["capture_output"])
            self.assertEqual(Path(receipt["stages"][0]["stdout"]["path"]).read_bytes(), b"raw stdout\n")
            self.assertEqual(Path(receipt["stages"][0]["stderr"]["path"]).read_bytes(), b"raw stderr\n")
            self.assertNotIn("--runner-source", LAUNCHER.build_parser().format_help())
            source = Path(LAUNCHER.__file__).read_text(encoding="utf-8")
            self.assertNotIn("trial.py", source)
            self.assertNotIn("exec(", source)

    def test_incremental_build_uses_checked_in_inputs_and_preserves_source_receipts(self):
        # Complete actual Python build flow with counted compiler/audit ports.
        # No C is compiled, and the synthetic artifact is never executed.
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary) / "clone"
            files = ["tools/build/native/windows_owned_launcher.c", "tools/build/native/CMakeLists.txt",
                     "tools/build/windows_owned_launcher.py", "tools/build/xp_editor.py", "tools/build/xp_compat.py",
                     "cmake/toolchains/windows-xp-llvm-common.cmake", "cmake/toolchains/windows-x86-xp-llvm.cmake",
                     "cmake/toolchains/windows-x64-xp-llvm.cmake"]
            for name in files:
                path = repo / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("counted first-party input: " + name + "\n", encoding="utf-8")
            out = repo / "build/cache"
            toolchain = Path(temporary) / "explicit-msvcrt"
            calls = []
            def runner(command, **kwargs):
                calls.append((command, kwargs))
                if "--build" in command:
                    helper = out / "out/jce_windows_owned_launcher.exe"
                    helper.parent.mkdir(parents=True, exist_ok=True)
                    helper.write_bytes(b"synthetic-counted-output")
                return subprocess.CompletedProcess(command, 0, "counted output\n", "")
            def preflight(root, arch, stages, selected_repo, env):
                stages.run("preflight-port", ["counted-compiler"], selected_repo, env, timeout=600)
                return {"compiler": "counted", "root": str(root), "arch": arch}
            with mock.patch.object(LAUNCHER, "os", types.SimpleNamespace(name="nt", environ={})), \
                 mock.patch.object(LAUNCHER.shutil, "which", side_effect=lambda name: "counted-" + name), \
                 mock.patch.object(LAUNCHER.xp_editor, "_preflight", side_effect=preflight), \
                 mock.patch.object(LAUNCHER.xp_editor, "runtime_identity", return_value={"runtime": "counted"}), \
                 mock.patch.object(LAUNCHER.xp_editor, "audit_pe", side_effect=lambda path, *args, **kwargs:
                                   {"sha256": LAUNCHER.xp_editor._sha(path), "state": "SYNTHETIC_CPU_PORT"}), \
                 contextlib.redirect_stdout(io.StringIO()):
                first = LAUNCHER.build(repo, "x86", toolchain, out, native_go=True, runner=runner)
                marker = out / "windows-owned-launcher-owner.json"
                original_marker = marker.read_bytes()
                source = repo / files[0]
                source.write_text(source.read_text(encoding="utf-8") + "edited first-party input\n", encoding="utf-8")
                second = LAUNCHER.build(repo, "x86", toolchain, out, native_go=True, runner=runner)
                self.assertEqual(marker.read_bytes(), original_marker)
                self.assertNotEqual(first["sources"][0]["sha256"], second["sources"][0]["sha256"])
                for receipt in (first, second):
                    self.assertEqual(receipt["state"], "BUILT_AND_STATICALLY_INSPECTED")
                    self.assertEqual(receipt["runtime_execution"], "NOT_EXECUTED")
                self.assertFalse((repo / "build/validation/xp-sync-port-trial/trial.py").exists())
                self.assertEqual(sum("--build" in command for command, _ in calls), 2)
                toolchain_file = repo / files[5]
                toolchain_file.write_text("changed toolchain contract\n", encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "identity differs"):
                    LAUNCHER.build(repo, "x86", toolchain, out, native_go=True, runner=runner)
                self.assertEqual(sum("--build" in command for command, _ in calls), 2)
                self.assertEqual(marker.read_bytes(), original_marker)
            for _, kwargs in calls:
                self.assertEqual(kwargs["env"]["CMAKE_BUILD_PARALLEL_LEVEL"], "1")

    def test_cache_refuses_arch_runtime_toolchain_source_root_and_foreign_owners(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary) / "cache"
            owner = LAUNCHER._cache_owner(ROOT, "x86", {"compiler": "a"}, {"runtime": "a"}, [])
            LAUNCHER._guard_cache(out, owner)
            LAUNCHER._guard_cache(out, owner)
            mutations = [{"arch": "x64"}, {"toolchain": {"compiler": "b"}},
                         {"runtime": {"runtime": "b"}}, {"source_root": str(ROOT / "other-source")}]
            for mutation in mutations:
                with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, "identity differs"):
                    LAUNCHER._guard_cache(out, {**owner, **mutation})
            foreign = Path(temporary) / "foreign"
            foreign.mkdir()
            (foreign / "untouched").write_bytes(b"original")
            with self.assertRaisesRegex(ValueError, "nonempty unowned"):
                LAUNCHER._guard_cache(foreign, owner)
            self.assertEqual((foreign / "untouched").read_bytes(), b"original")

    def test_legacy_cache_admission_requires_observed_source_root_and_toolchain_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary) / "cache"
            out.mkdir()
            old_sources = [{"path": "tools\\build\\native\\windows_owned_launcher.c", "sha256": "a" * 64},
                           {"path": "cmake\\toolchains\\windows-x86-xp-llvm.cmake", "sha256": "b" * 64}]
            new_sources = [{"path": "tools/build/native/windows_owned_launcher.c", "sha256": "c" * 64},
                           {"path": "cmake/toolchains/windows-x86-xp-llvm.cmake", "sha256": "b" * 64}]
            old = {"schema": "jce.windows.owned-launch-cache.v1", "arch": "x86", "toolchain": {},
                   "runtime": {}, "sources": old_sources, "jobs": 1}
            marker = out / "windows-owned-launcher-owner.json"
            LAUNCHER.xp_editor._write(marker, old)
            expected = LAUNCHER._cache_owner(ROOT, "x86", {}, {}, new_sources)
            cache = out / "CMakeCache.txt"
            cache.write_text("CMAKE_HOME_DIRECTORY:INTERNAL=" + (ROOT / "wrong-source").as_posix() + "\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "source root differs"):
                LAUNCHER._guard_cache(out, expected)
            self.assertEqual(json.loads(marker.read_text(encoding="utf-8")), old)
            cache.write_text("CMAKE_HOME_DIRECTORY:INTERNAL=" + (ROOT / LAUNCHER.SOURCE).as_posix() + "\n", encoding="utf-8")
            LAUNCHER._guard_cache(out, expected)
            self.assertEqual(json.loads(marker.read_text(encoding="utf-8")), expected)

class OwnedLauncherSourceControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = NATIVE.read_text(encoding="utf-8")

    def test_exact_executable_desktop_and_owned_raw_stream_launch_contract(self):
        main = function(self.source, "wmain")
        self.assertRegex(main, r"CreateProcessW\(options\.target, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,\s*NULL, options\.workdir, &startup, &process\)")
        self.assertIn("startup.lpDesktop = (WCHAR *)options.desktop", main)
        self.assertIn("STARTF_USESTDHANDLES | STARTF_FORCEOFFFEEDBACK", main)
        self.assertIn("startup.hStdOutput = streams.out; startup.hStdError = streams.err", main)
        self.assertEqual(main.count("CREATE_NEW"), 3)
        self.assertLess(main.index("save_standard_flags(old"), main.index("CreateProcessW("))
        for excluded in ("ShellExecute", "CreateProcessAsUser", "SetForegroundWindow", "ShowWindow", "SetWindowPos",
                         "SetTokenInformation", "AdjustTokenPrivileges", "GetInputDesktop", "taskkill", "JCE_ACK"):
            self.assertNotIn(excluded, self.source)
        self.assertNotRegex(self.source, r"\bOpenProcess\s*\(")

    def test_held_child_lifecycle_queries_reaps_and_closes_same_handles(self):
        body = function(self.source, "retire_child")
        self.assertEqual(body.count("ops->times(process,"), 2)
        self.assertEqual(body.count("ops->close(thread)"), 1)
        self.assertEqual(body.count("ops->close(process)"), 1)
        self.assertEqual(body.count("ops->terminate(process, 92)"), 1)
        self.assertIn("ops->wait(process, CLEANUP_LIMIT_MS)", body)
        self.assertIn("same_time(life->birth, after)", body)
        self.assertIn("life->exit_known = ops->exit_code(process, &life->child_exit)", body)
        self.assertNotIn("life->child_exit == 0", body)
        self.assertIn("process.dwProcessId", function(self.source, "wmain"))
        self.assertIn("return error || !created || !life.ok ? 1 : 0", self.source)

    def test_frozen_json_keys_and_output_limit_are_explicit(self):
        fields = ("schema nonce target_path working_dir requested_desktop controller_error child created pid birth_low birth_high "
                  "birth_known birth_stable first_wait final_wait exit_known exit_code timed_out terminate_attempted terminate_ok "
                  "reaped thread_closed process_closed streams stdout_name stderr_name stdout_bytes stderr_bytes stdout_closed "
                  "stderr_closed output_limit_exceeded output_limit_bytes output_poll_ms byte_limit_policy context user_sid "
                  "token_type session_id query_error").split()
        for field in fields:
            with self.subTest(field=field):
                self.assertIn('\\"' + field + '\\"', self.source)
        self.assertIn(LAUNCHER.SCHEMA, self.source)
        self.assertIn("observed-periodic-file-size; overshoot possible", self.source)
        self.assertIn("#define OUTPUT_LIMIT (32ULL * 1024ULL * 1024ULL)", self.source)
        self.assertIn("#define OUTPUT_POLL_MS 25U", self.source)
        self.assertIn("#define CLEANUP_LIMIT_MS 5000U", self.source)
        for name in ("jce-child-%ls.stdout.bin", "jce-child-%ls.stderr.bin", "jce-launch-%ls.json"):
            self.assertIn(name, self.source)

    def test_optional_context_is_queried_from_child_token(self):
        self.assertIn("OpenProcessToken(process, TOKEN_QUERY, &token)", self.source)
        for information in ("TokenUser", "TokenType", "TokenSessionId"):
            self.assertIn("GetTokenInformation(token, " + information, self.source)
        self.assertIn("observe_context(process.hProcess, &context)", self.source)
        self.assertEqual(self.source.count('else append("null")'), 3)

    def test_c99_static_system_runtime_recipe_and_owned_text_caps(self):
        recipe = (NATIVE.parent / "CMakeLists.txt").read_text(encoding="utf-8")
        for token in ("C_STANDARD 99", "C_EXTENSIONS OFF", "-static -municode", "JCE_XP_PE_MINOR", "PRIVATE advapi32"):
            self.assertIn(token, recipe)
        for path in (NATIVE, NATIVE.parent / "CMakeLists.txt", ROOT / "tools/build/windows_owned_launcher.py", Path(__file__)):
            raw = path.read_bytes()
            self.assertNotIn(b"\r", raw)
            self.assertFalse(raw.startswith(b"\xef\xbb\xbf"))
        self.assertLess(len(self.source.splitlines()), 400)


if __name__ == "__main__":
    unittest.main()
