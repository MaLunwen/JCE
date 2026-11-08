"""Pure lexical/dispatch controls; no bgfx runtime or GPU certification."""
from contextlib import contextmanager, redirect_stderr, redirect_stdout
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lint import check_bgfx_view_name_length as guard
from tools.lint import check_authored_path_consumed as authored
from tools.lint import lint_git_files as inventory


class ViewNameLengthTests(unittest.TestCase):
    def test_accepts_sentinel_nonnegative_and_nested_arguments(self):
        source = '\n'.join([
            'bgfx_set_view_name((uint16_t)(base+1), "UI, named", INT32_MAX);',
            'bgfx_set_view_name(view, name, (int32_t)(0));',
            'bgfx_set_view_name(view, choose(a, b), 0x7fffffffUL);',
            '#define LABEL(v,n) bgfx_set_view_name((v),(n),32u)',
        ])
        result = guard.scan_source(source)
        self.assertEqual(result['calls'], 4)
        self.assertEqual(result['literal_checked'], 4)
        self.assertEqual(result['findings'], [])
        self.assertEqual(result['unresolved'], [])

    def test_rejects_real_missing_or_empty_length(self):
        for call in ('bgfx_set_view_name(id, "Splash");',
                     'bgfx_set_view_name(id, "Splash", /* absent */);'):
            with self.subTest(call=call):
                result = guard.scan_source(call)
                self.assertEqual(len(result['findings']), 1)
                self.assertEqual(result['literal_checked'], 0)

    def test_negative_literal_mutation_cannot_pass(self):
        actual = 'bgfx_set_view_name(id, "UI/ImGui", INT32_MAX);'
        self.assertEqual(guard.scan_source(actual)['findings'], [])
        for bad in ('-1', '- 1', '(- /* guard */ 1)', '(int32_t)(-1)', '-0x1L', '2147483648'):
            with self.subTest(bad=bad):
                mutated = actual.replace('INT32_MAX', bad)
                result = guard.scan_source(mutated)
                self.assertEqual(result['calls'], 1)
                self.assertEqual(len(result['findings']), 1)
                self.assertEqual(result['unresolved'], [])

    def test_prose_raw_strings_and_delimiters_do_not_create_calls(self):
        source = '\n'.join([
            '// bgfx_set_view_name(id,name,-1);',
            '/* bgfx_set_view_name(id,name); */',
            'const char *s="bgfx_set_view_name(id,name,-1); \\" quote";',
            'const char *r=R"TAG(quote \" bgfx_set_view_name(id,name,-1);)TAG";',
            '// continued\\',
            'bgfx_set_view_name(id,name,-1);',
            'bgfx_set_view_name(get_id((a+b)), "(),[]{}", (12));',
        ])
        result = guard.scan_source(source)
        self.assertEqual(result['calls'], 1)
        self.assertEqual(result['literal_checked'], 1)
        self.assertEqual(result['findings'], [])

    def test_dynamic_lengths_remain_unresolved_without_false_rejection(self):
        source = '\n'.join(
            f'bgfx_set_view_name(id,name,{expr});'
            for expr in ('length', '(int32_t)strlen(name)', 'sizeof(name)-1',
                         'enabled ? sizes[slot] : 0', 'choose<A,B>(name)',
                         '(uint32_t)-1', '(int8_t)255'))
        result = guard.scan_source(source)
        self.assertEqual(result['calls'], 7)
        self.assertEqual(result['findings'], [])
        self.assertEqual(result['literal_checked'], 0)
        self.assertEqual(len(result['unresolved']), 7)

    def test_inventory_failure_and_unbalanced_call_are_not_clean(self):
        self.assertEqual(guard.first_party_sources([
            'engine/src/new.c','editor/src/ui/view.cpp','build/copied.c',
            'engine/src/third_party/upstream.c','tools/tests/control.py']),
            ['editor/src/ui/view.cpp','engine/src/new.c'])
        self.assertEqual(len(guard.scan_source('bgfx_set_view_name(id,name,-1;')['findings']), 1)
        with patch.object(guard, 'working_tree', return_value=[]), redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
            self.assertEqual(guard.main(), 1)


@contextmanager
def moved_owner_repository():
    """Tiny real Git index; never stages or changes the product repository."""
    with tempfile.TemporaryDirectory(prefix="jce-source-inventory-") as directory:
        root = Path(directory)
        subprocess.run(["git", "init", "-q", str(root)], check=True, capture_output=True)
        old = root / "engine/src/os/platform/jce_jni_bridge.c"
        current = root / "engine/src/application/jce_jni_bridge.c"
        old.parent.mkdir(parents=True)
        old.write_text("void old_owner(void) {}\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(root), "add", old.relative_to(root).as_posix()],
                       check=True, capture_output=True)
        old.unlink()
        current.parent.mkdir(parents=True)
        uses = "\n".join("load(c->asset%d_path);" % index for index in range(10))
        current.write_text('void owner(void) { bgfx_set_view_name(0,"current",INT32_MAX);\n' +
                           uses + "\n}\n", encoding="utf-8")
        (root / ".gitignore").write_text("build/\n", encoding="utf-8")
        ignored = root / "build/copied.c"
        ignored.parent.mkdir()
        ignored.write_text('bgfx_set_view_name(0,"ignored",-1);\n', encoding="utf-8")
        header = root / "engine/include/jce/middleware/scene/jce_scene.h"
        header.parent.mkdir(parents=True)
        header.write_text("typedef struct {\n" + "\n".join(
            "char asset%d_path[64];" % index for index in range(10)) +
            "\n} Fixture;\n", encoding="utf-8")
        with patch.object(inventory, "REPO_ROOT", root), \
                patch.object(authored._lgf, "REPO_ROOT", root), \
                patch.object(guard, "REPO_ROOT", root), \
                patch.object(authored, "REPO_ROOT", root), \
                patch.object(authored, "HEADER", header), \
                patch.object(authored, "EXEMPT", root / "no-exemptions.txt"):
            yield root, old, current, ignored


class WorkingTreeMigrationControls(unittest.TestCase):
    def run_gate(self, checker):
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            result = checker() if callable(checker) else checker.main()
        return result, stdout.getvalue(), stderr.getvalue()

    def test_real_deleted_index_entry_new_owner_and_ignored_copy_use_same_authority(self):
        with moved_owner_repository() as (root, old, current, ignored):
            old_rel = old.relative_to(root).as_posix()
            new_rel = current.relative_to(root).as_posix()
            self.assertIn(old_rel, inventory.tracked())
            self.assertNotIn(new_rel, inventory.tracked())
            self.assertNotIn(old_rel, inventory.working_tree())
            self.assertIn(new_rel, inventory.working_tree())
            self.assertNotIn(ignored.relative_to(root).as_posix(), inventory.working_tree())
            self.assertEqual(inventory.working_tree(strict=True), inventory.working_tree())
            self.assertEqual(self.run_gate(inventory.self_test)[0], 0)
            self.assertEqual(self.run_gate(guard)[0], 0)
            self.assertEqual(self.run_gate(authored)[0], 0)

    def test_moved_untracked_owner_still_enforces_both_existing_content_rules(self):
        with moved_owner_repository() as (_root, _old, current, _ignored):
            source = current.read_text(encoding="utf-8")
            current.write_text(source.replace("INT32_MAX", "-1"), encoding="utf-8")
            result, _stdout, stderr = self.run_gate(guard)
            self.assertEqual(result, 1)
            self.assertIn("engine/src/application/jce_jni_bridge.c", stderr)
            self.assertIn("outside [0, INT32_MAX]", stderr)
            current.write_text(source.replace("load(c->asset0_path);", "if(c->asset0_path[0]) {}"), encoding="utf-8")
            result, _stdout, stderr = self.run_gate(authored)
            self.assertEqual(result, 1)
            self.assertIn("asset0_path", stderr)
            self.assertIn("NOTHING under", stderr)

    def test_nondeleted_unreadable_source_is_not_filtered_out(self):
        with moved_owner_repository() as (root, _old, current, _ignored):
            self.assertIn(current.relative_to(root).as_posix(), inventory.working_tree())
            original = Path.read_text
            def denied(path, *args, **kwargs):
                if path == current:
                    raise PermissionError("fixture source permission denied")
                return original(path, *args, **kwargs)
            with patch.object(Path, "read_text", denied):
                result, _stdout, stderr = self.run_gate(guard)
                self.assertEqual(result, 1)
                self.assertIn("source permission denied", stderr)
                # Preserve authored gate's existing failing read/exit behavior.
                with self.assertRaisesRegex(PermissionError, "source permission denied"):
                    self.run_gate(authored)


class StrictInventoryControls(unittest.TestCase):
    def test_each_git_refusal_keeps_legacy_default_but_fails_explicit_strict_mode(self):
        for failure in ("--deleted", "--cached"):
            with self.subTest(failure=failure):
                def git_port(args, **kwargs):
                    return subprocess.CompletedProcess(args, 128 if failure in args else 0,
                                                       "", "fixture inventory refusal")
                with patch.object(inventory.subprocess, "run", side_effect=git_port):
                    self.assertEqual(inventory.working_tree(), [])
                    with self.assertRaisesRegex(inventory.GitInventoryError,
                                                "fixture inventory refusal"):
                        inventory.working_tree(strict=True)

    def test_git_launch_failure_preserves_tracked_and_legacy_default_behavior(self):
        with patch.object(inventory.subprocess, "run", side_effect=FileNotFoundError("fixture git missing")):
            self.assertEqual(inventory.tracked(), [])
            self.assertEqual(inventory.working_tree(), [])
            with self.assertRaisesRegex(inventory.GitInventoryError, "fixture git missing"):
                inventory.working_tree(strict=True)

    def test_strict_nul_inventory_preserves_space_and_newline_paths(self):
        paths = ["engine/src/with space.c", "editor/src/with\nnewline.cpp"]
        def git_port(args, **kwargs):
            self.assertIn("-z", args)
            self.assertEqual(kwargs["encoding"], "utf-8")
            output = "" if "--deleted" in args else "\0".join(paths + [paths[0]]) + "\0"
            return subprocess.CompletedProcess(args, 0, output, "")
        with patch.object(inventory.subprocess, "run", side_effect=git_port):
            self.assertEqual(inventory.working_tree(strict=True), paths)


if __name__ == '__main__':
    unittest.main()
