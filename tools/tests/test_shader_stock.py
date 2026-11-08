"""Current-build shader provenance and safe retirement controls."""
import json
import copy
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.assets.shader_stock import stage_stock, verify_stock, xxh32


class ShaderStockTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'build')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.source = self.base / 'source'
        (self.source / 'shaders').mkdir(parents=True)
        self.out = self.base / 'stock'
        self.bom = self.base / 'bom.json'
        self.entries = []
        self.add('vs_pbr_dx11.bin', b'original shader')

    def add(self, name, data):
        target = self.source / 'shaders' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        self.entries.append({'path': 'shaders/' + name, 'original_size': len(data),
                             'content_crc': f'0x{xxh32(data):08x}', 'verified': True})

    def stage(self):
        self.bom.write_text(json.dumps({'entries': self.entries}), encoding='utf-8')
        return stage_stock(self.bom, self.source, self.out, ['dx11'])

    def test_checksum_known_vectors(self):
        self.assertEqual(xxh32(b''), 0x02cc5d05)
        self.assertEqual(xxh32(b'a'), 0x550d7456)
        self.assertEqual(xxh32(b'abc'), 0x32d153ff)

    def test_filtered_bom_only_publishes_selected_variants(self):
        (self.source / 'shaders' / 'unselected_gl.bin').write_bytes(b'not in current pack')
        result = self.stage()
        self.assertEqual(result['profiles'], ['dx11'])
        self.assertEqual(len(result['files']), 1)
        self.assertFalse((self.out / 'unselected_gl.bin').exists())
        self.assertEqual(verify_stock(self.out), result)

    def test_crc_size_and_unverified_rejected_before_output(self):
        for field, value in [('content_crc', '0x00000000'), ('original_size', 99), ('verified', False), ('verified', None)]:
            original = self.entries[0][field]
            self.entries[0][field] = value
            with self.assertRaises(ValueError):
                self.stage()
            self.assertFalse(self.out.exists())
            self.entries[0][field] = original

    def test_unsafe_and_outside_bom_addresses(self):
        for name in ['../shader.bin', 'other/a.bin', 'shaders/../a.bin', 'shaders/a\\b.bin', 'shaders/C:a.bin', 'shaders//a.bin']:
            self.entries[0]['path'] = name
            with self.assertRaises(ValueError):
                self.stage()

    def test_opaque_inspection_cannot_recover_names_from_a_source_ledger(self):
        self.stage()
        previous_manifest = (self.out / 'manifest.json').read_bytes()
        previous_shader = (self.out / 'vs_pbr_dx11.bin').read_bytes()
        opaque = dict(self.entries[0], path=None)
        self.bom.write_text(json.dumps({
            'schema': 'jce.pakbom.v1', 'sources_verified': True,
            'header': {'has_debug_paths': False},
            'entries': [opaque],
            'sources': [{'path': self.entries[0]['path']}],
        }), encoding='utf-8')
        with self.assertRaises(ValueError):
            stage_stock(self.bom, self.source, self.out, ['dx11'])
        self.assertEqual((self.out / 'manifest.json').read_bytes(), previous_manifest)
        self.assertEqual((self.out / 'vs_pbr_dx11.bin').read_bytes(), previous_shader)

    def test_stock_hash_and_size_tamper(self):
        self.stage()
        binary = self.out / 'vs_pbr_dx11.bin'
        binary.write_bytes(b'X' * len(binary.read_bytes()))
        with self.assertRaises(ValueError):
            verify_stock(self.out)
        binary.write_bytes(b'wrong')
        with self.assertRaises(ValueError):
            verify_stock(self.out)
        with self.assertRaises(ValueError):
            self.stage()
        self.assertEqual(binary.read_bytes(), b'wrong')

    def test_unknown_binary_rejected_and_never_deleted(self):
        self.stage()
        unknown = self.out / 'custom.bin'
        unknown.write_bytes(b'unknown')
        with self.assertRaises(ValueError):
            self.stage()
        self.assertEqual(unknown.read_bytes(), b'unknown')

    def test_owned_upgrade_retires_only_verified_old_binary(self):
        self.stage()
        unknown = self.out / 'user-notes.txt'
        unknown.write_bytes(b'keep')
        self.entries.clear()
        self.add('nested/fs_new_dx11.bin', b'new')
        self.stage()
        self.assertFalse((self.out / 'vs_pbr_dx11.bin').exists())
        self.assertEqual(unknown.read_bytes(), b'keep')
        self.assertTrue((self.out / 'nested/fs_new_dx11.bin').exists())

    def test_duplicate_case_collision_rejected(self):
        self.entries.append(dict(self.entries[0]))
        with self.assertRaises(ValueError):
            self.stage()

    def test_symlink_escape_rejected(self):
        from tools.assets.shader_stock import inside
        with self.assertRaises(ValueError):
            inside(self.source, '../../escape.bin')



class ShaderStockProfileTests(unittest.TestCase):
    def setUp(self):
        from tools.shadergen import gen_shader_variants
        self.gen = gen_shader_variants
        self.manifest = self.gen.load()

    def test_full_cooker_stays_unrestricted_and_all_original_cells_remain(self):
        full = self.gen.stock_profiles(self.manifest)["profiles"]["full"]
        self.assertTrue(full["cook"]["unrestricted"])
        self.assertEqual(full["cook"]["vertex"], [])
        self.assertEqual(full["cook"]["fragment"], [])
        self.assertEqual(len(full["pbr_cells"]), 24)
        self.assertEqual(len({(p["vertex"], p["fragment"]) for p in full["pbr_cells"]}), 18)
        self.assertEqual([p["member"] for p in full["stock_pairs"]], [
            "color", "textured", "text_sdf", "mesh", "pbr_toon",
            "outline_skinned", "shadow", "shadow_inst", "shadow_skinned", "terrain"])

    def test_compatibility_requests_only_static_base_and_no_shadow(self):
        profile = self.gen.stock_profiles(self.manifest)["profiles"]["compatibility"]
        self.assertEqual([(p["variant_name"], p["key"], p["fragment"])
                          for p in profile["pbr_cells"]],
                         [("pbr", 0, "pbr"), ("pbr", 2, "pbr_noshadow")])
        self.assertEqual([p["member"] for p in profile["stock_pairs"]],
                         ["color", "textured", "text_sdf", "mesh", "shadow"])
        cook = profile["cook"]
        self.assertFalse(cook["unrestricted"])
        self.assertEqual(cook["compute"], [])
        self.assertEqual(cook["vertex"], sorted(set(cook["vertex"])))
        self.assertEqual(cook["fragment"], sorted(set(cook["fragment"])))
        self.assertIn("vs_imgui", cook["vertex"])
        self.assertIn("fs_pbr_noshadow", cook["fragment"])
        self.assertIn("fs_present", cook["fragment"])
        self.assertIn("fs_reveal_dissolve", cook["fragment"])
        self.assertTrue({"vs_water", "vs_foliage", "vs_foliage_shadow",
                         "vs_particle", "vs_particle_expanded"}.issubset(cook["vertex"]))
        self.assertTrue({"fs_water", "fs_foliage", "fs_foliage_shadow",
                         "fs_particle"}.issubset(cook["fragment"]))
        forbidden = ("inst", "skinned", "fwdplus", "terrain", "toon", "outline")
        self.assertFalse(any(any(part in stem for part in forbidden)
                             for stem in cook["vertex"] + cook["fragment"]))
        sources = {p.stem for p in (ROOT / "engine/shaders").rglob("*.sc")}
        self.assertFalse(set(cook["vertex"] + cook["fragment"]) - sources)

    def test_compatibility_selection_cannot_change_modern_generated_shaders(self):
        before = self.gen.wanted(self.manifest)
        altered = copy.deepcopy(self.manifest)
        altered["stock_profiles"][1]["pbr_cells"].pop()
        after = self.gen.wanted(altered)
        changed_outputs = {ROOT / altered["stock_profile_table_out"],
                           ROOT / altered["stock_profile_catalog_out"]}
        self.assertTrue(all(before[path] == after[path]
                            for path in before if path not in changed_outputs))
        self.assertNotEqual(before[ROOT / altered["stock_profile_table_out"]],
                            after[ROOT / altered["stock_profile_table_out"]])

    def test_invalid_profile_addresses_and_selectors_are_rejected(self):
        mutations = [
            lambda m: m["stock_programs"][0].update(vertex="../outside"),
            lambda m: m["stock_programs"].append(m["stock_programs"][0].copy()),
            lambda m: m["stock_profiles"][1].update(stock_members=["absent"]),
            lambda m: m["stock_profiles"][1]["pbr_cells"][0].update(variant="missing"),
            lambda m: m["stock_profiles"][1]["pbr_cells"][0].update(keywords=["missing"]),
            lambda m: m["stock_profiles"][1]["pbr_cells"].append(
                m["stock_profiles"][1]["pbr_cells"][0].copy()),
            lambda m: m["stock_profiles"][1].update(cook_unrestricted=1),
        ]
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                altered = copy.deepcopy(self.manifest)
                mutate(altered)
                with self.assertRaises(ValueError):
                    self.gen.wanted(altered)


if __name__ == '__main__':
    unittest.main()
