"""Exact guest-export reader/transport CPU controls; no SSH or native runs."""
import base64
import builtins
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import stat
import struct
import tempfile
import types
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("jce_guest_exports_controls", ROOT / "tools/build/xp_guest_exports.py")
EXPORTS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORTS)
ALLOWED = {"kernel32.dll", "ntdll.dll", "msvcrt.dll", "advapi32.dll"}
KEY = b"public deterministic guest export host-key fixture"
PIN = "SHA256:" + base64.b64encode(hashlib.sha256(KEY).digest()).decode().rstrip("=")
PASSWORD = "private-export-fixture-password-never-persist"


def dll_bytes(name="kernel32.dll", arch="x64", exports=None, base=1):
    exports = exports or [(["GetProcAddress"], None)]
    raw = bytearray(0x3200)
    raw[:2] = b"MZ"
    struct.pack_into("<I", raw, 0x3c, 0x80)
    raw[0x80:0x84] = b"PE\0\0"
    optional_size = 240 if arch == "x64" else 224
    struct.pack_into("<HHIIIHH", raw, 0x84, 0x8664 if arch == "x64" else 0x14c,
                     1, 0, 0, 0, optional_size, 0x2000)
    optional = 0x98
    struct.pack_into("<H", raw, optional, 0x20b if arch == "x64" else 0x10b)
    struct.pack_into("<HH", raw, optional + 40, 4, 0)  # DLL floor need not equal app floor.
    struct.pack_into("<II", raw, optional + 56, 0x5000, 0x200)
    directory = optional + (112 if arch == "x64" else 96)
    struct.pack_into("<I", raw, directory - 4, 16)
    struct.pack_into("<II", raw, directory, 0x1000, 0x1000)
    section = optional + optional_size
    raw[section:section + 8] = b".edata\0\0"
    struct.pack_into("<IIII", raw, section + 8, 0x3000, 0x1000, 0x3000, 0x200)

    def text(rva, value):
        value = value.encode("ascii") + b"\0"
        raw[rva - 0xe00:rva - 0xe00 + len(value)] = value

    text(0x1100, name)
    name_index = 0
    for slot, export in enumerate(exports):
        if export is None:
            continue
        names, forward = export
        address = 0x1800 + slot * 128 if forward else 0x2800 + slot * 4
        struct.pack_into("<I", raw, 0x400 + slot * 4, address)
        if forward:
            text(address, forward)
        for value in names:
            rva = 0x1500 + name_index * 64
            text(rva, value)
            struct.pack_into("<I", raw, 0x500 + name_index * 4, rva)
            struct.pack_into("<H", raw, 0x600 + name_index * 2, slot)
            name_index += 1
    struct.pack_into("<IIHHIIIIIII", raw, 0x200, 0, 0, 0, 0, 0x1100, base,
                     len(exports), name_index, 0x1200, 0x1300, 0x1400)
    return bytes(raw)


class ExportReaderTests(unittest.TestCase):
    def parse(self, raw, arch="x64", name="kernel32.dll"):
        return EXPORTS.parse_exports(raw, arch, name, ALLOWED)

    def test_both_architectures_and_dll_header_floor_are_independent(self):
        for arch in ("x86", "x64"):
            with self.subTest(arch=arch):
                entry = self.parse(dll_bytes(arch=arch), arch)
                self.assertEqual(entry["names"], ["GetProcAddress"])
                self.assertEqual(entry["ordinals"], [1])
                self.assertEqual(entry["sha256"], hashlib.sha256(dll_bytes(arch=arch)).hexdigest())
        with self.assertRaisesRegex(EXPORTS.CollectionError, "ARCH_MISMATCH"):
            self.parse(dll_bytes(arch="x86"))

    def test_named_aliases_and_real_ordinals_retain_forwarders_and_holes(self):
        raw = dll_bytes(exports=[(["Alias", "HeapAlloc"], "NTDLL.RtlAllocateHeap"), None, (["Direct"], None)], base=40)
        entry = self.parse(raw)
        self.assertEqual(entry["ordinals"], [40, 42])
        self.assertEqual(entry["pe"]["holes_excluded"], 1)
        self.assertEqual(set(entry["forwarders"]), {"Alias", "HeapAlloc", "40"})
        self.assertEqual(entry["forwarders"]["40"], {"dll": "ntdll.dll", "symbol": "RtlAllocateHeap"})
        hole = bytearray(raw)
        struct.pack_into("<I", hole, 0x400, 0)
        with self.assertRaisesRegex(EXPORTS.CollectionError, "POINTS_TO_HOLE"):
            self.parse(bytes(hole))

    def test_forwarded_ordinals_cycles_and_missing_destinations(self):
        source = self.parse(dll_bytes(exports=[(["Fwd"], "NTDLL.#7")]))
        destination = self.parse(dll_bytes("ntdll.dll", exports=[(["Target"], None)], base=7), name="ntdll.dll")
        EXPORTS.validate_forwarders({"kernel32.dll": source, "ntdll.dll": destination})
        destination["forwarders"] = {"7": {"dll": "kernel32.dll", "symbol": "Fwd"},
                                      "Target": {"dll": "kernel32.dll", "symbol": "Fwd"}}
        with self.assertRaisesRegex(EXPORTS.CollectionError, "CYCLIC"):
            EXPORTS.validate_forwarders({"kernel32.dll": source, "ntdll.dll": destination})
        with self.assertRaisesRegex(EXPORTS.CollectionError, "MISSING"):
            EXPORTS.validate_forwarders({"kernel32.dll": source})
        for value in ("otherdll.Target", "NTDLL.#0", "NTDLL.#65536", "NTDLL.\\escape", "missingseparator"):
            with self.subTest(value=value), self.assertRaises(EXPORTS.CollectionError):
                self.parse(dll_bytes(exports=[(["Fwd"], value)]))

    def test_malformed_tables_names_offsets_and_export_identity_are_refused(self):
        mutations = []
        raw = bytearray(dll_bytes())
        struct.pack_into("<I", raw, 0x200 + 28, 0x70000000)
        mutations.append(raw)
        raw = bytearray(dll_bytes())
        struct.pack_into("<H", raw, 0x600, 9)
        mutations.append(raw)
        raw = bytearray(dll_bytes(exports=[(["Same", "Same"], None)]))
        mutations.append(raw)
        raw = bytearray(dll_bytes())
        struct.pack_into("<I", raw, 0x98 + 56, 0x1000)
        mutations.append(raw)
        for raw in mutations:
            with self.assertRaises(EXPORTS.CollectionError):
                self.parse(bytes(raw))
        with self.assertRaisesRegex(EXPORTS.CollectionError, "EXPORT_NAME_MISMATCH"):
            self.parse(dll_bytes("ntdll.dll"))
        with self.assertRaises(EXPORTS.CollectionError):
            self.parse(dll_bytes()[:100])

    def test_drive_mappings_cover_verified_bitvise_and_preserve_drive(self):
        for path in ("/C/WINDOWS/System32", "/C:/WINDOWS/System32", "C:/WINDOWS/System32", "/cygdrive/c/WINDOWS/System32"):
            self.assertEqual(EXPORTS._windows_mapping(path), "C:/WINDOWS/System32")
        with self.assertRaisesRegex(EXPORTS.CollectionError, "UNVERIFIED"):
            EXPORTS._windows_mapping("/unknown/system32")


class FakeChannel:
    def __init__(self, client):
        self.client, self.raw, self.closed = client, b"", False
    def settimeout(self, value):
        pass
    def exec_command(self, value):
        self.client.commands.append(value)
        if value.endswith("/c ver"):
            result = "Microsoft Windows [Version " + ("5.2.3790" if self.client.arch == "x64" else "5.1.2600") + "]"
        elif "%PROCESSOR_ARCHITECTURE%" in value:
            result = "AMD64" if self.client.arch == "x64" else "X86"
        elif "%SystemRoot%" in value:
            result = "C:\\WINDOWS"
        else:
            key = value.rsplit(" ", 1)[-1]
            values = {"ProductName": "Microsoft Windows XP Professional", "CurrentVersion": "5.2" if self.client.arch == "x64" else "5.1",
                      "CurrentBuildNumber": "3790" if self.client.arch == "x64" else "2600", "CSDVersion": "Service Pack fixture"}
            values.update(self.client.registry_overrides)
            result = "    " + key + "    REG_SZ    " + values[key]
        self.raw = (self.client.metadata_override or result).encode() + b"\r\n"
    def recv_ready(self):
        return bool(self.raw)
    def recv(self, size):
        result, self.raw = self.raw[:size], self.raw[size:]
        return result
    def recv_stderr_ready(self):
        return False
    def recv_stderr(self, size):
        return b""
    def exit_status_ready(self):
        return True
    def recv_exit_status(self):
        return 0
    def close(self):
        self.closed = True


class FakeFile:
    def __init__(self, sftp, name):
        self.sftp, self.name, self.position, self.repeat, self.closed = sftp, name, 0, 0, False
    def stat(self):
        result = self.sftp.attributes(self.name)
        if self.position and self.sftp.change_metadata:
            result.st_mtime += 1
        return result
    def seek(self, offset):
        self.position, self.repeat = offset, self.repeat + 1
    def read(self, size):
        raw = self.sftp.files[self.name]
        if self.repeat and self.sftp.change_contents:
            raw = raw[:-1] + bytes([raw[-1] ^ 1])
        result = raw[self.position:self.position + size]
        self.position += len(result)
        if self.sftp.advance:
            self.sftp.advance()
        return result
    def close(self):
        self.closed = True
        if self.sftp.file_close_failure:
            raise OSError("file close failed: " + PASSWORD)


class FakeSftp:
    def __init__(self, arch="x64", directory="/C/WINDOWS/System32"):
        self.directory, self.closed, self.opens, self.aliases = directory, False, [], {}
        self.change_contents = self.change_metadata = self.file_close_failure = False
        self.advance = None
        self.files = {"kernel32.dll": dll_bytes(arch=arch, exports=[(["HeapAlloc", "Alias"], "NTDLL.RtlAllocateHeap"), (["GetProcAddress"], None)]),
                      "ntdll.dll": dll_bytes("ntdll.dll", arch=arch, exports=[(["RtlAllocateHeap"], None)]),
                      "msvcrt.dll": dll_bytes("msvcrt.dll", arch=arch, exports=[(["printf"], None)])}
    def attributes(self, name):
        return types.SimpleNamespace(st_mode=stat.S_IFREG | 0o444, st_size=len(self.files[name]), st_mtime=1, st_uid=0, st_gid=0)
    def lstat(self, value):
        if value == self.directory:
            return types.SimpleNamespace(st_mode=stat.S_IFDIR | 0o555, st_size=0, st_mtime=1)
        return self.attributes(value.rsplit("/", 1)[-1])
    def normalize(self, value):
        return self.aliases.get(value, value)
    def open(self, path, mode):
        if mode != "rb":
            raise AssertionError("guest writes are forbidden")
        self.opens.append((path, mode))
        return FakeFile(self, path.rsplit("/", 1)[-1])
    def get_channel(self):
        return types.SimpleNamespace(settimeout=lambda value: None)
    def close(self):
        self.closed = True


class FakeClient:
    def __init__(self, arch="x64"):
        self.arch, self.sftp, self.commands, self.closed = arch, FakeSftp(arch), [], False
        self.metadata_override = None
        self.registry_overrides = {}
        self.connect_failure = self.close_failure = False
        self.key = KEY
    def set_missing_host_key_policy(self, pin):
        self.pin = pin
    def connect(self, **values):
        self.connection = values
        if self.connect_failure:
            raise OSError("authentication failed: " + PASSWORD)
    def get_transport(self):
        return self
    def get_remote_server_key(self):
        return types.SimpleNamespace(asbytes=lambda: self.key)
    def open_session(self, timeout):
        return FakeChannel(self)
    def open_sftp(self):
        return self.sftp
    def close(self):
        self.closed = True
        if self.close_failure:
            raise OSError("close failed: " + PASSWORD)


class CollectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name) / "repo"
        self.client = FakeClient()
        self.count = 0
    def args(self, *extra):
        self.count += 1
        return EXPORTS.build_parser().parse_args(["--host", "192.0.2.17", "--user", "fixture",
            "--host-key-sha256", PIN, "--expected-arch", self.client.arch, "--remote-system32", self.client.sftp.directory,
            "--password-env", "JCE_EXPORT_TEST_PASSWORD", "--dll", "kernel32.dll", "--out",
            str(self.repo / "build/validation" / ("exports-" + str(self.count))), *extra])
    def collect(self, *extra, args=None, **kwargs):
        args = args or self.args(*extra)
        self.last_args = args
        result = EXPORTS.run_collection(args, PASSWORD, repo=self.repo, client_factory=lambda: self.client, **kwargs)
        self.assertEqual(json.loads((args.out / "receipt.json").read_text()), result)
        return result

    def test_collects_exact_bytes_and_forwarder_closure_for_both_guests(self):
        for arch in ("x86", "x64"):
            with self.subTest(arch=arch):
                self.client = FakeClient(arch)
                receipt = self.collect()
                self.assertTrue(receipt["collected"])
                self.assertFalse(receipt["editor_qualified"])
                inventory = json.loads((self.last_args.out / "inventory.json").read_text())
                self.assertEqual(set(inventory["dlls"]), {"kernel32.dll", "ntdll.dll"})
                self.assertEqual(inventory["guest"]["arch"], arch)
                self.assertTrue(receipt["mapping"]["kernel32_arch_verified"])
                for name, entry in inventory["dlls"].items():
                    self.assertEqual(hashlib.sha256(self.client.sftp.files[name]).hexdigest(), entry["sha256"])
                self.assertFalse(receipt["raw_capture_retained"])
                self.assertEqual(receipt["raw_capture_mode"], "inventory-only")
                self.assertFalse((self.last_args.out / "dlls").exists())
                self.assertEqual(receipt["transfer_bytes"], 2 * sum(len(self.client.sftp.files[name]) for name in inventory["dlls"]))
                self.assertFalse(self.client.connection["allow_agent"])
                self.assertFalse(self.client.connection["look_for_keys"])
                self.assertTrue(self.client.closed and self.client.sftp.closed)
                self.assertTrue(all(mode == "rb" for _, mode in self.client.sftp.opens))

    def test_wrong_pinned_key_registry_and_cross_arch_mapping_cannot_publish(self):
        self.client.key = b"different host"
        result = self.collect()
        self.assertEqual(result["state"], "HOST_KEY_MISMATCH")
        self.assertEqual(self.client.commands, [])
        self.client = FakeClient()
        self.client.metadata_override = "Microsoft Windows [Version 6.1.7601]"
        self.assertEqual(self.collect()["state"], "GUEST_OS_VERSION_MISMATCH")
        self.client = FakeClient()
        self.client.sftp.files["kernel32.dll"] = dll_bytes(arch="x86")
        self.assertEqual(self.collect()["state"], "SYSTEM_DLL_ARCH_MISMATCH")
        self.assertFalse((self.last_args.out / "inventory.json").exists())
        self.client = FakeClient()
        self.client.registry_overrides = {"ProductName": "Microsoft Windows Server fixture"}
        self.assertEqual(self.collect()["state"], "GUEST_REGISTRY_IDENTITY_MISMATCH")

    def test_alias_escape_and_unknown_dll_are_rejected_without_crawling(self):
        self.client.sftp.aliases[self.client.sftp.directory + "/kernel32.dll"] = "/C/other/kernel32.dll"
        self.assertEqual(self.collect()["state"], "SYSTEM_DLL_PATH_ALIAS")
        self.client = FakeClient()
        self.assertEqual(self.collect("--dll", "../kernel32.dll")["state"], "INVALID_SYSTEM_DLL_BASENAME")
        self.assertFalse(hasattr(self.client, "connection"))
        self.assertEqual(self.collect("--dll", "foreign.dll")["state"], "DLL_OUTSIDE_XP_SYSTEM_POLICY")
        self.client = FakeClient()
        self.client.sftp.directory = "/D/WINDOWS/System32"
        self.assertEqual(self.collect()["state"], "SYSTEM32_MAPPING_MISMATCH")

    def test_two_hashes_metadata_and_count_byte_budgets_are_real_gates(self):
        self.client.sftp.change_contents = True
        self.assertEqual(self.collect()["state"], "SYSTEM_DLL_CONTENT_CHANGED")
        self.client = FakeClient()
        self.client.sftp.change_metadata = True
        self.assertEqual(self.collect()["state"], "SYSTEM_DLL_HANDLE_CHANGED")
        self.client = FakeClient()
        self.assertEqual(self.collect("--max-total-bytes", "1")["state"], "TOTAL_TRANSFER_BYTE_LIMIT")
        self.client = FakeClient()
        self.assertEqual(self.collect("--max-dll-bytes", "1")["state"], "INVALID_SYSTEM_DLL_FILE_OR_SIZE")
        self.client = FakeClient()
        self.assertEqual(self.collect("--max-dlls", "1")["state"], "SYSTEM_DLL_COUNT_LIMIT")

    def test_transport_errors_metadata_and_binary_captures_never_persist_password(self):
        self.client.connect_failure = True
        self.assertFalse(self.collect()["collected"])
        self.client = FakeClient()
        self.client.metadata_override = PASSWORD
        self.assertEqual(self.collect()["state"], "CREDENTIAL_IN_GUEST_METADATA")
        self.client = FakeClient()
        self.client.sftp.files["kernel32.dll"] += PASSWORD.encode()
        self.assertEqual(self.collect("--retain-dlls")["state"], "CREDENTIAL_BYTES_IN_BINARY_CAPTURE")
        self.client = FakeClient()
        self.client.close_failure = True
        self.assertEqual(self.collect()["state"], "COLLECTION_CLOSE_FAILURE")
        for path in self.repo.rglob("*"):
            if path.is_file():
                self.assertNotIn(PASSWORD.encode(), path.read_bytes())
        self.assertFalse((self.last_args.out / "inventory.json").exists())

    def test_inventory_only_keeps_exact_identity_without_retaining_coincidental_raw_bytes(self):
        self.client.sftp.files["kernel32.dll"] += PASSWORD.encode()
        receipt = self.collect()
        self.assertTrue(receipt["collected"])
        self.assertFalse(receipt["raw_capture_retained"])
        inventory = json.loads((self.last_args.out / "inventory.json").read_text())
        self.assertEqual(inventory["dlls"]["kernel32.dll"]["sha256"], hashlib.sha256(self.client.sftp.files["kernel32.dll"]).hexdigest())
        self.assertFalse((self.last_args.out / "dlls").exists())
        for path in self.last_args.out.rglob("*"):
            if path.is_file():
                self.assertNotIn(PASSWORD.encode(), path.read_bytes())
        entries = {"kernel32.dll": {"sha256": "1234" * 16, "names": ["PublicExport"],
            "ordinals": [1234], "forwarders": {}}}
        self.assertFalse(EXPORTS._inventory_text_has_credential(entries, {}, "1234"))
        entries["kernel32.dll"]["names"] = ["SourceText1234"]
        self.assertTrue(EXPORTS._inventory_text_has_credential(entries, {}, "1234"))

    def test_opt_in_raw_retention_preserves_exact_verified_bytes(self):
        receipt = self.collect("--retain-dlls")
        self.assertTrue(receipt["collected"])
        self.assertTrue(receipt["raw_capture_retained"])
        for row in receipt["files"]:
            self.assertTrue(row["raw_capture_retained"])
            self.assertEqual((self.last_args.out / row["local_relative"]).read_bytes(), self.client.sftp.files[row["dll"]])

    def test_primary_failure_survives_close_failure_and_publication_failure_has_receipt(self):
        self.client.key = b"wrong pinned key"
        self.client.close_failure = True
        receipt = self.collect()
        self.assertEqual(receipt["state"], "HOST_KEY_MISMATCH")
        self.assertEqual(receipt["primary_state"], "HOST_KEY_MISMATCH")
        self.assertEqual(receipt["close_failures"], ["client"])
        self.client = FakeClient()
        original_open = Path.open
        def deny_inventory(path, *values, **kwargs):
            if path.name == "inventory.json.tmp":
                raise OSError("publication denied: " + PASSWORD)
            return original_open(path, *values, **kwargs)
        with mock.patch.object(Path, "open", deny_inventory):
            receipt = self.collect()
        self.assertEqual(receipt["state"], "INVENTORY_PUBLICATION_FAILURE")
        self.assertFalse(receipt["collected"])
        self.assertFalse((self.last_args.out / "inventory.json").exists())
        self.assertNotIn(PASSWORD, (self.last_args.out / "receipt.json").read_text())

    def test_file_close_failure_keeps_primary_mutation_and_cleanup_unavailable(self):
        self.client.sftp.change_contents = True
        self.client.sftp.file_close_failure = True
        receipt = self.collect()
        self.assertEqual(receipt["state"], "SYSTEM_DLL_CONTENT_CHANGED")
        self.assertEqual(receipt["primary_state"], "SYSTEM_DLL_CONTENT_CHANGED")
        self.assertEqual(receipt["cleanup_state"], "CLOSE_FAILURE")
        self.assertIn("dll:kernel32.dll", receipt["close_failures"])
        self.assertFalse(receipt["collected"])

    def test_total_deadline_closes_owned_transport_and_leaves_no_inventory(self):
        now = [0.0]
        self.client.sftp.advance = lambda: now.__setitem__(0, now[0] + 2)
        receipt = self.collect("--timeout", "1", clock=lambda: now[0], sleep=lambda delay: None)
        self.assertEqual(receipt["state"], "COLLECTION_TIMEOUT")
        self.assertTrue(self.client.closed)
        self.assertFalse((self.last_args.out / "inventory.json").exists())

    def test_artifact_import_reader_and_existing_auditor_consume_real_schema(self):
        spec = importlib.util.spec_from_file_location("xp_exports_import_fixture", ROOT / "tools/tests/test_xp_editor.py")
        fixture = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fixture)
        self.client.sftp.files["advapi32.dll"] = dll_bytes("advapi32.dll", exports=[(["CryptGenRandom"], None)])
        artifact = self.repo / "artifact.exe"
        artifact.parent.mkdir(parents=True)
        artifact.write_bytes(fixture.editor_pe())
        receipt = self.collect("--artifact", str(artifact))
        self.assertTrue(receipt["collected"])
        xp_editor = EXPORTS._module("xp_editor")
        report = xp_editor.audit_pe(artifact, "x64", self.last_args.out / "inventory.json")
        self.assertEqual(report["state"], "STATIC_IMPORT_AUDIT_PASSED")
        self.assertFalse(receipt["editor_qualified"])

    def test_import_and_help_are_lazy_and_cli_reads_password_only_at_runtime(self):
        original = builtins.__import__
        def guard(name, *values, **kwargs):
            if name == "paramiko" or name.startswith("paramiko."):
                raise AssertionError("Paramiko imported before connection")
            return original(name, *values, **kwargs)
        with mock.patch("builtins.__import__", side_effect=guard):
            spec = importlib.util.spec_from_file_location("xp_exports_lazy_control", ROOT / "tools/build/xp_guest_exports.py")
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            self.assertIn("--password-env", module.build_parser().format_help())
        args = self.args()
        argv = ["--host", args.host, "--user", args.user, "--host-key-sha256", PIN, "--expected-arch", "x64",
                "--remote-system32", args.remote_system32, "--password-env", "JCE_EXPORT_TEST_PASSWORD", "--dll", "kernel32.dll",
                "--out", str(args.out)]
        with mock.patch.dict("os.environ", {"JCE_EXPORT_TEST_PASSWORD": PASSWORD}), mock.patch.object(
                EXPORTS, "run_collection", return_value={"state": "GUEST_EXPORTS_COLLECTED", "collected": True}) as run, mock.patch("sys.stdout", io.StringIO()):
            self.assertEqual(EXPORTS.main(argv), 0)
        self.assertEqual(run.call_args.args[1], PASSWORD)
        self.assertNotIn(PASSWORD, repr(run.call_args.args[0]))


if __name__ == "__main__":
    unittest.main()
