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
    "tools/build/xp_editor.py",
    "engine/CMakeLists.txt", "engine/cmake/JCEVendorSources.cmake",
    "engine/cmake/codec_ports/libhevc/CMakeLists.txt",
    "engine/cmake/codec_ports/openh264/CMakeLists.txt",
    "contracts/vendor-sources.json", "editor/src/core/jce_build_manager.cpp",
    "editor/src/core/jce_project_build_target.cpp",
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

    def gate(self, passes=False, reason=None):
        result = subprocess.run([sys.executable, str(GATE), str(self.root)],
                                capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(result.returncode, 0 if passes else 1, result.stdout + result.stderr)
        if not passes:
            self.assertIn("check_patented_codec_gate: FAIL", result.stdout)
        if reason:
            self.assertIn(reason, result.stdout)

    def test_current_tree(self):
        self.gate(passes=True)

    def test_sdk_metadata_owner_must_exist(self):
        (self.root / "editor/src/core/jce_project_build_target.cpp").unlink()
        self.gate(reason="lost actual SDK metadata variant comparison")

    def test_comment_cannot_replace_sdk_metadata_definition(self):
        self.replace("editor/src/core/jce_project_build_target.cpp",
                     "std::string jce_project_build_sdk_target_error(",
                     "// std::string jce_project_build_sdk_target_error(")
        self.gate(reason="lost actual SDK metadata variant comparison")

    def test_sdk_parser_must_consume_real_archive_metadata(self):
        self.replace("editor/src/core/jce_project_build_target.cpp",
                     "jce_fs_host_read_capped(", "jce_fake_read_stamp(")
        self.gate(reason="lost actual SDK metadata variant comparison")

    def test_unrelated_call_and_comment_cannot_replace_build_admission(self):
        name = "editor/src/core/jce_build_manager.cpp"
        self.replace(name, "jce_project_build_sdk_target_error(sdk,",
                     'std::string("") /* jce_project_build_sdk_target_error */ (sdk,')
        path = self.root / name
        path.write_text(path.read_text(encoding="utf-8") +
                        '\nvoid unrelated() { auto stamp = jce_project_build_sdk_target_error(sdk, arch, platform, variant); }\n',
                        encoding="utf-8")
        self.gate(reason="lost SDK target/variant rejection")

    def test_reading_metadata_without_variant_comparison_is_rejected(self):
        self.replace("editor/src/core/jce_project_build_target.cpp",
                     'std::strcmp(text(expected.first), expected.second)', 'false')
        self.gate(reason="lost actual SDK metadata variant comparison")

    def test_comparison_without_failure_is_rejected(self):
        self.replace("editor/src/core/jce_build_manager.cpp",
                     "            selected_arch, build_platform, variant); !error.empty()) {",
                     "            selected_arch, build_platform, variant); false) {")
        self.gate(reason="lost SDK target/variant rejection")

    def test_cli_cannot_omit_archive_variant_admission(self):
        self.replace("tools/build/jce.py", '.require_sdk_target(sdk, system, t["arch"], config, variant, delivery)',
                     '.ignore_sdk_target(sdk, system, t["arch"], config, variant, delivery)')
        self.gate(reason="lost actual per-archive SDK target/variant admission")

    def test_dist_effective_codec_switch_still_must_be_forced_off(self):
        name = "CMakeLists.txt"
        self.replace(name,
                     "\tendif()\n\tset(JCE_PATENTED_CODECS_ENABLED OFF)\nendif()",
                     "\tendif()\n\tset(JCE_PATENTED_CODECS_ENABLED ON)\nendif()")
        self.gate(reason="CMakeLists.txt no longer forces JCE_PATENTED_CODECS_ENABLED OFF")

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

    def test_web_release_cannot_be_implicitly_disabled(self):
        self.replace("CMakeLists.txt", '# Assert the derivation actually held.',
            'if(JCE_PLATFORM_WEB)\n\tset(JCE_PATENTED_CODECS_ENABLED OFF)\nendif()\n\n# Assert the derivation actually held.')
        self.gate(reason="Web Release must not silently override")

    def test_xp_cannot_force_dist_when_release_was_requested(self):
        self.replace("tools/build/xp_editor.py",
                     'f"-DJCE_BUILD_VARIANT={variant}"', '"-DJCE_BUILD_VARIANT=dist"')
        self.gate(reason="XP producer forces Dist")

    def test_xp_cannot_force_patented_codecs_off_for_every_build(self):
        self.replace("tools/build/xp_editor.py",
                     '"-DJCE_BUILD_TESTS=OFF",',
                     '"-DJCE_BUILD_TESTS=OFF", "-DJCE_ENABLE_PATENTED_CODECS=OFF",')
        self.gate(reason="XP producer unconditionally disables patented codecs")

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
