"""Mutate only JCE-owned scratch files; prove the source gate rejects regressions."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
GATE = ROOT / "tools/audit/check_patented_codec_gate.py"
FILES = (
    "CMakePresets.json", "CMakeLists.txt", "tools/build/jce.py",
    "engine/CMakeLists.txt", "engine/cmake/JCEVendorSources.cmake",
    "engine/cmake/codec_ports/libhevc/CMakeLists.txt",
    "engine/cmake/codec_ports/openh264/CMakeLists.txt",
    "contracts/vendor-sources.json", "editor/src/core/jce_build_manager.cpp",
)


class SourcePinsGateTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="jce-owned-codec-gate-")
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        for name in FILES:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes((ROOT / name).read_bytes())

    def replace(self, name, old, new):
        path = self.root / name
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def gate(self, passes=False):
        result = subprocess.run([sys.executable, str(GATE), str(self.root)],
                                capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(result.returncode, 0 if passes else 1, result.stdout + result.stderr)
        if not passes:
            self.assertIn("check_patented_codec_gate: FAIL", result.stdout)

    def test_current_tree(self):
        self.gate(passes=True)

    def test_missing_acquisition(self):
        self.replace("engine/CMakeLists.txt",
                     "jce_vendor_source(fdk-aac JCE_FDKAAC_SOURCE_DIR)", "")
        self.gate()

    def test_silent_downgrade(self):
        path = self.root / "engine/CMakeLists.txt"
        path.write_text(path.read_text() + "\nset(JCE_PATENTED_CODECS_ENABLED OFF)\n")
        self.gate()

    def test_acquisition_failure_is_fatal(self):
        self.replace("engine/cmake/JCEVendorSources.cmake", "message(FATAL_ERROR", "message(WARNING")
        self.gate()

    def test_incremental_verification_remains(self):
        self.replace("engine/cmake/codec_ports/openh264/CMakeLists.txt",
                     "add_dependencies(openh264dec jce_vendor_verify_openh264)", "")
        self.gate()

    def test_verification_command_remains(self):
        self.replace("engine/cmake/JCEVendorSources.cmake", "--verify-only", "")
        self.gate()

    def test_fixed_hash_remains(self):
        path = self.root / "contracts/vendor-sources.json"
        pins = json.loads(path.read_text())
        del pins["sources"]["libhevc"]["sha256"]
        path.write_text(json.dumps(pins))
        self.gate()

    def test_release_default_remains(self):
        path = self.root / "engine/CMakeLists.txt"
        text, count = re.subn(r'(option\(JCE_ENABLE_PATENTED_CODECS.*?"\s+)ON\)',
                              r'\1OFF)', path.read_text(), count=1, flags=re.S)
        self.assertEqual(count, 1)
        path.write_text(text)
        self.gate()

    def test_opt_out_is_not_stamped_by_preset(self):
        path = self.root / "CMakePresets.json"
        presets = json.loads(path.read_text())
        presets["configurePresets"][0].setdefault("cacheVariables", {})["JCE_ENABLE_PATENTED_CODECS"] = "ON"
        path.write_text(json.dumps(presets))
        self.gate()


if __name__ == "__main__":
    unittest.main()
