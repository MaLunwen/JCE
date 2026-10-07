"""Current-build shader provenance and safe retirement controls."""
import json
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


if __name__ == '__main__':
    unittest.main()
