"""CPU ownership controls against actual host-tool links and the matrix.

These invoke the existing dependency gate, not a compiler, packer or GPU.
"""
import copy
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.audit import check_dependency_boundaries as gate


class HostPakDependencyOwnership(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rules = gate.build_rules(gate.load_ownership())

    def test_dependency_matrix_builtin_yaml_parser_matches_real_ownership(self):
        # Offline runs must not require PyYAML to understand block and
        # wrapped-flow lists in the checked-in ownership matrix.
        source = (ROOT / 'contracts/dependency-ownership.yml').read_text(encoding='utf-8')
        fallback = gate.build_rules(gate._mini_yaml_load(source))
        for capability in ('ecs', 'json', 'image-decode-ldr', 'sdl-portability-substrate'):
            self.assertEqual(fallback[capability], self.rules[capability], capability)
        self.assertEqual(gate.check_links(fallback), [])

    def test_actual_host_links_and_existing_header_owners_pass(self):
        self.assertEqual(gate.check_links(self.rules), [])
        self.assertIn('jce_pak', self.rules['image-decode-ldr']['link_whitelist'])
        self.assertIn('jce_pak', self.rules['json']['link_whitelist'])
        self.assertEqual(self.rules['image-decode-ldr']['include_whitelist'],
                         ['engine/src/renderer/'])
        self.assertIn('engine/src/resource/jce_bundle_loader.c',
                      self.rules['json']['include_whitelist'])
        self.assertFalse(any(path.startswith('tools/') for path in
                             self.rules['json']['include_whitelist']))

    def test_removing_each_actual_tool_permission_rejects_its_real_direct_link(self):
        for capability, dep in (('image-decode-ldr', 'sdl_image'), ('json', 'cjson')):
            with self.subTest(capability=capability):
                rules = copy.deepcopy(self.rules)
                rules[capability]['link_whitelist'].remove('jce_pak')
                violations = gate.check_links(rules)
                self.assertTrue(any(item['file'] == 'CMake target jce_pak' and
                                    item['dep'] == dep and item['kind'] == 'link'
                                    for item in violations), violations)

    def test_new_tool_or_project_target_does_not_inherit_the_permissions(self):
        source = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        blocks = [match.group(0) for match in gate._TLL_RE.finditer(source)
                  if match.group(1) == 'jce_pak']
        self.assertEqual(len(blocks), 1)
        with tempfile.TemporaryDirectory(prefix='jce-pak-owner-') as directory:
            root = Path(directory)
            # Mutate the real declaration; do not reproduce the link scanner.
            (root / 'CMakeLists.txt').write_text(
                blocks[0].replace('jce_pak', 'unsanctioned_consumer', 1), encoding='utf-8')
            with patch.object(gate, 'REPO_ROOT', root), \
                    patch.object(gate, 'CMAKE_FILES', ['CMakeLists.txt']):
                violations = gate.check_links(self.rules)
        refused = {item['dep'] for item in violations
                   if item['file'] == 'CMake target unsanctioned_consumer'}
        self.assertTrue({'sdl_image', 'cjson'}.issubset(refused), violations)

    def test_public_tool_facades_do_not_permit_direct_decoder_or_json_headers(self):
        with tempfile.TemporaryDirectory(prefix='jce-pak-header-owner-') as directory:
            root = Path(directory)
            (root / 'tools').mkdir()
            (root / 'tools/bypass.c').write_text(
                '#include <SDL3_image/SDL_image.h>\n#include <cjson/cJSON.h>\n', encoding='utf-8')
            with patch.object(gate, 'REPO_ROOT', root), \
                    patch.object(gate, 'SCAN_ROOTS', ['tools']):
                violations = gate.check_includes(self.rules)
        self.assertEqual({item['dep'] for item in violations}, {'sdl_image', 'cjson'})
        self.assertTrue(all(item['file'] == 'tools/bypass.c' for item in violations))


if __name__ == '__main__':
    unittest.main()
