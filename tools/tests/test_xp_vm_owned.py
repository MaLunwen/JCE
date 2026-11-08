"""Pure typed report and fake-client controls for the owned XP child launcher."""
import ast
import copy
import hashlib
import json
from types import SimpleNamespace
import unittest
from unittest import mock

from tools.build import xp_vm_owned as owned
from tools.tests import test_xp_vm as controls

VM = controls.VM
DIRECTORY = "C:\\Documents and Settings\\Test User\\Desktop\\jce-xp-smoke-" + controls.NONCE


def configure_owned(fixture, *, stdout=b"", stderr=b""):
    """Author a report fixture; no native executable is launched."""
    fixture.args.kind = "editor"
    fixture.artifact.write_bytes(controls.sample_pe(subsystem=2))
    launcher = fixture.repo / "owned-launcher.exe"
    launcher.write_bytes(controls.sample_pe())
    fixture.args.owned_launcher = launcher
    fixture.args.export_receipt = controls.export_receipt(fixture.repo / "exports", fixture.artifact)
    target = DIRECTORY + "\\artifact-" + hashlib.sha256(fixture.artifact.read_bytes()).hexdigest() + ".exe"
    names = owned.output_names(controls.NONCE)
    report = dict(schema=owned.SCHEMA, nonce=controls.NONCE, target_path=target,
        working_dir=DIRECTORY, requested_desktop=owned.DESKTOP, controller_error=0,
        child=dict(created=True, pid=4242, birth_low=41, birth_high=23, birth_known=True,
            birth_stable=True, first_wait=0, final_wait=0, exit_known=True, exit_code=0,
            timed_out=False, terminate_attempted=False, terminate_ok=False, reaped=True,
            thread_closed=True, process_closed=True),
        streams=dict(stdout_name=names["stdout"], stderr_name=names["stderr"],
            stdout_bytes=len(stdout), stderr_bytes=len(stderr), stdout_closed=True,
            stderr_closed=True, output_limit_exceeded=False, output_limit_bytes=33554432,
            output_poll_ms=25, byte_limit_policy="observed-periodic-file-size; overshoot possible"),
        context=dict(user_sid="S-1-5-21-1000", token_type=1, session_id=0, query_error=0))
    fixture.client.transport.stdout = b""
    fixture.client.transport.stderr = b""
    fixture.client.transport.generated_files = {
        names["report"]: json.dumps(report).encode(), names["stdout"]: stdout, names["stderr"]: stderr}
    return report, names


def sync_report(fixture, report, names):
    fixture.client.transport.generated_files[names["report"]] = json.dumps(report).encode()


class OwnedLauncherTests(unittest.TestCase):
    def setUp(self):
        fixture = controls.XpVmTests(methodName="runTest")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.fixture = fixture
        self.report, self.names = configure_owned(fixture)
        self.args, self.client = fixture.args, fixture.client

    def run_smoke(self):
        return self.fixture.run_smoke()

    def test_owned_launcher_option_is_explicit_and_default_off(self):
        action = VM.build_parser()._option_string_actions.get("--owned-launcher")
        self.assertIsNotNone(action)
        self.assertIsNone(action.default)

    def test_public_forwarder_preserves_explicit_flags_and_omits_defaults(self):
        tree = ast.parse((controls.ROOT / "tools/build/jce.py").read_text(encoding="utf-8"))
        function = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "cmd_xp_vm")
        calls = []
        namespace = dict(sys=SimpleNamespace(executable="python"), ROOT=controls.ROOT,
                         _run_xp_tool=lambda label, command: calls.append(command))
        exec(compile(ast.Module(body=[function], type_ignores=[]), "actual-xp-vm-forwarder", "exec"), namespace)
        self.args.require_terminal_ack = True
        namespace["cmd_xp_vm"](self.args)
        command = calls.pop()
        self.assertIn("--owned-launcher", command)
        self.assertEqual(command[command.index("--owned-launcher") + 1], str(self.args.owned_launcher))
        self.assertEqual(command.count("--require-terminal-ack"), 1)
        self.assertNotIn(controls.PASSWORD, command)
        self.args.owned_launcher, self.args.require_terminal_ack = None, False
        namespace["cmd_xp_vm"](self.args)
        self.assertNotIn("--owned-launcher", calls[0])
        self.assertNotIn("--require-terminal-ack", calls[0])

    def test_owned_mode_audits_two_executables_and_uses_observed_child_identity(self):
        with mock.patch.object(VM, "_audit_native", wraps=VM._audit_native) as audit:
            result = self.run_smoke()
        self.assertTrue(result["passed"], result["state"])
        self.assertEqual(audit.call_count, 2)
        self.assertEqual(result["child_process"]["process_identity"]["pid"], 4242)
        self.assertEqual(result["child_process"]["process_identity"]["creation_time_windows_filetime"], str((23 << 32) | 41))
        self.assertIsNone(result["child_process"]["process_identity"]["creation_time_unix_s"])
        self.assertEqual(result["owned_launch"]["actual_child_desktop"], "UNOBSERVED")
        self.assertEqual(result["owned_launch"]["process_tree_cleanup"], "UNKNOWN")
        self.assertFalse(result["editor_graphics_verified"])
        self.assertEqual(set(result["collection"]["requested"]), set(self.names.values()))
        command = self.client.transport.commands[-1]
        self.assertIn('"--target" "' + self.report["target_path"] + '"', command)
        self.assertIn('"--desktop" "WinSta0\\Default"', command)
        self.assertIn('"--timeout-ms" "60000"', command)
        self.assertEqual(result["owned_launch"]["controller_exit_code"], 0)

    def test_owned_arguments_preserve_unicode_paths_and_refuse_cmd_syntax(self):
        directory = "C:\\Documents and Settings\\\u6d4b\u8bd5\\Desktop"
        target, helper = directory + "\\editor.exe", directory + "\\launcher.exe"
        arguments = owned.arguments(target, directory, controls.NONCE, 60)
        try:
            command = VM.launch_command(directory, helper, {}, arguments=arguments)
        except VM.SmokeError:
            self.fail("valid Unicode guest paths must survive owned command construction")
        self.assertIn(target, command)
        self.assertIn(directory, command)
        for value in ('bad"quote', "bad%PATH%", "bad!PATH!", "bad&command", "bad|command", "bad\ncommand"):
            with self.subTest(value=value), self.assertRaises(VM.SmokeError):
                VM.launch_command(directory, helper, {}, arguments=(value,))

    def test_child_nonzero_is_preserved_despite_successful_helper(self):
        self.report["child"]["exit_code"] = 9
        sync_report(self.fixture, self.report, self.names)
        result = self.run_smoke()
        self.assertFalse(result["passed"])
        self.assertEqual(result["state"], "NONZERO_EXIT")
        self.assertEqual(result["commands"]["artifact"]["exit_code"], 0)
        self.assertEqual(result["child_process"]["exit_code"], 9)

    def test_child_nonzero_survives_a_missing_stream_collection(self):
        self.report["child"]["exit_code"] = 9
        sync_report(self.fixture, self.report, self.names)
        del self.client.transport.generated_files[self.names["stderr"]]
        result = self.run_smoke()
        self.assertFalse(result["passed"])
        self.assertEqual(result["state"], "NONZERO_EXIT")
        self.assertEqual(result["child_process"]["exit_code"], 9)
        self.assertFalse(result["collection"]["verified"])

    def test_timeout_retains_targeted_termination_and_actual_child_exit(self):
        self.report["child"].update(first_wait=258, timed_out=True, terminate_attempted=True,
                                    terminate_ok=True, exit_code=92)
        sync_report(self.fixture, self.report, self.names)
        result = self.run_smoke()
        self.assertEqual(result["state"], "TIMEOUT")
        self.assertFalse(result["passed"])
        self.assertEqual(result["child_process"]["exit_code"], 92)
        self.assertTrue(result["owned_launch"]["report"]["child"]["terminate_attempted"])

    def test_controller_nonzero_cannot_be_reported_as_child_success(self):
        self.client.transport.exit_code = 7
        result = self.run_smoke()
        self.assertEqual(result["state"], "OWNED_LAUNCHER_NONZERO_EXIT")
        self.assertEqual(result["child_process"]["exit_code"], 0)
        self.assertFalse(result["passed"])

    def test_context_query_unavailability_stays_unknown_without_inventing_identity(self):
        self.report["context"] = dict(user_sid=None, token_type=None, session_id=None, query_error=5)
        sync_report(self.fixture, self.report, self.names)
        result = self.run_smoke()
        self.assertTrue(result["passed"], result["state"])
        self.assertEqual(result["owned_launch"]["report"]["context"], self.report["context"])
        self.assertEqual(result["owned_launch"]["actual_child_desktop"], "UNOBSERVED")

    def test_missing_report_refuses_with_no_child_exit_invention(self):
        del self.client.transport.generated_files[self.names["report"]]
        result = self.run_smoke()
        self.assertFalse(result["passed"])
        self.assertEqual(result["state"], "OWNED_LAUNCH_REPORT_REFUSED")
        self.assertNotIn("child_process", result)

    def test_missing_export_receipt_is_refused_before_connecting(self):
        self.args.export_receipt = None
        result = self.run_smoke()
        self.assertEqual(result["state"], "OWNED_LAUNCH_EXPORT_RECEIPT_REQUIRED")
        self.assertFalse(self.client.connected)

    def test_owned_launcher_is_editor_only(self):
        self.args.kind = "core-probe"
        result = self.run_smoke()
        self.assertEqual(result["state"], "OWNED_LAUNCH_EDITOR_REQUIRED")
        self.assertFalse(self.client.connected)

    def test_owned_editor_must_be_gui_subsystem(self):
        self.fixture.artifact.write_bytes(controls.sample_pe(subsystem=3))
        result = self.run_smoke()
        self.assertEqual(result["state"], "EDITOR_GUI_SUBSYSTEM_REQUIRED")
        self.assertFalse(self.client.connected)

    def test_report_nonce_target_desktop_types_and_unknown_fields_refuse(self):
        variants = []
        for key, value in (("nonce", "f" * 32), ("target_path", "C:\\foreign.exe"),
                           ("working_dir", "C:\\foreign"), ("requested_desktop", "Default"),
                           ("controller_error", False), ("extra", 1)):
            report = copy.deepcopy(self.report)
            report[key] = value
            variants.append(report)
        report = copy.deepcopy(self.report)
        report["child"]["pid"] = True
        variants.append(report)
        report = copy.deepcopy(self.report)
        report["child"]["reaped"] = 1
        variants.append(report)
        report = copy.deepcopy(self.report)
        report["streams"]["stdout_name"] = "foreign.bin"
        variants.append(report)
        for index, report in enumerate(variants):
            with self.subTest(index=index):
                self.fixture.client = controls.FakeClient()
                _, self.names = configure_owned(self.fixture)
                self.client = self.fixture.client
                self.args.out = "build/validation/refused-" + str(index)
                sync_report(self.fixture, report, self.names)
                result = self.run_smoke()
                self.assertFalse(result["passed"])
                self.assertEqual(result["state"], "OWNED_LAUNCH_REPORT_REFUSED")

    def test_incomplete_lifecycle_and_output_overshoot_never_pass(self):
        variants = (("child", "birth_stable", False), ("child", "first_wait", 258),
                    ("child", "reaped", False), ("child", "exit_known", False),
                    ("child", "thread_closed", False), ("child", "process_closed", False),
                    ("streams", "stderr_closed", False), ("streams", "output_limit_exceeded", True))
        for index, (section, key, value) in enumerate(variants):
            with self.subTest(key=key):
                self.fixture.client = controls.FakeClient()
                report, names = configure_owned(self.fixture)
                report[section][key] = value
                self.args.out = "build/validation/lifecycle-" + str(index)
                sync_report(self.fixture, report, names)
                result = self.run_smoke()
                self.assertFalse(result["passed"], key)
                self.assertNotEqual(result["state"], "SMOKE_PASSED")

    def test_substituted_valid_report_after_collection_is_refused(self):
        actual = VM.collect_outputs
        def substitute(*args, **kwargs):
            collection = actual(*args, **kwargs)
            path = self.fixture.output() / "collected" / self.names["report"]
            path.write_text(json.dumps(self.report, indent=1), encoding="utf-8")
            return collection
        with mock.patch.object(VM, "collect_outputs", side_effect=substitute):
            result = self.run_smoke()
        self.assertFalse(result["passed"])
        self.assertEqual(result["state"], "OWNED_LAUNCH_REPORT_REFUSED")

    def test_duplicate_json_key_is_refused(self):
        raw = json.dumps(self.report).encode()
        self.client.transport.generated_files[self.names["report"]] = b'{"nonce":"foreign",' + raw[1:]
        result = self.run_smoke()
        self.assertFalse(result["passed"])
        self.assertEqual(result["state"], "OWNED_LAUNCH_REPORT_REFUSED")


if __name__ == "__main__":
    unittest.main()
