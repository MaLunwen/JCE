"""Negative controls for base static model readiness and source census."""
import json
import struct
import unittest

from static_asset_suite import fixture, glb_census, pixels_pass, validate_import


def glb(document):
    content = json.dumps(document).encode()
    content += b" " * (-len(content) % 4)
    return struct.pack("<IIIII", 0x46546c67, 2, 20 + len(content), len(content), 0x4e4f534a) + content


def observations():
    return "benchmark-asset: still controls taa=0 ssao=0 ssr=0 motion_blur=0\n" + \
        "benchmark-asset: ready elapsed_ms=11000 frame=0\n" + "".join(
            "benchmark-asset: frame=%u meshes=1 animators=0 vertices=120 primitives=2 "
            "joints=0 clips=0 player=0 time=0.000000 gpu_bytes=1000\n" % frame for frame in (20, 50))


class StaticAssetTests(unittest.TestCase):
    def test_source_node_references_are_not_unique_mesh_counts(self):
        doc = {"nodes": [{"mesh": 0}, {"mesh": 0}],
               "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
               "accessors": [{"count": 60}]}
        census = glb_census(glb(doc))
        self.assertEqual((census["primitives"], census["vertices"]), (2, 120))
        for bad in (b"", glb(doc)[:-1], glb({**doc, "skins": [{}]}),
                    glb({**doc, "extensionsRequired": ["unsupported_required_extension"]})):
            with self.assertRaises(ValueError):
                glb_census(bad)

    def test_missing_duplicate_late_readiness_and_wrong_topology_fail(self):
        log = observations()
        census = {"vertices": 120, "primitives": 2}
        self.assertEqual(len(validate_import(log, census, True)), 2)
        for bad in (log.replace("ready elapsed_ms=11000", "pending elapsed_ms=11000"),
                    log + "benchmark-asset: ready elapsed_ms=1 frame=0\n",
                    log.replace("elapsed_ms=11000", "elapsed_ms=60000"),
                    log.replace("frame=0", "frame=50"), log.replace("vertices=120", "vertices=60")):
            with self.assertRaises(ValueError):
                validate_import(bad, census, True)
        with self.assertRaises(ValueError):
            validate_import(log, census, False)

    def test_empty_unchanged_and_noisy_images_cannot_pass(self):
        good = {"coverage": .2, "frame20_50": 0, "repeat": 0}
        self.assertTrue(pixels_pass(good))
        for bad in ({}, {**good, "coverage": 0}, {**good, "frame20_50": .1},
                    {**good, "repeat": .1}, {**good, "repeat": float("nan")}):
            self.assertFalse(pixels_pass(bad))

    def test_framing_and_empty_controls_are_consumer_inputs(self):
        doc = fixture("unchanged.glb", .5, (0, .7, 1), -32.5, True)
        self.assertEqual(len(doc["entities"]), 3)
        self.assertEqual(doc["entities"][0]["components"][0]["rotX"], -32.5)
        self.assertEqual(doc["entities"][2]["components"][0]["scaleX"], .5)
        self.assertEqual(len(fixture("unchanged.glb", 1, (0, 1, 2), 0, False)["entities"]), 2)


if __name__ == "__main__":
    unittest.main()
