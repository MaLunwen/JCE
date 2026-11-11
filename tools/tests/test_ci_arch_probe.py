"""Positive/negative controls for CI object-header verification (no compiler)."""
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("ci_arch_probe", ROOT / "tools/build/ci_arch_probe.py")
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


class CIObjectProbeTests(unittest.TestCase):
    def test_elf_x86_and_arm64(self):
        for machine, width in ((3, 4), (183, 8), (62, 8), (40, 4)):
            payload = bytearray(64)
            payload[:4] = b"\x7fELF"
            payload[4] = 1 if width == 4 else 2
            payload[5] = 1
            struct.pack_into("<H", payload, 18, machine)
            self.assertEqual(probe.inspect_object(payload), ("elf", machine, width))

    def test_macho_x64_and_arm64(self):
        for machine in (0x01000007, 0x0100000C):
            payload = bytearray(32)
            payload[:4] = b"\xcf\xfa\xed\xfe"
            struct.pack_into("<I", payload, 4, machine)
            self.assertEqual(probe.inspect_object(payload), ("macho", machine, 8))

    def test_coff_x86_x64_arm64(self):
        for machine, width in ((0x014C, 4), (0x8664, 8), (0xAA64, 8)):
            payload = struct.pack("<HHIIIHH", machine, 1, 0, 0, 0, 0, 0)
            self.assertEqual(probe.inspect_object(payload), ("coff", machine, width))

    def test_wasm(self):
        self.assertEqual(probe.inspect_object(b"\x00asm\x01\x00\x00\x00"), ("wasm", 1, 4))

    def test_negative_controls(self):
        for malformed in (b"", b"no architecture", b"\x00asm\x05\x00\x00\x00",
                          b"\x7fELF\x01\x02" + bytes(64),
                          struct.pack("<HHIIIHH", 0x8664, 0, 0, 0, 0, 0, 0)):
            with self.subTest(malformed=malformed[:8]), self.assertRaises(ValueError):
                probe.inspect_object(malformed)

    def test_all_targets_have_consistent_machine_width(self):
        for target, (compiler, fmt, machine, width) in probe.TARGETS.items():
            self.assertIn(fmt, ("coff", "elf", "macho", "wasm"))
            self.assertIn(width, (4, 8))
            if target.endswith("x86") or "armv7" in target or target == "wasm32":
                self.assertEqual(width, 4)


if __name__ == "__main__":
    unittest.main()
