"""Pure explicit XP VM input-file controls; no native or SSH execution."""
import ast
import hashlib
import io
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest import mock

from tools.build import xp_vm_inputs as inputs
from tools.tests import test_xp_vm as controls
from tools.tests import test_xp_vm_owned as owned_controls

VM = controls.VM


class XpVmInputTests(unittest.TestCase):
    def setUp(self):
        fixture = controls.XpVmTests(methodName="runTest")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.fixture, self.args, self.client = fixture, fixture.args, fixture.client
        self.source = fixture.repo / "authored data=source.scene"
        self.source.write_bytes(b'{"authored":true}\n')

    def request(self, name="scene.jce", path=None):
        self.args.input_relative = [name + "=" + str(path or self.source)]

    def run_smoke(self):
        return self.fixture.run_smoke()

    def test_cli_exposes_repeatable_explicit_input_files(self):
        action = VM.build_parser()._option_string_actions.get("--input-relative")
        self.assertIsNotNone(action)
        self.assertEqual(action.default, [])
        args = VM.build_parser().parse_args(["--host", "fixture", "--user", "fixture",
            "--host-key-sha256", controls.PIN, "--artifact", str(self.fixture.artifact),
            "--expected-arch", "x64", "--kind", "core-probe", "--out", "build/validation/fixture",
            "--input-relative", "one.scene=" + str(self.source),
            "--input-relative", "two.scene=" + str(self.source)])
        self.assertEqual(args.input_relative, ["one.scene=" + str(self.source), "two.scene=" + str(self.source)])

    def test_exact_file_upload_retains_hash_and_windows_path_after_first_equals(self):
        self.request()
        original = self.source.read_bytes()
        result = self.run_smoke()
        self.assertTrue(result["passed"], result["state"])
        entry = result["inputs"][0]
        self.assertEqual(entry["name"], "scene.jce")
        self.assertEqual(entry["local_path"], str(self.source))
        self.assertEqual(entry["size_bytes"], len(original))
        self.assertEqual(entry["sha256"], hashlib.sha256(original).hexdigest())
        self.assertTrue(entry["upload_verified"])
        self.assertEqual(entry["state"], "UPLOADED")
        self.assertEqual(self.client.sftp.files[result["delivery"]["sftp_dir"] + "/scene.jce"], original)
        self.assertEqual(self.source.read_bytes(), original)
        self.assertIn("scene.jce", {item["name"] for item in result["launch"]["inventory"]})
        self.assertFalse((self.fixture.output() / "scene.jce").exists())

    def test_default_old_namespace_preserves_no_input_receipt_or_upload(self):
        del self.args.input_relative
        result = self.run_smoke()
        self.assertTrue(result["passed"])
        self.assertNotIn("inputs", result)
        self.assertEqual(len(result["launch"]["inventory"]), 1)

    def test_four_files_fit_owned_editor_inventory_without_becoming_launch_targets(self):
        owned_controls.configure_owned(self.fixture)
        self.args.input_relative = ["data" + str(index) + ".exe=" + str(self.source) for index in range(4)]
        result = self.run_smoke()
        self.assertTrue(result["passed"], result["state"])
        self.assertEqual(len(result["inputs"]), 4)
        self.assertEqual(len(result["launch"]["inventory"]), 7)
        self.assertTrue(result["launch"]["inventory_verified"])
        self.assertNotIn("data0.exe", self.client.transport.commands[-1])
        self.assertIn('"--target" "' + result["owned_launch"]["report"]["target_path"] + '"', self.client.transport.commands[-1])

    def test_collected_output_casefold_collision_refuses_before_connection(self):
        self.request("scene.jce")
        self.args.collect_relative = ["SCENE.JCE"]
        result = self.run_smoke()
        self.assertEqual(result["state"], "INPUT_NAME_COLLISION")
        self.assertFalse(self.client.connected)

    def test_target_and_helper_executable_names_are_reserved(self):
        for index, helper in enumerate((False, True)):
            with self.subTest(helper=helper):
                if helper:
                    owned_controls.configure_owned(self.fixture)
                path = self.args.owned_launcher if helper else self.fixture.artifact
                name = ("launcher-" if helper else "artifact-") + hashlib.sha256(path.read_bytes()).hexdigest() + ".exe"
                self.request(name.upper())
                self.args.out = "build/validation/collision-" + str(index)
                result = self.run_smoke()
                self.assertEqual(result["state"], "INPUT_NAME_COLLISION")
                self.assertFalse(self.client.connected)

    def test_profile_and_owned_report_stream_names_are_reserved(self):
        names = ["editor-profile", *owned_controls.owned.output_names(controls.NONCE).values()]
        for index, name in enumerate(names):
            with self.subTest(name=name):
                self.request(name.upper())
                self.args.out = "build/validation/reserved-" + str(index)
                result = self.run_smoke()
                self.assertEqual(result["state"], "INPUT_NAME_COLLISION")
                self.assertFalse(self.client.connected)

    def test_invalid_flat_names_windows_reserved_names_and_duplicate_case_refuse(self):
        for index, name in enumerate(("../scene", "folder/scene", "folder\\scene", ".jce-xp-owner", "NUL", "COM1.scene", "scene.", "scene ")):
            with self.subTest(name=name):
                self.request(name)
                self.args.out = "build/validation/invalid-" + str(index)
                result = self.run_smoke()
                self.assertEqual(result["state"], "INVALID_INPUT_REQUEST")
                self.assertFalse(self.client.connected)
        self.args.out = "build/validation/duplicate"
        self.args.input_relative = ["scene=" + str(self.source), "SCENE=" + str(self.source)]
        self.assertEqual(self.run_smoke()["state"], "INVALID_INPUT_REQUEST")

    def test_file_count_zero_and_single_file_size_limits_refuse_before_connection(self):
        cases = [["f" + str(index) + "=" + str(self.source) for index in range(5)]]
        empty = self.fixture.repo / "empty"
        empty.write_bytes(b"")
        large = self.fixture.repo / "large"
        with large.open("wb") as stream:
            stream.truncate(inputs.MAX_FILE_BYTES + 1)
        cases += [["empty=" + str(empty)], ["large=" + str(large)]]
        for index, values in enumerate(cases):
            with self.subTest(index=index):
                self.args.out = "build/validation/size-" + str(index)
                self.args.input_relative = values
                result = self.run_smoke()
                self.assertFalse(result["passed"] or self.client.connected)
                self.assertIn(result["state"], ("INVALID_INPUT_REQUEST", "INPUT_SIZE_LIMIT"))

    def test_exact_per_file_and_aggregate_limits_use_real_files(self):
        paths = []
        for index in range(3):
            path = self.fixture.repo / ("bounded" + str(index))
            with path.open("wb") as stream:
                stream.truncate(inputs.MAX_FILE_BYTES)
            paths.append(path)
        values = ["data" + str(index) + "=" + str(path) for index, path in enumerate(paths)]
        prepared = inputs.prepare(values[:2], validate_names=VM.collection_names, no_aliases=VM._no_aliases)
        self.assertEqual(sum(item.info.st_size for item in prepared), inputs.MAX_TOTAL_BYTES)
        with self.assertRaisesRegex(inputs.InputError, "INPUT_SIZE_LIMIT"):
            inputs.prepare(values, validate_names=VM.collection_names, no_aliases=VM._no_aliases)

    def test_directory_and_local_path_traversal_are_refused(self):
        directory = self.fixture.repo / "directory"
        directory.mkdir()
        cases = ((directory, "INPUT_SOURCE_NOT_REGULAR"),
                 (directory / ".." / self.source.name, "INPUT_SOURCE_UNSAFE"))
        for index, (path, state) in enumerate(cases):
            with self.subTest(path=path):
                self.request(path=path)
                self.args.out = "build/validation/path-" + str(index)
                result = self.run_smoke()
                self.assertEqual(result["state"], state)
                self.assertFalse(self.client.connected)

    def test_existing_alias_guard_is_called_for_the_input_source(self):
        original = VM._no_aliases
        def aliases(path):
            if Path(path) == self.source:
                raise VM.SmokeError("ALIASED_LOCAL_PATH")
            return original(path)
        self.request()
        with mock.patch.object(VM, "_no_aliases", side_effect=aliases):
            result = self.run_smoke()
        self.assertEqual(result["state"], "INPUT_SOURCE_UNSAFE")
        self.assertFalse(self.client.connected)

    def test_source_mutation_after_snapshot_refuses_before_input_upload(self):
        self.request()
        connect = self.client.connect
        def mutate(**kwargs):
            connect(**kwargs)
            self.source.write_bytes(b"changed after snapshot")
        self.client.connect = mutate
        result = self.run_smoke()
        self.assertEqual(result["state"], "INPUT_CHANGED")
        self.assertFalse(result["passed"])
        self.assertFalse(any(path.endswith("/scene.jce") for path, _mode in self.client.sftp.opens))
        self.fixture.assert_not_launched()

    def test_mutation_during_upload_cannot_pass_the_original_hash(self):
        self.request()
        upload = VM._upload_checked
        def mutate(path, *args):
            if path == self.source:
                self.source.write_bytes(b"changed during transfer")
            return upload(path, *args)
        with mock.patch.object(VM, "_upload_checked", side_effect=mutate):
            result = self.run_smoke()
        self.assertFalse(result["passed"])
        self.assertEqual(result["state"], "UPLOAD_HASH_MISMATCH")
        self.assertFalse(result["inputs"][0]["upload_verified"])
        self.fixture.assert_not_launched()

    def test_growing_input_is_never_sent_beyond_its_snapshotted_byte_budget(self):
        self.request()
        original = self.source.read_bytes()
        upload = VM._upload_checked
        def grow(path, *args):
            if path == self.source:
                self.source.write_bytes(original + b"extra bytes" * 100)
            return upload(path, *args)
        with mock.patch.object(VM, "_upload_checked", side_effect=grow):
            result = self.run_smoke()
        self.assertFalse(result["passed"])
        remote = result["delivery"]["sftp_dir"] + "/scene.jce"
        self.assertLessEqual(len(self.client.sftp.files[remote]), len(original))
        self.fixture.assert_not_launched()

    def test_sftp_readback_corruption_refuses_without_launch(self):
        self.request()
        original = self.client.sftp.open
        def corrupt(path, mode):
            if path.endswith("/scene.jce") and mode == "rb":
                return io.BytesIO(b"X" * self.source.stat().st_size)
            return original(path, mode)
        with mock.patch.object(self.client.sftp, "open", side_effect=corrupt):
            result = self.run_smoke()
        self.assertEqual(result["state"], "UPLOAD_HASH_MISMATCH")
        self.assertFalse(result["passed"])
        self.fixture.assert_not_launched()

    def test_input_size_is_rechecked_in_exact_prelaunch_inventory(self):
        self.request()
        original = self.client.sftp.listdir_iter
        def changed(path, read_aheads=1):
            for item in original(path, read_aheads):
                if item.filename == "scene.jce":
                    item.st_size += 1
                yield item
        with mock.patch.object(self.client.sftp, "listdir_iter", side_effect=changed):
            result = self.run_smoke()
        self.assertEqual(result["state"], "UNEXPECTED_LAUNCH_INVENTORY")
        self.assertFalse(result["passed"])
        self.fixture.assert_not_launched()

    def test_public_forwarder_preserves_repeated_input_values_and_window_mode(self):
        tree = ast.parse((controls.ROOT / "tools/build/jce.py").read_text(encoding="utf-8"))
        function = next(item for item in tree.body if isinstance(item, ast.FunctionDef) and item.name == "cmd_xp_vm")
        calls = []
        namespace = dict(sys=SimpleNamespace(executable="python"), ROOT=controls.ROOT,
                         _run_xp_tool=lambda label, command: calls.append(command))
        exec(compile(ast.Module(body=[function], type_ignores=[]), "actual-xp-vm-forwarder", "exec"), namespace)
        values = ["one=" + str(self.source), "two=" + str(self.source)]
        self.args.input_relative, self.args.editor_window = values, "visible"
        namespace["cmd_xp_vm"](self.args)
        command = calls.pop()
        self.assertEqual([command[index + 1] for index, value in enumerate(command) if value == "--input-relative"], values)
        self.assertEqual(command[command.index("--editor-window") + 1], "visible")
        self.args.input_relative, self.args.editor_window = [], None
        namespace["cmd_xp_vm"](self.args)
        self.assertNotIn("--input-relative", calls[0])
        self.assertNotIn("--editor-window", calls[0])


if __name__ == "__main__":
    unittest.main()
