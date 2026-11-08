"""Source/witness provenance controls; not GPU numerical tests."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "classic_power_probe", ROOT / "tests/cmake/verify_classic_lit_power.py")
PROBE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROBE)
SHADER = ROOT / "engine/shaders/pbr/fs_classic_lit.sc"


class ClassicPowerProbeTests(unittest.TestCase):
    def test_save_during_read_cannot_split_snapshot_helper_or_hash(self):
        scratch = ROOT / "build/tests/classic-power-provenance"
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as temporary:
            folder = Path(temporary)
            live = folder / "live.sc"
            expected = SHADER.read_bytes()
            live.write_bytes(expected)
            real_read = Path.read_bytes
            def save_after_read(path):
                data = real_read(path)
                if path == live:
                    live.write_bytes(b"changed while collecting")
                return data
            with mock.patch.object(Path, "read_bytes", save_after_read):
                function, original, digest = PROBE.stage_source(live, folder, False)
            self.assertEqual((folder / "fs_classic_lit.sc").read_bytes(), expected)
            self.assertEqual(digest, hashlib.sha256(expected).hexdigest())
            self.assertIn(function.strip(), expected.decode())
            self.assertIsNone(original)
            self.assertNotEqual(live.read_bytes(), expected)

    def test_incomplete_lobe_migration_rejects_the_witness(self):
        source = SHADER.read_text(encoding="utf-8")
        source = source.replace("classic_lobe_power(max(dot(N,H),0.0),shine)",
                                "pow(max(dot(N,H),0.0),shine)")
        with self.assertRaisesRegex(ValueError, "all three shine lobes"):
            PROBE.power_function(source, False)

    def test_fixed_source_cannot_be_reported_as_legacy_formula(self):
        with self.assertRaisesRegex(ValueError, "legacy source"):
            PROBE.power_function(SHADER.read_text(encoding="utf-8"), True)

    def test_runtime_uniform_witness_uses_exact_production_definition(self):
        source = SHADER.read_text(encoding="utf-8")
        function, _ = PROBE.power_function(source, False)
        output = PROBE.witness(function)
        self.assertIn(function, output)
        self.assertIn("uniform vec4 u_powerBases;", output)
        self.assertIn("uniform vec4 u_powerExponents;", output)
        self.assertIn("classic_lobe_power(base, exponent)", output)
        self.assertNotIn("classic_lobe_power(0.0, 0.0)", output)


if __name__ == "__main__":
    unittest.main()
