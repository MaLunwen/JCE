import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SKY_SHADER = ROOT / "engine" / "shaders" / "standard" / "fs_sky.sc"


class SkyDomeContractTests(unittest.TestCase):
    def test_origin_anchored_dome_falls_back_near_or_outside_shell(self):
        source = SKY_SHADER.read_text(encoding="utf-8")

        self.assertRegex(source, r"vec3\s+viewDir\s*=\s*dir;")
        self.assertIn("float anchorWeight", source)
        self.assertIn("smoothstep(R * 0.75, R * 0.95", source)
        self.assertIn("disc > 0.0", source)
        self.assertIn("camRadius2 < R * R", source)
        self.assertNotIn("sqrt(max(disc, 0.0))", source)


if __name__ == "__main__":
    unittest.main()
