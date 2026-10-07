"""Offline IBL payload validation negatives, without a graphics device."""
import importlib.util
from pathlib import Path
import struct
import unittest
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("bake_ibl", ROOT / "tools/assets/bake_ibl.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
class IblPayloadTests(unittest.TestCase):
    def test_canonical_payload_and_bad_channels(self):
        data = struct.pack("<4e", .5, .1, 0, 1) * (256 * 256)
        module.validate(data)
        for changed in (data[:-1], struct.pack("<4e", float("nan"), .1, 0, 1) + data[8:],
                        struct.pack("<4e", .5, .1, 1, 1) + data[8:],
                        struct.pack("<4e", .5, .1, 0, 0) + data[8:]):
            with self.assertRaises(ValueError):
                module.validate(changed)
if __name__ == "__main__":
    unittest.main()
