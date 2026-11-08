"""Pure explicit Editor window-mode controls; no windows or SSH are opened."""
import unittest
from tools.tests import test_xp_vm as controls

VM = controls.VM


class XpVmWindowTests(unittest.TestCase):
    def test_explicit_window_mode_is_available_and_omission_remains_hidden(self):
        action = VM.build_parser()._option_string_actions.get("--editor-window")
        self.assertIsNotNone(action)
        self.assertEqual(set(action.choices), {"hidden", "visible", "interactive"})
        self.assertIsNone(action.default)
        env = VM.environment([], "editor", 60)
        self.assertEqual(env["JCE_WINDOW_HIDDEN"], "1")
        self.assertEqual(env["JCE_WINDOW_NOT_FOCUSABLE"], "1")
        self.assertEqual(VM.environment([], "editor", 60, editor_window="hidden"), env)

    def test_visible_opt_in_keeps_window_non_focusable(self):
        env = VM.environment([], "editor", 60, editor_window="visible")
        self.assertEqual(env["JCE_WINDOW_HIDDEN"], "0")
        self.assertEqual(env["JCE_WINDOW_NOT_FOCUSABLE"], "1")
        self.assertLess(float(env["JCE_MAX_SECONDS"]), 60)
        self.assertEqual(VM.environment(["JCE_WINDOW_HIDDEN=0"], "editor", 60, editor_window="visible"), env)

    def test_interactive_opt_in_explicitly_allows_human_focus(self):
        try:
            env = VM.environment([], "editor", 60, editor_window="interactive")
        except VM.SmokeError:
            self.fail("explicit interactive Editor mode must be supported")
        self.assertEqual(env["JCE_WINDOW_HIDDEN"], "0")
        self.assertEqual(env["JCE_WINDOW_NOT_FOCUSABLE"], "0")

    def test_interactive_contradictory_environment_flags_refuse(self):
        for values in (["JCE_WINDOW_HIDDEN=1"], ["JCE_WINDOW_NOT_FOCUSABLE=1"]):
            with self.subTest(values=values), self.assertRaisesRegex(VM.SmokeError, "EDITOR_WINDOW_POLICY_MISMATCH"):
                VM.environment(values, "editor", 60, editor_window="interactive")
        expected = VM.environment([], "editor", 60, editor_window="interactive")
        self.assertEqual(VM.environment(["JCE_WINDOW_HIDDEN=0", "JCE_WINDOW_NOT_FOCUSABLE=0"],
                                       "editor", 60, editor_window="interactive"), expected)

    def test_parser_accepts_explicit_interactive_without_changing_omission(self):
        base = ["--host", "fixture", "--user", "fixture", "--host-key-sha256", controls.PIN,
                "--artifact", "fixture.exe", "--expected-arch", "x64", "--kind", "editor",
                "--out", "build/validation/fixture"]
        parser = VM.build_parser()
        self.assertIsNone(parser.parse_args(base).editor_window)
        self.assertEqual(parser.parse_args(base + ["--editor-window", "interactive"]).editor_window, "interactive")

    def test_inconsistent_environment_cannot_bypass_selected_window_mode(self):
        cases = ((None, ["JCE_WINDOW_HIDDEN=0"]),
                 ("hidden", ["JCE_WINDOW_HIDDEN=0"]),
                 ("visible", ["JCE_WINDOW_HIDDEN=1"]),
                 ("visible", ["JCE_WINDOW_NOT_FOCUSABLE=0"]),
                 ("hidden", ["JCE_WINDOW_NOT_FOCUSABLE=0"]))
        for mode, values in cases:
            with self.subTest(mode=mode, values=values), self.assertRaises(VM.SmokeError):
                VM.environment(values, "editor", 60, editor_window=mode)

    def test_both_explicit_modes_are_editor_only_and_unknown_modes_refuse(self):
        for kind in ("core-probe", "native-probe"):
            self.assertEqual(VM.environment([], kind, 60), {})
            for mode in ("hidden", "visible", "interactive"):
                with self.subTest(kind=kind, mode=mode), self.assertRaisesRegex(VM.SmokeError, "EDITOR_WINDOW_EDITOR_REQUIRED"):
                    VM.environment([], kind, 60, editor_window=mode)
        for mode in ("unknown", "", 1, True):
            with self.subTest(mode=mode), self.assertRaisesRegex(VM.SmokeError, "INVALID_EDITOR_WINDOW_MODE"):
                VM.environment([], "editor", 60, editor_window=mode)

    def test_fake_transport_receives_explicit_visible_policy(self):
        fixture = controls.XpVmTests(methodName="runTest")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        fixture.args.kind, fixture.args.editor_window = "editor", "visible"
        result = fixture.run_smoke()
        self.assertTrue(result["passed"], result["state"])
        self.assertEqual(result["editor_window_request"], "visible")
        command = fixture.client.transport.commands[-1]
        self.assertIn('set "JCE_WINDOW_HIDDEN=0"', command)
        self.assertIn('set "JCE_WINDOW_NOT_FOCUSABLE=1"', command)
        self.assertFalse(result["editor_graphics_verified"])

    def test_fake_transport_receives_interactive_request_without_graphics_claim(self):
        fixture = controls.XpVmTests(methodName="runTest")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        fixture.args.kind, fixture.args.editor_window = "editor", "interactive"
        result = fixture.run_smoke()
        self.assertTrue(result["passed"], result["state"])
        self.assertEqual(result["editor_window_request"], "interactive")
        command = fixture.client.transport.commands[-1]
        self.assertIn('set "JCE_WINDOW_HIDDEN=0"', command)
        self.assertIn('set "JCE_WINDOW_NOT_FOCUSABLE=0"', command)
        self.assertFalse(result["editor_graphics_verified"])

    def test_non_editor_cli_request_refuses_before_connection(self):
        fixture = controls.XpVmTests(methodName="runTest")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        fixture.args.editor_window = "visible"
        result = fixture.run_smoke()
        self.assertEqual(result["state"], "EDITOR_WINDOW_EDITOR_REQUIRED")
        self.assertFalse(fixture.client.connected)


if __name__ == "__main__":
    unittest.main()
