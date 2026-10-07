import unittest

from report_static_asset import validate_inventory, validate_pair


class StaticReportTests(unittest.TestCase):
    def rows(self):
        return [{"backend": backend, "executable_sha256": "same", "asset_sha256": "original"}
                for backend in ("d3d11", "d3d12", "vulkan", "gl")]

    def test_missing_duplicate_or_mixed_identity_matrix_is_rejected(self):
        validate_inventory(self.rows())
        for rows in (self.rows()[:-1], self.rows()[:3] + self.rows()[:1]):
            with self.assertRaises(ValueError): validate_inventory(rows)
        for key in ("executable_sha256", "asset_sha256"):
            rows = self.rows()
            rows[0][key] = "different"
            with self.assertRaises(ValueError): validate_inventory(rows)

    def test_pair_rejects_changed_scene_asset_topology_and_pixels(self):
        old = {**self.rows()[0], "census": {"vertices": 100}}
        picture = (2, 2, 4, bytearray([10, 10, 10, 255] * 4))
        self.assertEqual(0, validate_pair(old, dict(old), {}, {}, picture, picture))
        for key, value in (("backend", "gl"), ("asset_sha256", "edited"),
                           ("census", {"vertices": 101})):
            after = {**old, key: value}
            with self.assertRaises(ValueError): validate_pair(old, after, {}, {}, picture, picture)
        with self.assertRaises(ValueError):
            validate_pair(old, dict(old), {"camera": 1}, {"camera": 2}, picture, picture)
        with self.assertRaises(ValueError):
            validate_pair(old, dict(old), {}, {}, picture,
                          (2, 2, 4, bytearray([90, 90, 90, 255] * 4)))


if __name__ == "__main__":
    unittest.main()
