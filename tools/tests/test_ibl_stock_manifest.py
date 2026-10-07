"""Exercise the actual build integrity checker with tampered copies only."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
class ManifestTests(unittest.TestCase):
    def test_real_checker_rejects_payload_kernel_and_sampling_tampering(self):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("CMake not available")
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            directory = Path(temporary)
            data = directory / "stock.f16"
            source = ROOT / "engine/src/renderer/data/brdf_256.f16"
            manifest = json.loads(source.with_suffix(".f16.json").read_text())
            for name in manifest["kernel_sha256"]:
                shutil.copy2(ROOT / "engine/src/renderer" / name, directory / name)
            def run():
                return subprocess.run([cmake, "-DDATA=" + str(data),
                    "-DKERNEL_ROOT=" + str(directory), "-P",
                    str(ROOT / "cmake/JCEVerifyIblStock.cmake")],
                    capture_output=True, text=True).returncode
            data.write_bytes(source.read_bytes())
            metadata = data.with_suffix(".f16.json")
            metadata.write_text(json.dumps(manifest))
            self.assertEqual(run(), 0)
            data.write_bytes(source.read_bytes()[:-1])
            self.assertNotEqual(run(), 0)
            data.write_bytes(source.read_bytes())
            changed = dict(manifest, samples=64)
            metadata.write_text(json.dumps(changed))
            self.assertNotEqual(run(), 0)
            metadata.write_text(json.dumps(manifest))
            kernel = directory / "jce_ibl_brdf.h"
            kernel.write_bytes(kernel.read_bytes() + b"\n/* changed */\n")
            self.assertNotEqual(run(), 0)
if __name__ == "__main__":
    unittest.main()
