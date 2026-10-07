"""Reject silent feature toggles, blank output and ineffective post passes."""
import unittest

from postfx_fixture import EFFECTS
from postfx_suite import validate_images, validate_log


class PostFXControls(unittest.TestCase):
    def test_transition_requires_both_exact_feature_states_in_order(self):
        first = "stock-host: effect=ssao taa=0 ssao=1 ssr=0 bloom=0 fog=0\n"
        second = "stock-host: effect=ssr taa=0 ssao=0 ssr=1 bloom=0 fog=0\n"
        views = "".join(f"stock-view: frame=120 id={vid} name={name} at host.c:2\n"
                        for vid, name in ((30, "Scene/Color"), (48, "SSR"), (49, "Composite")))
        self.assertEqual(validate_log(first + second + views, "ssr", before="ssao")[48], "SSR")
        for invalid in (second + views, second + first + views, first + first + second + views):
            with self.assertRaises(ValueError):
                validate_log(invalid, "ssr", before="ssao")

    def test_live_views_and_exact_gates(self):
        log = ("stock-host: effect=taa taa=1 ssao=0 ssr=0 bloom=0 fog=0 at host.c:1\n"
               "stock-view: frame=120 id=30 name=Scene/Color at host.c:2\n"
               "stock-view: frame=120 id=103 name=PostFX/TAA_Resolve at host.c:2\n")
        self.assertEqual(validate_log(log, "taa")[103], "PostFX/TAA_Resolve")
        for invalid in (log.replace("TAA_Resolve", "NoResolve"),
                        log.replace("ssao=0", "ssao=1"), log + "] ERROR - missing input"):
            with self.assertRaises(ValueError):
                validate_log(invalid, "taa")

    def test_blank_unchanged_and_noisy_pictures_fail(self):
        def image(value):
            return (1280, 720, 3, bytearray([value, value, value]) * (1280 * 720))
        images = {effect: image(100 + i * 10) for i, effect in enumerate(EFFECTS)}
        images.update(empty=image(0), repeat=image(100))
        self.assertEqual(validate_images(images)["repeat_fraction"], 0)
        for key, replacement in (("ssr", "base"), ("base", "empty"), ("repeat", "bloom")):
            invalid = dict(images)
            invalid[key] = images[replacement]
            with self.assertRaises(ValueError):
                validate_images(invalid)


if __name__ == "__main__":
    unittest.main()
