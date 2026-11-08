"""Actual singleton scanner, working files and Git inventory CPU controls."""
from contextlib import contextmanager, redirect_stderr, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
GATE_PATH = ROOT / "tools/audit/check_singleton_ownership.py"
INVENTORY_PATH = ROOT / "tools/lint/lint_git_files.py"
ALIASES = {
    "core/jce_build_manager.cpp": ("WorkerLogSink", "DeferredLogEntry"),
    "panels/jce_panel_benchmark.cpp": ("Baseline",),
    "panels/jce_panel_memory_profiler.cpp": ("TagRow",),
    "panels/jce_panel_profile_analyzer.cpp": ("FrameRecord",),
    "panels/jce_panel_test_runner.cpp": ("CaseStatus",),
}


def load_module(path=GATE_PATH, transform=None):
    spec = importlib.util.spec_from_file_location("singleton_control", path)
    module = importlib.util.module_from_spec(spec)
    if transform is None:
        spec.loader.exec_module(module)
    else:
        exec(compile(transform(path.read_text(encoding="utf-8")), str(path), "exec"),
             module.__dict__)
    return module


def run_gate(gate, *args):
    stdout, stderr = io.StringIO(), io.StringIO()
    with patch.object(sys, "argv", [str(GATE_PATH), *map(str, args)]), \
            redirect_stdout(stdout), redirect_stderr(stderr):
        result = gate.main()
    return result, stdout.getvalue(), stderr.getvalue()


@contextmanager
def fixture_root(gate):
    with tempfile.TemporaryDirectory(prefix="jce-singleton-") as directory:
        root = Path(directory)
        with patch.object(gate, "REPO_ROOT", root), \
                patch.object(gate._lgf, "REPO_ROOT", root):
            yield root


def write(root, relative, source):
    target = root / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(source, encoding="utf-8")
    return target


def source_aliases():
    """Consume the actual six declarations whose aliases inflated the count."""
    declarations = []
    for relative, names in ALIASES.items():
        lines = (ROOT / "editor/src" / relative).read_text(encoding="utf-8").splitlines()
        for name in names:
            declarations.append(next(line for line in lines
                                     if line.startswith(f"using {name} =")))
    return "\n".join(declarations)


class SingletonClassifierControls(unittest.TestCase):
    def assert_classifier(self, gate):
        with fixture_root(gate) as root:
            relative = "editor/src/core/aliases.cpp"
            write(root, relative, "namespace {\n" + source_aliases() + "\n" +
                  "using Callback = void (*)(void);\n"
                  "typedef void (*LegacyCallback)(void);\n"
                  "static Callback s_callback;\n"
                  "LegacyCallback active_callback;\n"
                  "int active_state = 0;\n"
                  "static int *s_pointer;\n"
                  "std::vector<int *> pending_requests;\n"
                  "static const int s_constant = 1;\n"
                  "constexpr int s_other_constant = 2;\n"
                  "static void helper(void) {}\n"
                  "}\n")
            with patch.object(gate._lgf, "working_tree", return_value=[relative]) as files:
                found = gate.scan()
                files.assert_called_once_with(strict=True)
                self.assertEqual({item["name"] for item in found},
                                 {"s_callback", "active_callback", "active_state",
                                  "s_pointer", "pending_requests"})
                self.assertTrue(all(item["static"] and not item["extern"] for item in found))
                result, output, error = run_gate(gate, "--json")
                self.assertEqual(result, 0, error)
                self.assertEqual(json.loads(output), found)
                baseline = write(root, "baseline.txt", "4\n")
                result, output, error = run_gate(gate, "--check", "--baseline", baseline)
                self.assertEqual(result, 1, error)
                self.assertIn("5 globals > baseline 4", output)
                self.assertEqual(baseline.read_text(encoding="utf-8"), "4\n")

    def test_real_aliases_exclude_types_but_retain_anonymous_mutable_callback_state(self):
        self.assert_classifier(load_module())

    def test_omitting_only_alias_classifier_fails_the_same_ordinary_count_assertion(self):
        old = "if is_cpp and USING_ALIAS_RE.match(code):"
        def omit_classifier(source):
            self.assertEqual(source.count(old), 1)
            return source.replace(old, "if False and is_cpp and USING_ALIAS_RE.match(code):", 1)
        mutated = load_module(transform=omit_classifier)
        with self.assertRaises(AssertionError):
            self.assert_classifier(mutated)

    def test_c_static_pointer_and_aggregate_rules_are_preserved(self):
        gate = load_module()
        with fixture_root(gate) as root:
            relative = "engine/src/core/state.c"
            write(root, relative,
                  "typedef void (*Callback)(void);\n"
                  "typedef int using;\n"
                  "using s_using_value = 0;\n"
                  "static Callback s_callback;\n"
                  "static int *s_pointer;\n"
                  "static struct State {\n    int value;\n} s_state;\n"
                  "static const int s_constant = 1;\n"
                  "void helper(void) {\n    static int local_state;\n}\n")
            with patch.object(gate._lgf, "working_tree", return_value=[relative]):
                self.assertEqual({item["name"] for item in gate.scan()},
                                 {"s_callback", "s_pointer", "s_state", "s_using_value"})


class SingletonWorkingTreeControls(unittest.TestCase):
    def prepare_repository(self, root):
        subprocess.run(["git", "init", "-q", str(root)], check=True, capture_output=True)
        write(root, "engine/src/core/tracked.c", "static int s_tracked;\n")
        deleted = write(root, "engine/src/core/deleted.c", "static int s_deleted;\n")
        # Only this fresh temporary repository's index is populated.
        subprocess.run(["git", "-C", str(root), "add", "--", "engine"],
                       check=True, capture_output=True)
        deleted.unlink()
        write(root, "editor/src/core/new owner.cpp", "namespace {\nint active_state = 0;\n}\n")
        write(root, ".gitignore", "engine/src/ignored/\n")
        for relative in ("engine/src/ignored/copy.c", "engine/src/third_party/copy.c",
                         "editor/src/generated/copy.cpp", "engine/src/core/stb_copy.c",
                         "engine/src/core/cJSON_copy.c", "tools/copy.c", "engine/src/core/copy.h"):
            write(root, relative, "static int s_excluded;\n")

    def assert_current_sources(self, gate, root):
        found = gate.scan()
        self.assertEqual({item["name"] for item in found}, {"s_tracked", "active_state"})
        self.assertEqual({item["file"] for item in found},
                         {"engine/src/core/tracked.c", "editor/src/core/new owner.cpp"})
        baseline = write(root, "baseline.txt", "1\n")
        result, output, error = run_gate(gate, "--check", "--baseline", baseline)
        self.assertEqual(result, 1, error)
        self.assertIn("2 globals > baseline 1", output)
        self.assertEqual(baseline.read_text(encoding="utf-8"), "1\n")

    def test_real_git_counts_untracked_owner_skips_only_declared_deletion_and_exclusions(self):
        gate = load_module()
        with fixture_root(gate) as root:
            self.prepare_repository(root)
            original_run = subprocess.run
            calls = []
            def counted_git(args, **kwargs):
                calls.append((args, kwargs))
                return original_run(args, **kwargs)
            with patch.object(gate._lgf.subprocess, "run", side_effect=counted_git):
                self.assert_current_sources(gate, root)
            self.assertEqual(len(calls), 4)  # scan, then the CLI: two Git commands each.
            for args, kwargs in calls:
                self.assertEqual(kwargs["cwd"], root)
                self.assertIn("-z", args)
                if "--deleted" not in args:
                    self.assertTrue({"--cached", "--others", "--exclude-standard"} <= set(args))

    def test_missing_untracked_inventory_wire_fails_same_ordinary_count_assertion(self):
        gate = load_module()
        def omit_untracked(source):
            old = '"--cached", "--others", "--exclude-standard"'
            self.assertEqual(source.count(old), 1)
            return source.replace(old, '"--cached", "--exclude-standard"', 1)
        gate._lgf = load_module(INVENTORY_PATH, omit_untracked)
        with fixture_root(gate) as root:
            self.prepare_repository(root)
            with self.assertRaises(AssertionError):
                self.assert_current_sources(gate, root)

    def test_counted_git_port_keeps_nul_paths_and_deduplicates_sources(self):
        gate = load_module()
        with fixture_root(gate) as root:
            relative = "engine/src/core/new owner.c"
            write(root, relative, "static int s_owned;\n")
            candidates = [relative, relative, "engine/src/core/deleted.c",
                          "engine/src/third_party/vendor.c", "editor/src/generated/copy.cpp",
                          "engine/src/core/stb_copy.c", "tools/copy.c", "engine/src/core/copy.h"]
            calls = []
            def git_port(args, **kwargs):
                calls.append(args)
                self.assertEqual(kwargs["cwd"], root)
                self.assertEqual(kwargs["encoding"], "utf-8")
                self.assertIn("-z", args)
                paths = ["engine/src/core/deleted.c"] if "--deleted" in args else candidates
                return subprocess.CompletedProcess(args, 0, "\0".join(paths) + "\0", "")
            with patch.object(gate._lgf.subprocess, "run", side_effect=git_port):
                self.assertEqual([item["name"] for item in gate.scan()], ["s_owned"])
            self.assertEqual(calls, [["git", "ls-files", "--deleted", "-z"],
                                    ["git", "ls-files", "--cached", "--others",
                                     "--exclude-standard", "-z"]])

    def test_each_failed_git_command_refuses_instead_of_reporting_empty_clean_state(self):
        gate = load_module()
        for failure in ("--deleted", "--cached"):
            with self.subTest(failure=failure), fixture_root(gate):
                def git_port(args, **kwargs):
                    return subprocess.CompletedProcess(args, 128 if failure in args else 0,
                                                       "", "fixture Git refusal")
                with patch.object(gate._lgf.subprocess, "run", side_effect=git_port):
                    result, output, error = run_gate(gate, "--json")
                self.assertEqual(result, 2)
                self.assertEqual(output, "")
                self.assertIn("fixture Git refusal", error)

    def test_launch_error_and_missing_or_unreadable_admitted_source_refuse(self):
        gate = load_module()
        with fixture_root(gate) as root:
            with patch.object(gate._lgf.subprocess, "run", side_effect=FileNotFoundError("no git")):
                self.assertEqual(run_gate(gate)[0], 2)
            relative = "engine/src/core/admitted.c"
            with patch.object(gate._lgf, "working_tree", return_value=[relative]):
                result, output, error = run_gate(gate)
                self.assertEqual((result, output), (2, ""))
                self.assertIn(relative, error)
                write(root, relative, "static int s_admitted;\n")
                with patch.object(Path, "read_text", side_effect=PermissionError("fixture read refusal")):
                    result, output, error = run_gate(gate)
                self.assertEqual((result, output), (2, ""))
                self.assertIn("fixture read refusal", error)


if __name__ == "__main__":
    unittest.main()
