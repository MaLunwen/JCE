"""Pure fixed project-policy staging controls; no SSH, native app or GPU."""
import hashlib
import io
import json
import stat
from types import SimpleNamespace
import unittest

from tools.tests import test_xp_vm as controls
from tools.tests import test_xp_vm_owned as owned_controls

VM = controls.VM
POLICY = b'{"player":{"run_in_background":true}}\n'


class PolicySFTP(controls.FakeSFTP):
    case = ""

    def mkdir(self, path):
        if path.endswith("/.jce") and self.case == "collision":
            raise FileExistsError(path)
        super().mkdir(path)

    def normalize(self, path):
        if self.case == "directory-alias" and path.endswith("/.jce"):
            return path + "-other"
        if self.case == "file-alias" and path.endswith("/.jce/project-settings.json"):
            return path + "-other"
        return super().normalize(path)

    def lstat(self, path):
        attributes = super().lstat(path)
        if (self.case == "directory-link" and path.endswith("/.jce")
                or self.case == "file-link" and path.endswith("/.jce/project-settings.json")):
            attributes.st_mode = stat.S_IFLNK | 0o700
        return attributes

    def open(self, path, mode):
        if self.case == "readback" and mode == "rb" and path.endswith("/.jce/project-settings.json"):
            return io.BytesIO(b"unexpected settings")
        return super().open(path, mode)

    def listdir_iter(self, path, read_aheads=1):
        if self.case == "late-mutation" and path in self.created and not path.endswith(("/.jce", "/editor-profile")):
            target = path + "/.jce/project-settings.json"
            if target in self.files:
                self.files[target] = b'{"player":{"run_in_background":null}}\n'
        yield from super().listdir_iter(path, read_aheads)
        if self.case == "unexpected" and path.endswith("/.jce"):
            yield SimpleNamespace(filename="foreign.json", st_mode=stat.S_IFREG | 0o600, st_size=1)


class XpVmBackgroundTests(unittest.TestCase):
    def setUp(self):
        fixture = controls.XpVmTests(methodName="runTest")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.fixture, self.args, self.client = fixture, fixture.args, fixture.client
        self.client.sftp = PolicySFTP()
        self.client.transport.sftp = self.client.sftp
        self.args.kind = "editor"
        self.args.editor_run_in_background = True

    def run_smoke(self):
        return self.fixture.run_smoke()

    def test_parser_exposes_explicit_default_off_option(self):
        action = VM.build_parser()._option_string_actions.get("--editor-run-in-background")
        self.assertIsNotNone(action, "typed background policy option is required")
        self.assertIs(action.default, False)

    def test_exact_setting_is_uploaded_and_verified_in_its_owned_subtree(self):
        result = self.run_smoke()
        self.assertTrue(result["passed"], result["state"])
        self.assertIn("editor_background", result, "explicit request must have its own staging receipt")
        entry = result["editor_background"]
        remote = result["delivery"]["sftp_dir"]
        self.assertEqual(self.client.sftp.files[remote + "/.jce/project-settings.json"], POLICY)
        self.assertEqual(json.loads(POLICY), {"player": {"run_in_background": True}})
        self.assertTrue(entry["upload_verified"] and entry["verified"])
        self.assertEqual(entry["state"], "VERIFIED_BEFORE_LAUNCH")
        self.assertEqual(entry["sha256"], hashlib.sha256(POLICY).hexdigest())
        self.assertEqual(entry["size_bytes"], len(POLICY))
        self.assertEqual(entry["relative_path"], ".jce/project-settings.json")
        self.assertEqual(entry["effective_setting"], "UNOBSERVED")
        self.assertEqual(entry["scope"], "PROJECTLESS_CWD_OR_MATCHING_PROJECT_ROOT_ONLY")
        self.assertEqual([item["name"] for item in entry["inventory"]], ["project-settings.json"])
        self.assertIn({"name": ".jce", "kind": "directory", "size_bytes": 0}, result["launch"]["inventory"])
        self.assertNotIn("RUN_IN_BACKGROUND", self.client.transport.commands[-1])
        self.assertEqual(self.client.transport.child_launches, 1)
        self.assertNotIn(controls.PASSWORD, (self.fixture.output() / "receipt.json").read_text())

    def test_old_namespace_default_adds_no_setting_file_or_metadata(self):
        del self.args.editor_run_in_background
        result = self.run_smoke()
        self.assertTrue(result["passed"], result["state"])
        self.assertNotIn("editor_background", result)
        self.assertFalse(any(path.endswith("/.jce") for path in self.client.sftp.created))
        self.assertFalse((self.fixture.output() / "editor-project-settings.json").exists())
        self.assertEqual(len(result["launch"]["inventory"]), 2)

    def test_non_editor_or_non_boolean_request_refuses_before_connection(self):
        self.args.kind = "core-probe"
        result = self.run_smoke()
        self.assertEqual(result["state"], "EDITOR_BACKGROUND_EDITOR_REQUIRED")
        self.assertFalse(self.client.connected)

    def test_non_boolean_request_refuses_before_connection(self):
        self.args.editor_run_in_background = "true"
        result = self.run_smoke()
        self.assertEqual(result["state"], "INVALID_EDITOR_BACKGROUND_REQUEST")
        self.assertFalse(self.client.connected)

    def test_existing_directory_refuses_without_launching(self):
        self.client.sftp.case = "collision"
        result = self.run_smoke()
        self.assertEqual(result["state"], "EDITOR_SETTINGS_COLLISION_OR_DENIED")
        self.assertFalse(result["passed"])
        self.assertEqual(self.client.transport.child_launches, 0)

    def test_aliases_and_links_refuse_without_launching(self):
        self.client.sftp.case = "directory-alias"
        result = self.run_smoke()
        self.assertEqual(result["state"], "EDITOR_SETTINGS_PATH_ALIAS")
        self.assertEqual(self.client.transport.child_launches, 0)

    def test_file_alias_refuses_without_launching(self):
        self.client.sftp.case = "file-alias"
        result = self.run_smoke()
        self.assertEqual(result["state"], "EDITOR_SETTINGS_PATH_ALIAS")
        self.assertEqual(self.client.transport.child_launches, 0)

    def test_directory_link_refuses_without_launching(self):
        self.client.sftp.case = "directory-link"
        result = self.run_smoke()
        self.assertEqual(result["state"], "EDITOR_SETTINGS_PATH_ALIAS")
        self.assertEqual(self.client.transport.child_launches, 0)

    def test_file_link_refuses_without_launching(self):
        self.client.sftp.case = "file-link"
        result = self.run_smoke()
        self.assertEqual(result["state"], "EDITOR_SETTINGS_NOT_REGULAR_FILE")
        self.assertEqual(self.client.transport.child_launches, 0)

    def test_readback_mismatch_preserves_existing_upload_refusal(self):
        self.client.sftp.case = "readback"
        result = self.run_smoke()
        self.assertEqual(result["state"], "UPLOAD_HASH_MISMATCH")
        self.assertEqual(self.client.transport.child_launches, 0)

    def test_unexpected_subtree_entry_refuses(self):
        self.client.sftp.case = "unexpected"
        result = self.run_smoke()
        self.assertEqual(result["state"], "EDITOR_SETTINGS_INVENTORY_UNVERIFIED")
        self.assertEqual(self.client.transport.child_launches, 0)

    def test_mutation_after_upload_is_rechecked_before_launch(self):
        self.client.sftp.case = "late-mutation"
        result = self.run_smoke()
        self.assertFalse(result["passed"])
        self.assertIn(result["state"], ("EDITOR_SETTINGS_BYTES_UNVERIFIED", "EDITOR_SETTINGS_INVENTORY_UNVERIFIED"))
        self.assertTrue(result["editor_background"]["upload_verified"])
        self.assertFalse(result["editor_background"]["verified"])
        self.assertEqual(self.client.transport.child_launches, 0)

    def test_four_flat_inputs_and_owned_helper_still_fit_the_existing_eight_entry_cap(self):
        owned_controls.configure_owned(self.fixture)
        source = self.fixture.repo / "public.scene"
        source.write_bytes(b"authored input\n")
        self.args.input_relative = ["data%d.scene=%s" % (index, source) for index in range(4)]
        result = self.run_smoke()
        self.assertTrue(result["passed"], result["state"])
        self.assertEqual(len(result["inputs"]), 4)
        self.assertEqual(len(result["launch"]["inventory"]), 8)
        self.assertTrue(result["editor_background"]["verified"])
        self.assertTrue(result["launch"]["inventory_verified"])


if __name__ == "__main__":
    unittest.main()
