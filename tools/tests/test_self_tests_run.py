"""Maintained-source enumeration and bounded lexical caller controls."""
from contextlib import contextmanager, redirect_stdout
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import types
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "self_tests_run", Path(__file__).parents[1] / "lint/check_self_tests_run.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)

SYMBOL = "jce_sample_self_test"
OWN = "engine/src/sample.c"
DEFINITION = "int %s(void)\n{ return 0; }\n" % SYMBOL


@contextmanager
def repository(files=None, *, ignores="", tests=True, engine=True, checker=gate):
    """Actual Git authorities, without copying the product or build tree."""
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        subprocess.run(["git", "init", "-q", str(root)],
                       check=True, capture_output=True)
        for relative, text in {".gitignore": ignores, **(files or {})}.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8", newline="\n")
        if tests:
            (root / "tests").mkdir(exist_ok=True)
        if engine:
            (root / "engine/src").mkdir(parents=True, exist_ok=True)
        exemption = root / "self_test_exempt.txt"
        with patch.object(checker, "REPO_ROOT", root), \
                patch.object(checker, "ENGINE_SRC", root / "engine/src"), \
                patch.object(checker, "EXEMPT_FILE", exemption), \
                patch.object(checker._lgf, "REPO_ROOT", root):
            yield root


def run_main(checker=gate):
    output = io.StringIO()
    with redirect_stdout(output):
        code = checker.main()
    return code, output.getvalue()


def source_mutant(old, new):
    source = Path(gate.__file__).read_text(encoding="utf-8")
    if source.count(old) != 1:
        raise AssertionError("negative-control site is not unique")
    module = types.ModuleType("self_test_source_mutant")
    module.__file__ = gate.__file__
    exec(compile(source.replace(old, new, 1), gate.__file__, "exec"), module.__dict__)
    return module


class SelfTestCallerControls(unittest.TestCase):
    def test_actual_external_call_is_found_once_and_own_file_is_excluded(self):
        files = {OWN: DEFINITION + "void recur(void) { %s(); }\n" % SYMBOL,
                 "tests/sample.c": "void run(void) { %s(); %s(); }\n" %
                 (SYMBOL, SYMBOL)}
        with repository(files):
            index = gate.source_index()
            self.assertEqual(gate.definitions(index), {SYMBOL: OWN})
            self.assertEqual(gate.callers(SYMBOL, OWN, index), ["tests/sample.c"])
            self.assertEqual(run_main()[0], 0)

    def test_definition_self_declaration_and_own_call_cannot_pass(self):
        with repository({OWN: "int %s(void);\n" % SYMBOL + DEFINITION +
                         "void recur(void) { %s(); }\n" % SYMBOL}):
            self.assertEqual(gate.callers(SYMBOL, OWN), [])
            code, output = run_main()
            self.assertEqual(code, 1)
            self.assertIn("Self-tests nothing calls (1)", output)

    def test_comment_calls_and_bare_mentions_are_rejected(self):
        with repository({OWN: DEFINITION,
                         "tests/comments.c": "/* %s(); */\n// %s();\n%s\n" %
                         (SYMBOL, SYMBOL, SYMBOL)}):
            self.assertEqual(gate.callers(SYMBOL, OWN), [])
            self.assertEqual(run_main()[0], 1)

    def test_existing_declaration_and_string_matching_remain_lexical(self):
        # Preserve the previous extractor's scope; these are not execution proof.
        for source in ("int %s(void);\n" % SYMBOL,
                       'const char *note = "%s()";\n' % SYMBOL):
            with self.subTest(source=source), repository(
                    {OWN: DEFINITION, "tests/lexical.h": source}):
                self.assertEqual(gate.callers(SYMBOL, OWN), ["tests/lexical.h"])
                self.assertEqual(run_main()[0], 0)

    def test_identifier_boundaries_do_not_invent_a_caller(self):
        with repository({OWN: DEFINITION, "tests/other.cpp":
                         "void run() { other_%s(); %s_suffix(); }\n" %
                         (SYMBOL, SYMBOL)}):
            self.assertEqual(run_main()[0], 1)

    def test_existing_caller_api_accepts_other_function_identifiers(self):
        with repository({OWN: DEFINITION,
                         "tests/caller.c": "void run(void) { ordinary_function(); }\n"}):
            self.assertEqual(gate.callers("ordinary_function", OWN), ["tests/caller.c"])
            self.assertEqual(gate.callers("function", OWN), [])

    def test_git_ignored_generated_callers_cannot_prove_consumption(self):
        ignored = "tests/consumers/demo/build/frozen/engine/src/caller.c"
        files = {OWN: DEFINITION, ignored: "void run(void) { %s(); }\n" % SYMBOL,
                 "tests/third_party/caller.c": "void run(void) { %s(); }\n" % SYMBOL,
                 "tools/vendor/caller.c": "void run(void) { %s(); }\n" % SYMBOL}
        with repository(files, ignores="**/build/\n") as root:
            subprocess.run(["git", "-C", str(root), "add", OWN],
                           check=True, capture_output=True)
            self.assertNotIn(ignored, gate._lgf.working_tree())
            self.assertEqual(gate.callers(SYMBOL, OWN), [])
            self.assertEqual(run_main()[0], 1)

    def test_same_source_generated_scan_mutant_exposes_false_consumption(self):
        mutant = source_mutant(
            "for rel in sorted(_lgf.working_tree()):",
            'for rel in sorted(p.relative_to(REPO_ROOT).as_posix() for p in '
            'REPO_ROOT.rglob("*") if p.is_file()):')
        files = {OWN: DEFINITION,
                 "tests/consumers/demo/build/frozen/caller.c": "%s();\n" % SYMBOL}
        with repository(files, ignores="**/build/\n"):
            self.assertEqual(run_main()[0], 1)
        with repository(files, ignores="**/build/\n", checker=mutant):
            # Removing only maintained enumeration incorrectly accepts the same fixture.
            self.assertEqual(run_main(mutant)[0], 0)

    def test_untracked_first_party_caller_is_included_without_staging(self):
        real = "tests/new/caller.cpp"
        with repository({OWN: DEFINITION, real: "void run() { %s(); }\n" % SYMBOL}) as root:
            subprocess.run(["git", "-C", str(root), "add", OWN],
                           check=True, capture_output=True)
            self.assertNotIn(real, gate._lgf.tracked())
            self.assertIn(real, gate._lgf.working_tree())
            self.assertEqual(gate.callers(SYMBOL, OWN), [real])
            self.assertEqual(run_main()[0], 0)

    def test_all_symbols_share_one_read_and_comment_strip_per_source(self):
        symbols = ["jce_fixture_%d_self_test" % number for number in range(24)]
        files = {OWN: "\n".join("int %s(void)\n{ return 0; }" % name for name in symbols),
                 "tests/all.c": "void run(void) {\n" +
                 "\n".join("%s();" % name for name in symbols) + "\n}\n",
                 "editor/src/irrelevant.hpp": "void unrelated();\n"}
        with repository(files) as root:
            reads = {}
            original_read = Path.read_text

            def counted_read(path, *args, **kwargs):
                if path.suffix in gate.SUFFIXES:
                    reads[path.relative_to(root).as_posix()] = reads.get(
                        path.relative_to(root).as_posix(), 0) + 1
                return original_read(path, *args, **kwargs)

            with patch.object(Path, "read_text", counted_read), \
                    patch.object(gate, "strip_comments", wraps=gate.strip_comments) as strip, \
                    patch.object(gate._lgf, "working_tree", wraps=gate._lgf.working_tree) as listing:
                code, output = run_main()
            self.assertEqual(code, 0)
            self.assertIn("24 of 24", output)
            self.assertEqual(reads, {relative: 1 for relative in files})
            self.assertEqual(strip.call_count, len(files))
            self.assertEqual(listing.call_count, 1)

    def test_direct_apis_do_not_reuse_stale_index_in_another_repository(self):
        with repository({OWN: DEFINITION, "tests/caller.c": "%s();\n" % SYMBOL}):
            self.assertEqual(gate.callers(SYMBOL, OWN), ["tests/caller.c"])
            self.assertEqual(gate.definitions(), {SYMBOL: OWN})
        with repository({OWN: DEFINITION}):
            self.assertEqual(gate.callers(SYMBOL, OWN), [])
            self.assertEqual(gate.definitions(), {SYMBOL: OWN})
            self.assertEqual(run_main()[0], 1)

    def test_repeated_main_observes_a_removed_caller(self):
        with repository({OWN: DEFINITION, "tests/caller.c": "%s();\n" % SYMBOL}) as root:
            self.assertEqual(run_main()[0], 0)
            (root / "tests/caller.c").write_text("// no caller\n", encoding="utf-8")
            self.assertEqual(run_main()[0], 1)

    def test_exemptions_require_reason_and_stale_entries_still_fail(self):
        with repository({OWN: DEFINITION}) as root:
            path = root / "self_test_exempt.txt"
            path.write_text(SYMBOL + "\n", encoding="utf-8")
            self.assertEqual(run_main()[0], 1)
            path.write_text(SYMBOL + " # requires an actual device\n", encoding="utf-8")
            self.assertEqual(run_main()[0], 0)
            path.write_text(SYMBOL + " # requires an actual device\n" +
                            "jce_stale_self_test # missing function\n", encoding="utf-8")
            code, output = run_main()
            self.assertEqual(code, 1)
            self.assertIn("Exemptions for self-tests that no longer exist (1)", output)

    def test_missing_source_and_unmatched_definitions_are_unavailable(self):
        for options in ({"tests": False}, {"engine": False}, {}):
            with self.subTest(options=options), repository(**options):
                code, output = run_main()
                self.assertEqual(code, 2)
                self.assertIn("UNAVAILABLE", output)
                self.assertNotIn("check: OK", output)

    def test_unreadable_maintained_source_cannot_report_partial_clean_scan(self):
        with repository({OWN: DEFINITION, "tests/caller.c": "%s();\n" % SYMBOL,
                         "engine/src/unreadable.c": "int jce_hidden_self_test(void)\n{}\n"}):
            original_read = Path.read_text

            def refused_read(path, *args, **kwargs):
                if path.name == "unreadable.c":
                    raise OSError("fixture unreadable source")
                return original_read(path, *args, **kwargs)

            with patch.object(Path, "read_text", refused_read):
                code, output = run_main()
            self.assertEqual(code, 2)
            self.assertIn("engine/src/unreadable.c", output)
            self.assertNotIn("check: OK", output)

    def test_vendor_definitions_and_non_caller_trees_are_not_maintained_inputs(self):
        with repository({OWN: DEFINITION,
                         "engine/src/third_party/vendor.c":
                         "int jce_vendor_self_test(void)\n{}\n",
                         "examples/demo/caller.c": "%s();\n" % SYMBOL}):
            self.assertEqual(gate.definitions(), {SYMBOL: OWN})
            self.assertEqual(run_main()[0], 1)


if __name__ == "__main__":
    unittest.main()
