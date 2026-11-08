"""Requested-authoring CPU controls using real pinned TD source.

Every mutation is synthetic and in memory. No original file, GPU, executable,
or port generator is modified or run. Missing ignored reference fixtures are
explicitly unavailable on clean clones; present but invalid pins are failures.
"""
import copy
import importlib.util
import json
from pathlib import Path
import re
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[4]
PROJECT = Path(__file__).resolve().parents[1]
REFERENCE_ROOT = ROOT / "_workspace/references/phase1_tower_defense"
MANIFEST = PROJECT / "build/reference/manifest.json"
SOURCE_PATHS = (
    "js/TowerDefense/Levels/TowerDefense.Level.js",
    "js/TowerDefense/TowerDefense.js",
    "js/lib/three.min.js",
)
LEVEL_PATH = SOURCE_PATHS[0]
SPEC = importlib.util.spec_from_file_location(
    "td_render_parameter_audit",
    PROJECT / "tools/td_render_parameter_audit.py",
)
AUDIT = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = AUDIT
SPEC.loader.exec_module(AUDIT)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


class RequestedRenderParameterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        required = (MANIFEST,) + tuple(
            REFERENCE_ROOT / name for name in SOURCE_PATHS)
        missing = [str(path) for path in required if not path.is_file()]
        if missing:
            raise unittest.SkipTest(
                "Immutable TD reference fixture unavailable: " + ", ".join(missing))
        cls.sources, cls.identity = AUDIT.load_pinned_sources(
            REFERENCE_ROOT, MANIFEST, SOURCE_PATHS)
        cls.reference = AUDIT.parse_reference_rendering(cls.sources)
        cls.scene = read_json(PROJECT / "resources/scenes/td_lab.scene.json")
        cls.settings = read_json(PROJECT / ".jce/project-settings.json")
        cls.rp = read_json(PROJECT / "Settings/RenderPipeline.rp.json")
        cls.rp_cooked = read_json(PROJECT / "resources/Settings/RenderPipeline.rp.json")
        cls.classic = (ROOT / "engine/shaders/pbr/fs_classic_lit.sc").read_text(
            encoding="utf-8")

    def audit(self, reference=None, scene=None, settings=None, rp=None,
              rp_cooked=None):
        return AUDIT.audit_requested_rendering(
            copy.deepcopy(self.reference if reference is None else reference),
            copy.deepcopy(self.scene if scene is None else scene),
            copy.deepcopy(self.settings if settings is None else settings),
            copy.deepcopy(self.rp if rp is None else rp),
            copy.deepcopy(self.rp_cooked if rp_cooked is None else rp_cooked),
            self.classic,
        )

    def synthetic_reference(self, pattern, delta, count=1):
        sources = copy.deepcopy(self.sources)
        def replace(match):
            value = float(match.group(2)) + delta
            return match.group(1) + format(value, ".12g")
        sources[LEVEL_PATH], replaced = re.subn(
            pattern, replace, sources[LEVEL_PATH])
        self.assertEqual(replaced, count, "Synthetic mutation must reach its source field")
        reference = AUDIT.parse_reference_rendering(sources)
        self.assertNotEqual(reference, self.reference)
        return reference

    def camera(self, scene):
        cameras = [
            component for entity in scene["scene"]["entities"]
            for component in entity["components"]
            if component["type"] == "Camera" and component.get("primary")
        ]
        self.assertEqual(len(cameras), 1)
        return cameras[0]

    def test_real_pinned_requested_authoring_matches_current_public_documents(self):
        report = self.audit()
        self.assertEqual(report["scope"], "REQUESTED_AUTHORING_ONLY")
        shadows = self.reference["shadow"]
        self.assertEqual(shadows["map_size"], [512, 512])
        self.assertEqual(shadows["renderer_legacy_map_size"], [1024, 1024])
        self.assertNotEqual(shadows["map_size"], shadows["renderer_legacy_map_size"])
        differences = [row for row in report["policy_differences"]
                       if row["name"] == "shadow_application"]
        self.assertEqual(len(differences), 1)
        self.assertEqual(differences[0]["status"],
                         "DOCUMENTED_DIFFERENCE_NOT_PARITY_PASS")
        self.assertIsNone(differences[0]["pass"])
        self.assertIn("original", differences[0])
        self.assertIn("jce", differences[0])

    def test_synthetic_source_camera_fov_change_rejects_unchanged_scene(self):
        reference = self.synthetic_reference(
            r"(new\s+THREE\.PerspectiveCamera\(\s*)([\d.]+)", 1)
        with self.assertRaises(AUDIT.ParameterAuditError):
            self.audit(reference=reference)

    def test_synthetic_source_hsl_changes_reject_unchanged_sun(self):
        pattern = re.compile(
            r"(dirLight\.color\.setHSL\(\s*)([\d.]+)"
            r"(\s*,\s*)([\d.]+)(\s*,\s*)([\d.]+)")
        for channel, group in enumerate((2, 4, 6)):
            with self.subTest(synthetic_hsl_channel=channel):
                sources = copy.deepcopy(self.sources)
                matches = list(pattern.finditer(sources[LEVEL_PATH]))
                self.assertEqual(len(matches), 1)
                match = matches[0]
                start, end = match.span(group)
                changed = format(float(match.group(group)) - 0.01, ".12g")
                sources[LEVEL_PATH] = (
                    sources[LEVEL_PATH][:start] + changed + sources[LEVEL_PATH][end:])
                reference = AUDIT.parse_reference_rendering(sources)
                self.assertNotEqual(reference, self.reference)
                with self.assertRaises(AUDIT.ParameterAuditError):
                    self.audit(reference=reference)

    def test_synthetic_per_light_shadow_size_change_rejects_unchanged_pipeline(self):
        reference = self.synthetic_reference(
            r"(dirLight\.shadowMap(?:Width|Height)\s*=\s*)(\d+)", 256, count=2)
        self.assertEqual(reference["shadow"]["renderer_legacy_map_size"],
                         self.reference["shadow"]["renderer_legacy_map_size"])
        with self.assertRaises(AUDIT.ParameterAuditError):
            self.audit(reference=reference)

    def test_synthetic_scene_missing_camera_fov_is_not_a_defaulted_match(self):
        scene = copy.deepcopy(self.scene)
        del self.camera(scene)["fov"]
        with self.assertRaises(AUDIT.ParameterAuditError):
            self.audit(scene=scene)

    def test_synthetic_scene_black_hemisphere_ground_rejects(self):
        scene = copy.deepcopy(self.scene)
        scene["scene"]["rendering"]["look"]["groundColor"] = [0, 0, 0]
        with self.assertRaises(AUDIT.ParameterAuditError):
            self.audit(scene=scene)

    def test_synthetic_matching_rp_copies_missing_quality_reject(self):
        rp, rp_cooked = copy.deepcopy(self.rp), copy.deepcopy(self.rp_cooked)
        del rp["post_quality"]
        del rp_cooked["post_quality"]
        with self.assertRaises(AUDIT.ParameterAuditError):
            self.audit(rp=rp, rp_cooked=rp_cooked)

    def test_synthetic_matching_rp_copies_non_mid_quality_reject(self):
        for quality in ("low", "high", "ultra"):
            with self.subTest(synthetic_post_quality=quality):
                rp, rp_cooked = copy.deepcopy(self.rp), copy.deepcopy(self.rp_cooked)
                rp["post_quality"] = rp_cooked["post_quality"] = quality
                with self.assertRaises(AUDIT.ParameterAuditError):
                    self.audit(rp=rp, rp_cooked=rp_cooked)

    def test_synthetic_packaged_rp_copy_mismatch_rejects(self):
        rp_cooked = copy.deepcopy(self.rp_cooked)
        rp_cooked["render_scale"] = self.rp["render_scale"] / 2
        with self.assertRaises(AUDIT.ParameterAuditError):
            self.audit(rp_cooked=rp_cooked)

    def test_synthetic_settings_linear_color_space_rejects_gamma_reference(self):
        settings = copy.deepcopy(self.settings)
        settings["graphics"]["color_space"] = 1
        with self.assertRaises(AUDIT.ParameterAuditError):
            self.audit(settings=settings)

    def test_synthetic_classic_policy_change_requires_reaudit(self):
        # Preserve both recognized directional statements, then introduce a
        # conflicting final shadow operation. The old semantic witness expires.
        changed = self.classic + "\nvoid synthetic_final_shadow(inout vec3 finalColor, float shadow) { finalColor *= shadow; }\n"
        with self.assertRaises(AUDIT.ParameterAuditError):
            AUDIT.audit_requested_rendering(
                self.reference, self.scene, self.settings, self.rp,
                self.rp_cooked, changed)

    def test_synthetic_source_bytes_fail_actual_untouched_manifest_pin(self):
        real_read = Path.read_bytes
        level = REFERENCE_ROOT / LEVEL_PATH
        original = real_read(level)
        observed = []
        def altered_level_bytes(path):
            data = real_read(path)
            if path == level:
                observed.append(path)
                return data + b"\n// Synthetic in-memory provenance control.\n"
            return data
        with mock.patch.object(Path, "read_bytes", altered_level_bytes):
            with self.assertRaises(AUDIT.ParameterAuditError):
                AUDIT.load_pinned_sources(REFERENCE_ROOT, MANIFEST, SOURCE_PATHS)
        self.assertEqual(observed, [level])
        self.assertEqual(real_read(level), original)


if __name__ == "__main__":
    unittest.main()
