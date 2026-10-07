"""CPU controls reject missing or geometry-only shadow evidence."""
import unittest

import numpy as np

from shadow_suite import CASES, HEIGHT, WIDTH, primitive_environment, scene, validate


def synthetic():
    images = {label: np.full((HEIGHT, WIDTH, 4), 150, np.uint8) for label in CASES}
    for pixels in images.values():
        pixels[:, :, 3] = 255
    for label in ("off", "on", "repeat", "moved-off", "moved-on"):
        images[label][100:200, 100:200, :3] = 200
    for label in ("on", "repeat", "moved-on"):
        images[label][300:400, 300:400, :3] = 80
    for label in ("moved-off", "moved-on"):
        images[label][100:200, 100:220, :3] = 200
    images["moved-on"][300:400, 300:420, :3] = 80
    return images


class ShadowControls(unittest.TestCase):
    def test_instancing_control_is_explicit_and_default_inert(self):
        self.assertEqual(primitive_environment(None), {})
        self.assertEqual(primitive_environment("on"), {"JCE_PRIM_INSTANCE": "1"})
        self.assertEqual(primitive_environment("off"), {"JCE_PRIM_INSTANCE": "0"})
        with self.assertRaises(ValueError):
            primitive_environment("auto")

    def test_default_caster_contrast_preserves_geometric_control(self):
        value = scene("on")
        receiver = value["entities"][2]["components"][1]
        channels = ("baseColorR", "baseColorG", "baseColorB")
        receiver_rgb = tuple(receiver[key] for key in channels)
        for entity in value["entities"][3:]:
            transform, material = entity["components"]
            self.assertNotEqual(tuple(material[key] for key in channels), receiver_rgb)
            self.assertEqual((transform["scaleX"], transform["scaleY"], transform["scaleZ"]), (.8, 2, .8))
            self.assertEqual(material["roughness"], receiver["roughness"])
            self.assertEqual(material["metallic"], receiver["metallic"])
        a, b = scene("off"), scene("moved-off")
        self.assertEqual([i for i, pair in enumerate(zip(a["entities"], b["entities"])) if pair[0] != pair[1]], [30])

    def test_authored_population_and_fixed_settings(self):
        for label in CASES:
            value = scene(label)
            self.assertEqual(len(value["entities"]), 3 if label.startswith("empty") else 67)
            self.assertEqual(value["rendering"]["shadows"]["cascades"], 2)
            self.assertEqual(value["rendering"]["shadows"]["resolution"], 1024)
        a, b = scene("on"), scene("moved-on")
        changed = [i for i, pair in enumerate(zip(a["entities"], b["entities"]))
                   if pair[0] != pair[1]]
        self.assertEqual(changed, [30])

    def test_oblique_camera_and_material_controls_preserve_population(self):
        original = scene("on")
        changed = scene("on", camera_mode="perspective", distinct_casters=True)
        self.assertEqual(len(changed["entities"]), len(original["entities"]))
        transform, camera = changed["entities"][0]["components"]
        self.assertEqual((transform["posY"], transform["posZ"], transform["rotX"]),
                         (25, 25, -45))
        self.assertFalse(camera["orthographic"])
        self.assertEqual((camera["fov"], camera["farClip"]), (55, 90))
        self.assertEqual(changed["entities"][2], original["entities"][2])
        for caster in changed["entities"][3:]:
            self.assertEqual(caster["components"][1]["baseColorR"], .2)
            self.assertNotIn("emissiveR", caster["components"][1])
        self.assertEqual(changed["rendering"], original["rendering"])
        with self.assertRaises(ValueError):
            scene("on", camera_mode="bad")

    def test_low_sun_rigid_receiver_tilt_preserves_contact_geometry(self):
        tilted = scene("on", sun_direction=(-1, -.2, -.3), receiver_tilt_deg=30)
        self.assertEqual(tilted["entities"][1]["components"][1]["dirY"], -.2)
        floor = tilted["entities"][2]["components"][0]
        self.assertAlmostEqual(floor["posX"], .1)
        self.assertAlmostEqual(floor["posY"], -.17320508075688773)
        self.assertEqual(floor["rotZ"], 30)
        for entity in tilted["entities"][3:]:
            self.assertEqual(entity["components"][0]["rotZ"], 30)
        empty = scene("empty-on", receiver_tilt_deg=30)
        self.assertEqual(len(empty["entities"]), 3)
        self.assertEqual(empty["entities"][2], tilted["entities"][2])
        repeated = scene("repeat", sun_direction=(-1, -.2, -.3), receiver_tilt_deg=30)
        self.assertEqual(repeated, tilted)
        moved = scene("moved-on", sun_direction=(-1, -.2, -.3), receiver_tilt_deg=30)
        first = tilted["entities"][30]["components"][0]
        second = moved["entities"][30]["components"][0]
        self.assertAlmostEqual(second["posX"]-first["posX"], 1.0392304845413265)
        self.assertAlmostEqual(second["posY"]-first["posY"], .6)
        for kwargs in ({"sun_direction": (0, 0, 0)},
                       {"sun_direction": (float("nan"), 1, 0)},
                       {"receiver_tilt_deg": 31}):
            with self.assertRaises(ValueError):
                scene("on", **kwargs)

    def test_negative_pixel_controls(self):
        images = synthetic()
        self.assertGreater(validate(images)["darkened_fraction"], .001)
        for destination, source in (("on", "off"), ("repeat", "off"),
                                   ("empty-on", "on"), ("moved-on", "on")):
            wrong = {key: value.copy() for key, value in images.items()}
            wrong[destination] = wrong[source].copy()
            with self.assertRaises(ValueError):
                validate(wrong)
        wrong = {key: value.copy() for key, value in images.items()}
        wrong["moved-on"] = wrong["on"].copy()
        wrong["moved-on"][100:200, 100:220, :3] = 200
        with self.assertRaises(ValueError):
            validate(wrong)
        with self.assertRaises(ValueError):
            validate(dict(images, off=np.zeros((1, 1, 4), np.uint8)))
        with self.assertRaises(ValueError):
            validate({key: value for key, value in images.items() if key != "repeat"})


if __name__ == "__main__":
    unittest.main()
