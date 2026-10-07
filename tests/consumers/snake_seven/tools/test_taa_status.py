"""Actual pipeline marker sequences cannot let Scene status decide Game parity."""
import unittest
from taa_status import resolve_taa


def marker(state, view=None):
    suffix = " view_base=%d" % view if view is not None else ""
    return "2026-10-08 [MAIN] WARN - postfx: taa resolve: %s%s at jce_postfx.c:1478\n" % (state,suffix)

class TaaStatusTests(unittest.TestCase):
    def test_editor_only_game150_and_its_last_submission_state(self):
        log = marker("off",33)+marker("on",150)+marker("off",33)
        result = resolve_taa(log,"editor")
        self.assertEqual(result["state"],"on")
        self.assertEqual(result["selected_view"],150)
        self.assertFalse(result["legacy"])
        self.assertEqual(resolve_taa(log+marker("off",150)+marker("on",33),"editor")["state"],"off")
    def test_native100_wins_over_later_default20(self):
        result = resolve_taa(marker("on",100)+marker("off",20)+marker("off",33),"native")
        self.assertEqual((result["state"],result["selected_view"]),("on",100))
        result = resolve_taa(marker("on",20)+marker("off",20),"native")
        self.assertEqual((result["state"],result["selected_view"]),("off",20))
    def test_scene_only_or_other_host_marker_is_not_game_evidence(self):
        for log, host in ((marker("on",33),"editor"),(marker("on",33),"native"),
                          (marker("on",100),"editor"),(marker("on",150),"native")):
            with self.assertRaises(ValueError): resolve_taa(log,host)
    def test_request_without_submission_cannot_become_on_or_silent_off(self):
        request = "postfx: taa requested: on view_base=150\n"
        with self.assertRaises(ValueError): resolve_taa(request,"editor")
        self.assertEqual(resolve_taa(request+marker("off",150),"editor")["state"],"off")
    def test_only_complete_legacy_log_can_use_declared_unscoped_fallback(self):
        result = resolve_taa(marker("on")+marker("off"),"editor")
        self.assertEqual(result["state"],"off")
        self.assertTrue(result["legacy"])
        self.assertIsNone(result["selected_view"])
        self.assertIn("viewport cannot be proven",result["scope"])
        for log in (marker("on")+marker("off",33),marker("on",150)+marker("off")):
            with self.assertRaises(ValueError): resolve_taa(log,"editor")
    def test_malformed_identity_and_unknown_host_fail(self):
        for value in ("postfx: taa resolve: on view_base=garbage", "postfx: taa resolve: off view_base=-1"):
            with self.assertRaises(ValueError): resolve_taa(value,"editor")
        with self.assertRaises(ValueError): resolve_taa(marker("on",150),"scene")

if __name__ == "__main__":
    unittest.main()
