"""Pure controls for XP transport terminal acknowledgement; no SSH/native runs."""
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import os
import json
from types import SimpleNamespace
import unittest
from unittest import mock

from tools import terminal_ack_observation as terminal
from tools.tests import test_xp_vm as controls
from tools.tests import test_xp_vm_owned as owned_controls
from tools.tests.test_terminal_ack_observation import encoded, packet, trace_document

VM = controls.VM
TOKEN = "0123456789abcdef0123456789abcdef"


class XpVmTerminalTests(unittest.TestCase):
    def setUp(self):
        fixture = controls.XpVmTests(methodName="runTest")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.fixture = fixture
        self.args = fixture.args
        self.args.kind = "editor"
        self.args.require_terminal_ack = True
        self.client = fixture.client
        self.report, self.names = owned_controls.configure_owned(fixture, stderr=encoded(packet(token=TOKEN)))

    def run_smoke(self, password=controls.PASSWORD):
        with mock.patch.object(VM.uuid, "uuid4", return_value=SimpleNamespace(hex=TOKEN)):
            return self.fixture.run_smoke(password=password)

    def test_cli_is_default_off_and_only_accepts_boolean_switch(self):
        action = VM.build_parser()._option_string_actions.get("--require-terminal-ack")
        self.assertIsNotNone(action)
        self.assertIs(action.default, False)
        self.assertEqual(action.nargs, 0)

    def test_default_and_old_namespace_preserve_original_result_and_environment(self):
        del self.args.require_terminal_ack
        self.args.owned_launcher = None
        with mock.patch.object(terminal, "observe_terminal_ack", wraps=terminal.observe_terminal_ack) as observer:
            result = self.run_smoke()
        self.assertTrue(result["passed"])
        self.assertNotIn("terminal_ack_required", result)
        self.assertNotIn("terminal_ack_state", result)
        self.assertNotIn("terminal_ack", result)
        self.assertNotIn("JCE_TERMINAL_ACK_TOKEN", self.client.transport.commands[-1])
        observer.assert_not_called()

    def test_success_uses_original_owned_stderr_and_canonical_observer_once(self):
        with mock.patch.object(terminal, "observe_terminal_ack", wraps=terminal.observe_terminal_ack) as observer:
            result = self.run_smoke()
        self.assertTrue(result["passed"], result)
        self.assertTrue(result["terminal_ack_required"])
        self.assertEqual(result["terminal_ack_state"], "OBSERVED")
        self.assertEqual(result["terminal_ack"]["status"], "HOST_TERMINAL_ACK_OBSERVED")
        self.assertEqual(result["terminal_ack"]["packet"]["token"], TOKEN)
        self.assertEqual(result["terminal_ack"]["trace_binding"], "not_requested")
        raw_path = self.fixture.output() / "collected" / self.names["stderr"]
        self.assertEqual(raw_path.read_bytes(), self.client.transport.generated_files[self.names["stderr"]])
        self.assertEqual(result["terminal_ack"]["stderr"]["sha256"], hashlib.sha256(raw_path.read_bytes()).hexdigest())
        self.assertIn('set "JCE_TERMINAL_ACK_TOKEN=' + TOKEN + '"', self.client.transport.commands[-1])
        self.assertEqual(observer.call_count, 1)
        self.assertEqual(observer.call_args.kwargs["expected_stderr_path"], raw_path)
        self.assertFalse(result["editor_graphics_verified"])
        self.assertEqual(result["remote_process_cleanup"], "OWNED_CHILD_REAPED_HANDLES_CLOSED_PROCESS_TREE_UNVERIFIED")
        self.assertEqual(result["owned_launch"]["process_tree_cleanup"], "UNKNOWN")

    def test_generic_environment_still_rejects_terminal_token_and_secrets(self):
        for name in ("JCE_TERMINAL_ACK_TOKEN", "JCE_TOKEN", "JCE_SECRET", "JCE_PASSWORD", "JCE_KEY"):
            with self.subTest(name=name), self.assertRaises(VM.SmokeError):
                VM.environment([name + "=" + TOKEN], "editor", 60)

    def test_canonical_adapter_requires_observed_pid_and_retains_exact_filetime(self):
        out = self.fixture.repo / "adapter"
        out.mkdir()
        raw = encoded(packet(token=TOKEN))
        (out / "artifact.stderr.bin").write_bytes(raw)
        result = dict(state="EXITED", exit_code=0, output_truncated=False, stderr=raw)
        with self.assertRaisesRegex(VM.SmokeError, "TERMINAL_ACK_REFUSED"):
            VM._observe_terminal((terminal, TOKEN, None), result, out, dict(stderr_redacted=False))
        identity = dict(pid=4242, creation_time_unix_s=None,
                        creation_time_source="unavailable_remote_psutil",
                        creation_time_filetime="133000000000000001")
        result["process_identity"] = identity
        observed = VM._observe_terminal((terminal, TOKEN, None), result, out, dict(stderr_redacted=False))
        self.assertEqual(observed["process_identity"], identity)
        self.assertEqual(observed["status"], "HOST_TERMINAL_ACK_OBSERVED")

    def test_owned_identity_canonical_packet_refusal_controls(self):
        out = self.fixture.repo / "adapter-packets"
        out.mkdir()
        variants = (b"SDL safe shutdown fallback\n", encoded(packet(token="f" * 32)),
                    encoded(packet()) * 2, encoded(packet())[:-1])
        for raw in variants:
            with self.subTest(raw=raw):
                (out / "artifact.stderr.bin").write_bytes(raw)
                result = dict(state="EXITED", exit_code=0, output_truncated=False, stderr=raw,
                              process_identity=dict(pid=4242, creation_time_unix_s=None,
                                                    creation_time_source="unavailable_remote_psutil"))
                with self.assertRaisesRegex(VM.SmokeError, "TERMINAL_ACK_REFUSED"):
                    VM._observe_terminal((terminal, TOKEN, None), result, out, dict(stderr_redacted=False))

    def test_canonically_valid_trace_substitution_after_collection_is_refused(self):
        out = self.fixture.repo / "adapter-trace"
        out.mkdir()
        raw = encoded(packet(token=TOKEN, cleanup_scope="10", trace_status="complete",
                             trace_published=True, trace_write_ok=True))
        (out / "artifact.stderr.bin").write_bytes(raw)
        trace = out / "trace.json"
        document = trace_document()
        original = json.dumps(document).encode()
        trace.write_bytes(json.dumps(document, indent=1).encode())
        result = dict(state="EXITED", exit_code=0, output_truncated=False, stderr=raw,
                      process_identity=dict(pid=4242, creation_time_unix_s=None,
                                            creation_time_source="unavailable_remote_psutil"))
        collection = dict(files=[dict(name=trace.name, verified=True, bytes_read=len(original),
                                     source_sha256=hashlib.sha256(original).hexdigest())])
        with self.assertRaisesRegex(VM.SmokeError, "TERMINAL_ACK_REFUSED"):
            VM._observe_terminal((terminal, TOKEN, trace), result, out, dict(stderr_redacted=False), collection)

    def test_opt_in_requires_independently_observed_owned_child_mode(self):
        self.args.owned_launcher = None
        result = self.run_smoke()
        self.assertFalse(result["passed"] or self.client.connected)
        self.assertEqual(result["state"], "TERMINAL_ACK_OWNED_LAUNCH_REQUIRED")

    def test_non_editor_opt_in_is_refused_before_connection(self):
        self.args.owned_launcher = None
        self.args.kind = "core-probe"
        result = self.run_smoke()
        self.assertFalse(result["passed"] or self.client.connected)
        self.assertEqual(result["state"], "TERMINAL_ACK_EDITOR_REQUIRED")

    def test_non_boolean_opt_in_is_refused_before_connection(self):
        self.args.require_terminal_ack = 1
        result = self.run_smoke()
        self.assertFalse(result["passed"] or self.client.connected)
        self.assertEqual(result["state"], "INVALID_TERMINAL_ACK_REQUEST")

    def test_missing_wrong_duplicate_and_truncated_packets_cannot_pass(self):
        variants = (b"SDL safe shutdown fallback\n", encoded(packet(token="f" * 32)),
                    encoded(packet()) * 2, encoded(packet())[:-1])
        for index, raw in enumerate(variants):
            with self.subTest(raw=raw):
                self.client = controls.FakeClient()
                self.fixture.client = self.client
                self.args.out = "build/validation/run-" + str(index)
                self.report, self.names = owned_controls.configure_owned(self.fixture, stderr=raw)
                result = self.run_smoke()
                self.assertFalse(result["passed"], result)
                self.assertEqual(result["state"], "TERMINAL_ACK_REFUSED")
                self.assertEqual(result["terminal_ack_state"], "REFUSED")
                self.assertEqual(result["commands"]["artifact"]["exit_code"], 0)
                self.assertNotIn("terminal_ack", result)

    def test_nonzero_actual_exit_is_preserved_without_calling_observer(self):
        self.report["child"]["exit_code"] = 9
        owned_controls.sync_report(self.fixture, self.report, self.names)
        with mock.patch.object(terminal, "observe_terminal_ack", wraps=terminal.observe_terminal_ack) as observer:
            result = self.run_smoke()
        self.assertEqual(result["state"], "NONZERO_EXIT")
        self.assertEqual(result["commands"]["artifact"]["exit_code"], 0)
        self.assertEqual(result["child_process"]["exit_code"], 9)
        self.assertEqual(result["terminal_ack_state"], "NOT_OBSERVED")
        observer.assert_not_called()

    def test_output_limit_preserves_transport_failure_without_ack_pass(self):
        self.args.max_output_bytes = 128
        result = self.run_smoke()
        self.assertFalse(result["passed"])
        self.assertEqual(result["state"], "OUTPUT_LIMIT")
        self.assertEqual(result["terminal_ack_state"], "NOT_OBSERVED")

    def test_redacted_stderr_cannot_substitute_for_original(self):
        result = self.run_smoke(password="engine_tail_complete")
        self.assertFalse(result["passed"])
        self.assertEqual(result["state"], "OWNED_CHILD_STREAMS_UNVERIFIED")
        entry = next(item for item in result["collection"]["files"] if item["name"] == self.names["stderr"])
        self.assertTrue(entry["credential_redacted"])
        self.assertNotIn(b"engine_tail_complete", (self.fixture.output() / "collected" / self.names["stderr"]).read_bytes())

    def test_unavailable_canonical_observation_refuses_without_persisting_diagnostics(self):
        with mock.patch.object(terminal, "observe_terminal_ack", side_effect=OSError(controls.PASSWORD)):
            result = self.run_smoke()
        self.assertFalse(result["passed"])
        self.assertEqual(result["state"], "TERMINAL_ACK_REFUSED")
        self.assertNotIn(controls.PASSWORD, json.dumps(result))

    def test_exited_zero_truncated_stream_is_refused_before_canonical_observer(self):
        actual_command = VM.command
        def truncated(client, text, *args, **kwargs):
            result = actual_command(client, text, *args, **kwargs)
            if 'start ""' in text:
                result["output_truncated"] = True
            return result
        with mock.patch.object(VM, "command", side_effect=truncated), \
             mock.patch.object(terminal, "observe_terminal_ack", wraps=terminal.observe_terminal_ack) as observer:
            result = self.run_smoke()
        self.assertFalse(result["passed"])
        self.assertEqual(result["state"], "OUTPUT_LIMIT")
        self.assertEqual(result["commands"]["artifact"]["exit_code"], 0)
        observer.assert_not_called()

    def test_short_password_token_overlap_regenerates_bounded_public_witness(self):
        safe = "f" * 32
        raw = encoded(packet(token=safe))
        self.client.transport.generated_files[self.names["stderr"]] = raw
        self.report["streams"]["stderr_bytes"] = len(raw)
        owned_controls.sync_report(self.fixture, self.report, self.names)
        with mock.patch.object(VM.uuid, "uuid4", side_effect=[SimpleNamespace(hex=TOKEN), SimpleNamespace(hex=safe)]) as factory:
            result = self.fixture.run_smoke(password="012345")
        self.assertTrue(result["passed"], result)
        self.assertEqual(factory.call_count, 2)
        self.assertEqual(result["terminal_ack"]["packet"]["token"], safe)

    def test_perpetual_token_password_overlap_refuses_without_connection(self):
        with mock.patch.object(VM.uuid, "uuid4", return_value=SimpleNamespace(hex=TOKEN)) as factory:
            result = self.fixture.run_smoke(password="0")
        self.assertFalse(result["passed"] or self.client.connected)
        self.assertEqual(result["state"], "TERMINAL_ACK_TOKEN_UNAVAILABLE")
        self.assertLessEqual(factory.call_count, 32)
        self.assertNotIn("JCE_TERMINAL_ACK_TOKEN", json.dumps(result))

    def test_optional_trace_reuses_collected_exact_bytes_and_canonical_binding(self):
        self.args.env = ["JCE_TRACE_STREAM=trace.json"]
        self.args.collect_relative = ["trace.json"]
        raw = json.dumps(trace_document()).encode()
        self.client.transport.generated_files["trace.json"] = raw
        stderr = encoded(packet(token=TOKEN, cleanup_scope="10", trace_status="complete",
                                trace_published=True, trace_write_ok=True))
        self.client.transport.generated_files[self.names["stderr"]] = stderr
        self.report["streams"]["stderr_bytes"] = len(stderr)
        owned_controls.sync_report(self.fixture, self.report, self.names)
        result = self.run_smoke()
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["terminal_ack"]["trace_binding"], "validated")
        self.assertEqual(result["terminal_ack"]["trace"]["sha256"], hashlib.sha256(raw).hexdigest())

    def test_requested_trace_requires_the_same_ack_to_report_complete_trace(self):
        self.args.env = ["JCE_TRACE_STREAM=trace.json"]
        self.args.collect_relative = ["trace.json"]
        self.client.transport.generated_files["trace.json"] = json.dumps(trace_document()).encode()
        result = self.run_smoke()
        self.assertFalse(result["passed"])
        self.assertEqual(result["state"], "TERMINAL_ACK_REFUSED")
        self.assertNotIn("terminal_ack", result)

    def test_password_environment_cli_does_not_print_or_persist_credentials(self):
        actual = VM.run_smoke
        def controlled(args, password):
            return actual(args, password, repo=self.fixture.repo, client_factory=lambda: self.client,
                          clock=self.fixture.clock, sleep=self.fixture.clock.sleep,
                          nonce_factory=lambda: controls.NONCE)
        argv = ["--host", self.args.host, "--user", self.args.user,
                "--host-key-sha256", self.args.host_key_sha256, "--artifact", str(self.args.artifact),
                "--expected-arch", self.args.expected_arch, "--kind", "editor", "--out", str(self.args.out),
                "--remote-dir", self.args.remote_dir, "--owned-launcher", str(self.args.owned_launcher),
                "--export-receipt", str(self.args.export_receipt), "--require-terminal-ack",
                "--password-env", "JCE_XP_FIXTURE_CREDENTIAL"]
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.dict(os.environ, {"JCE_XP_FIXTURE_CREDENTIAL": controls.PASSWORD}), \
             mock.patch.object(VM, "run_smoke", side_effect=controlled), \
             mock.patch.object(VM.uuid, "uuid4", return_value=SimpleNamespace(hex=TOKEN)), \
             redirect_stdout(stdout), redirect_stderr(stderr):
            code = VM.main(argv)
        self.assertEqual(code, 0)
        self.assertNotIn(controls.PASSWORD, stdout.getvalue() + stderr.getvalue())
        for path in self.fixture.output().rglob("*"):
            if path.is_file():
                self.assertNotIn(controls.PASSWORD.encode(), path.read_bytes())
                self.assertNotIn(controls.PASSWORD.encode("utf-16le"), path.read_bytes())

    def test_explicit_trace_without_bounded_collection_refuses_before_connection(self):
        self.args.env = ["JCE_TRACE_STREAM=trace.json"]
        result = self.run_smoke()
        self.assertFalse(result["passed"] or self.client.connected)
        self.assertEqual(result["state"], "TERMINAL_ACK_TRACE_REQUEST_INVALID")

    def test_unavailable_requested_trace_cannot_pass(self):
        self.args.env = ["JCE_TRACE_STREAM=trace.json"]
        self.args.collect_relative = ["trace.json"]
        result = self.run_smoke()
        self.assertFalse(result["passed"])
        self.assertEqual(result["state"], "COLLECTION_FAILED")
        self.assertNotIn("terminal_ack", result)


if __name__ == "__main__":
    unittest.main()
