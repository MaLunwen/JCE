"""Real install-script registration, including logical DESTDIR paths."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.assets.shader_stock import xxh32


@unittest.skipUnless(shutil.which('cmake'), 'CMake subject unavailable')
class ShaderStockCMakeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'build')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.source = self.base / 'source'
        self.out = self.base / 'physical prefix' / 'share/jce/shaders'
        self.logical = self.base / 'logical prefix' / 'share/jce/shaders'
        self.bom = self.base / 'bom.json'
        self.entries = []
        for name, data in [('vs_fixture.bin', b'first shader'), ('nested/fs_fixture.bin', b'second shader')]:
            path = self.source / 'shaders' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            self.entries.append({'path': 'shaders/' + name, 'original_size': len(data),
                                 'content_crc': f'0x{xxh32(data):08x}', 'verified': True})

    def run_script(self, logical=None, corrupt=False):
        if corrupt:
            self.entries[0]['content_crc'] = '0x00000000'
        self.bom.write_text(json.dumps({'entries': self.entries}), encoding='utf-8')
        registration = self.base / 'registered.txt'
        driver = self.base / 'install.cmake'
        settings = {'PYTHON': Path(sys.executable).as_posix(),
                    'SCRIPT': (ROOT / 'tools/assets/shader_stock.py').as_posix(),
                    'BOM': self.bom.as_posix(), 'ROOT': self.source.as_posix(),
                    'OUT': self.out.as_posix(), 'PROFILES': 'dx11'}
        if logical is not None:
            settings['MANIFEST_ROOT'] = logical.as_posix()
        script = ''.join(f'set(JCE_SHADER_STOCK_{key} [==[{value}]==])\n' for key, value in settings.items())
        script += f'include([==[{(ROOT / "cmake/JCEStageShaderStock.cmake").as_posix()}]==])\n'
        script += 'list(JOIN CMAKE_INSTALL_MANIFEST_FILES "\\n" _files)\n'
        script += f'file(WRITE [==[{registration.as_posix()}]==] "${{_files}}\\n")\n'
        driver.write_text(script, encoding='utf-8', newline='\n')
        result = subprocess.run([shutil.which('cmake'), '-P', str(driver)], capture_output=True, text=True, timeout=30)
        return result, registration

    def assert_registration(self, path, prefix):
        expected = {str(prefix / 'manifest.json').replace('\\', '/'),
                    str(prefix / 'vs_fixture.bin').replace('\\', '/'),
                    str(prefix / 'nested/fs_fixture.bin').replace('\\', '/')}
        self.assertEqual(set(path.read_text().splitlines()), expected)
        for entry in self.entries:
            self.assertTrue((self.out / entry['path'][len('shaders/'):]).is_file())

    def test_normal_install_registers_complete_stock(self):
        result, path = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_registration(path, self.out)

    def test_destdir_registers_logical_not_physical_paths(self):
        result, path = self.run_script(self.logical)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_registration(path, self.logical)
        self.assertFalse(self.logical.exists())

    def test_failed_bom_does_not_register(self):
        result, path = self.run_script(corrupt=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(path.exists())
        self.assertFalse(self.out.exists())


if __name__ == '__main__':
    if not shutil.which('cmake'):
        print('SKIP: CMake subject unavailable (exit 77)')
        raise SystemExit(77)
    unittest.main()
