"""Production source enumeration and raw CTest executable closure controls."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("closure", Path(__file__).parents[1] /
                                           "lint/check_jce_tests_closure.py")
closure = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(closure)

RAW = 'add_test(NAME extra COMMAND real_test)\nset_tests_properties(extra PROPERTIES LABELS "unit")\n'
HELPERS = "\n".join('function(%s NAME)\n jce_test_build_dependencies(${NAME})\nendfunction()' %
                    name for name in closure.HELPER_NAMES)


class ClosureControls(unittest.TestCase):
    def test_real_target_requires_dependency_and_valid_helper_provider_counts(self):
        path = closure.REPO_ROOT / "tests/renderer/CMakeLists.txt"
        self.assertEqual(len(closure.check_raw_registrations_wired({path: RAW})), 1)
        self.assertEqual(closure.check_raw_registrations_wired(
            {path: RAW + 'jce_test_build_dependencies(real_test)'}), [])
        for helper in closure.HELPER_NAMES:
            self.assertEqual(closure.check_raw_registrations_wired(
                {path: RAW + helper + '(real_test SOURCES actual.c)'}), [])
        self.assertEqual(len(closure.check_raw_registrations_wired(
            {path: RAW + '# jce_add_unit_test(real_test SOURCES actual.c)'})), 1)
        self.assertEqual(closure.check_helpers_still_wired(HELPERS), [])
        self.assertEqual(len(closure.check_helpers_still_wired(
            HELPERS.replace('jce_test_build_dependencies(${NAME})', '', 1))), 1)

    def test_domain_routing_requires_both_edges_and_its_deferred_audit(self):
        metadata = closure.METADATA_CMAKE.read_text(encoding="utf-8")
        self.assertEqual(closure.check_build_dependency_authority(metadata), [])
        for wire in ('add_dependencies(jce_tests ${ARGN})',
                     'add_dependencies(jce_tests_${_domain} ${ARGN})',
                     '_jce_assert_test_build_closure()'):
            with self.subTest(wire=wire):
                self.assertEqual(len(closure.check_build_dependency_authority(
                    metadata.replace(wire, '', 1))), 1)
        path = closure.REPO_ROOT / "tests/renderer/CMakeLists.txt"
        self.assertEqual(len(closure.check_raw_registrations_wired(
            {path: RAW + 'add_dependencies(jce_tests real_test)'})), 1)
        self.assertEqual(closure.check_raw_registrations_wired(
            {path: RAW + 'jce_test_build_dependencies(other real_test script_provider)'}), [])

    def test_git_authority_excludes_nested_ignored_copy_keeps_new_real_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(['git', 'init', '-q', str(root)], check=True, capture_output=True)
            (root / '.gitignore').write_text('**/build/\n', encoding='utf-8')
            files = {
                'tests/CMakeLists.txt': HELPERS,
                'tests/real/CMakeLists.txt': RAW,
                'tests/consumer/build/frozen-sources/tests/renderer/CMakeLists.txt': RAW,
            }
            for relative, text in files.items():
                target = root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding='utf-8')
            subprocess.run(['git', '-C', str(root), 'add', 'tests/CMakeLists.txt'],
                           check=True, capture_output=True)
            with patch.object(closure, 'REPO_ROOT', root), patch.object(closure._lgf, 'REPO_ROOT', root):
                paths = closure.source_cmake_files()
                self.assertEqual({p.relative_to(root).as_posix() for p in paths},
                                 {'tests/CMakeLists.txt', 'tests/real/CMakeLists.txt'})
                text = {path: path.read_text(encoding='utf-8') for path in paths}
                self.assertEqual(len(closure.check_raw_registrations_wired(text)), 1)
                text[root / 'tests/real/CMakeLists.txt'] += 'jce_test_build_dependencies(real_test)'
                self.assertEqual(closure.check_raw_registrations_wired(text), [])


if __name__ == '__main__':
    unittest.main()
