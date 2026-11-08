"""CPU transport controls for the actual XP VM smoke owner; no SSH/native runs."""
import ast
import base64
import hashlib
import importlib.util
import io
import inspect
import json
from pathlib import Path
import stat
import struct
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("jce_xp_vm_test_owner", ROOT / "tools/build/xp_vm.py")
VM = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VM)
KEY = b"deterministic public host key fixture"
PIN = "SHA256:" + base64.b64encode(hashlib.sha256(KEY).digest()).decode().rstrip("=")
NONCE = "b" * 32
PASSWORD = "private-fixture-password-never-persist"
NATIVE_MARKER = "JCE_NATIVE_FIXTURE_OK"


def sample_pe(arch="x64", os_version=None, subsystem=3, graphics=False, gl_symbols=None):
    """Syntactically valid import fixture, never an executable payload."""
    raw = bytearray(4096)
    raw[:2] = b"MZ"
    struct.pack_into("<I", raw, 0x3c, 0x80)
    raw[0x80:0x84] = b"PE\x00\x00"
    is64 = arch == "x64"
    size = 240 if is64 else 224
    struct.pack_into("<HHIIIHH", raw, 0x84, 0x8664 if is64 else 0x14c, 1, 0, 0, 0, size, 0)
    opt = 0x98
    struct.pack_into("<H", raw, opt, 0x20b if is64 else 0x10b)
    version = os_version or ((5, 2) if is64 else (5, 1))
    struct.pack_into("<HH", raw, opt + 40, *version)
    struct.pack_into("<HH", raw, opt + 48, *version)
    struct.pack_into("<H", raw, opt + 68, subsystem)
    struct.pack_into("<I", raw, opt + (108 if is64 else 92), 16)
    data = opt + (112 if is64 else 96)
    struct.pack_into("<II", raw, data + 8, 0x1000, 100)
    section = opt + size
    raw[section:section + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", raw, section + 8, 0x800, 0x1000, 0x800, 0x200)
    for index, (dll, symbol) in enumerate((("kernel32.dll", "GetProcAddress"),
                                         ("msvcrt.dll", "printf"),
                                         ("advapi32.dll", "CryptGenRandom"))):
        name, thunk, hint = 0x1100 + index * 0x30, 0x1200 + index * 0x30, 0x1350 + index * 0x40
        struct.pack_into("<IIIII", raw, 0x200 + index * 20, thunk, 0, 0, name, thunk)
        encoded = dll.encode() + b"\0"
        raw[name - 0xe00:name - 0xe00 + len(encoded)] = encoded
        encoded = b"\0\0" + symbol.encode() + b"\0"
        raw[hint - 0xe00:hint - 0xe00 + len(encoded)] = encoded
        struct.pack_into("<Q" if is64 else "<I", raw, thunk - 0xe00, hint)
    if graphics:
        symbols = ("glGetError", "glGetIntegerv", "glGetString", "wglCreateContext", "wglDeleteContext",
                   "wglGetCurrentContext", "wglGetCurrentDC", "wglGetProcAddress", "wglMakeCurrent") if gl_symbols is None else gl_symbols
        name, thunk = 0x1190, 0x1290
        struct.pack_into("<IIIII", raw, 0x200 + 3 * 20, thunk, 0, 0, name, thunk)
        raw[name - 0xe00:name - 0xe00 + 13] = b"opengl32.dll\0"
        for index, symbol in enumerate(symbols):
            hint = 0x1500 + index * 0x40
            encoded = b"\0\0" + symbol.encode() + b"\0"
            raw[hint - 0xe00:hint - 0xe00 + len(encoded)] = encoded
            struct.pack_into("<Q" if is64 else "<I", raw, thunk - 0xe00 + index * (8 if is64 else 4), hint)
    return bytes(raw)


def export_receipt(directory, artifact, arch="x64"):
    spec = importlib.util.spec_from_file_location("xp_vm_native_evidence_fixture", ROOT / "tools/tests/test_xp_native_probe.py")
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value.write_export_receipt(directory, artifact, arch, host="fixture.invalid", user="Fixture User", pin=PIN)


class FakeClock:
    def __init__(self):
        self.now = 0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class FakeChannel:
    def __init__(self, transport):
        self.transport = transport
        self.stdout = bytearray()
        self.stderr = bytearray()
        self.code = 0
        self.closed = False
        self.hang = False
        self.release = threading.Event()

    def settimeout(self, seconds):
        self.transport.timeouts.append(seconds)

    def exec_command(self, text):
        self.transport.commands.append(text)
        if text.endswith("/c ver"):
            self.stdout.extend(self.transport.version)
        elif "%PROCESSOR_ARCHITECTURE%" in text:
            self.stdout.extend(self.transport.arch)
        elif "echo %USERPROFILE%" in text:
            self.stdout.extend(b"C:\\Documents and Settings\\Test User\r\n")
        elif "reg query" in text:
            if "User Shell Folders" in text:
                stdout, stderr, self.code = self.transport.desktop_user
            else:
                stdout, stderr, self.code = self.transport.desktop_resolved
            self.stdout.extend(stdout)
            self.stderr.extend(stderr)
        elif "type " in text:
            self.stdout.extend(self.transport.mapping)
        else:
            # Port the observed XP/Bitvise boundary: child ERRORLEVEL and SSH
            # status differ unless the explicit delayed exit tail is wired.
            explicit_exit = "/v:on " in text and text.endswith(' & exit /b !errorlevel!"')
            setup = text.split('start ""', 1)[0]
            setup_guarded = " && " in setup and " & " not in setup
            if self.transport.setup_exit_code and setup_guarded:
                self.code = self.transport.setup_exit_code if explicit_exit else 0
                self.stderr.extend(b"fixture setup failed before child launch\r\n")
                return
            self.transport.child_launches += 1
            self.stdout.extend(self.transport.stdout)
            self.stderr.extend(self.transport.stderr)
            self.code = self.transport.exit_code if explicit_exit else 0
            self.hang = self.transport.hang
            owned = self.transport.sftp.created[0]
            for name, value in self.transport.generated_files.items():
                self.transport.sftp.files[owned + "/" + name] = value
            if self.transport.hang_exec_ack:
                self.release.wait()

    def recv_ready(self):
        return bool(self.stdout)

    def recv_stderr_ready(self):
        return bool(self.stderr)

    @staticmethod
    def _read(buffer, size):
        data = bytes(buffer[:size])
        del buffer[:size]
        return data

    def recv(self, size):
        return self._read(self.stdout, size)

    def recv_stderr(self, size):
        return self._read(self.stderr, size)

    def exit_status_ready(self):
        return not self.hang

    def recv_exit_status(self):
        return self.code

    def close(self):
        self.closed = True
        self.release.set()


class FakeTransport:
    def __init__(self):
        self.key = KEY
        self.version = b"Microsoft Windows [Version 5.2.3790]\r\n"
        self.arch = b"AMD64\r\n"
        self.desktop_resolved = (b"    Desktop    REG_SZ    C:\\Documents and Settings\\Test User\\Desktop\r\n", b"", 0)
        self.desktop_user = (b"    Desktop    REG_EXPAND_SZ    %USERPROFILE%\\Desktop\r\n", b"", 0)
        self.mapping = NONCE.encode()
        self.stdout = (VM.PROBE_MARKER + "\r\n").encode()
        self.stderr = b""
        self.exit_code = 0
        self.setup_exit_code = 0
        self.child_launches = 0
        self.hang = False
        self.hang_exec_ack = False
        self.generated_files = {}
        self.commands = []
        self.timeouts = []
        self.channels = []

    def get_remote_server_key(self):
        return SimpleNamespace(asbytes=lambda: self.key)

    def open_session(self, timeout):
        self.timeouts.append(timeout)
        channel = FakeChannel(self)
        self.channels.append(channel)
        return channel


class FakeFile(io.BytesIO):
    def __init__(self, sftp, path, mode):
        self.sftp, self.path, self.mode = sftp, path, mode
        value = sftp.files.get(path, b"")
        if mode == "rb" and sftp.corrupt and path.endswith(".exe"):
            value = b"X" + value[1:]
        super().__init__(value)

    def close(self):
        if not self.closed and self.mode == "wx":
            self.sftp.files[self.path] = self.getvalue()
        super().close()


class FakeSFTP:
    def __init__(self):
        self.files = {}
        self.created = []
        self.opens = []
        self.closed = False
        self.corrupt = False
        self.collision = False
        self.file_collision = False
        self.profile_collision = False
        self.profile_nonempty = False
        self.extra_launch_file = False
        self.removed = []
        self.timeouts = []
        self.lstat_calls = {}
        self.aliases = {}
        self.symlink_names = set()
        self.changed_names = set()
        self.advertised_sizes = {}

    def get_channel(self):
        return SimpleNamespace(settimeout=self.timeouts.append)

    def stat(self, path):
        if path != "/virtual-home/Desktop":
            raise FileNotFoundError(path)
        return SimpleNamespace(st_mode=stat.S_IFDIR | 0o700)

    def normalize(self, path):
        return "/virtual-home" if path == "." else self.aliases.get(path, path)

    def lstat(self, path):
        self.lstat_calls[path] = self.lstat_calls.get(path, 0) + 1
        name = path.rsplit("/", 1)[-1]
        if path in self.created:
            return SimpleNamespace(st_mode=stat.S_IFDIR | 0o700, st_size=0, st_mtime=1)
        if path not in self.files:
            raise FileNotFoundError(path)
        mode = stat.S_IFLNK if name in self.symlink_names else stat.S_IFREG
        modified = 2 if name in self.changed_names and self.lstat_calls[path] >= 2 else 1
        return SimpleNamespace(st_mode=mode | 0o600,
                               st_size=self.advertised_sizes.get(name, len(self.files[path])), st_mtime=modified)

    def mkdir(self, path):
        if self.collision or self.profile_collision and path.endswith("/editor-profile"):
            raise FileExistsError(path)
        self.created.append(path)

    def remove(self, path):
        del self.files[path]
        self.removed.append(path)

    def listdir_iter(self, path, read_aheads=1):
        if path.endswith("/editor-profile") and self.profile_nonempty:
            yield SimpleNamespace(filename="existing-prefs.json", st_mode=stat.S_IFREG | 0o600, st_size=1)
            return
        for name, value in self.files.items():
            if name.startswith(path + "/") and "/" not in name[len(path) + 1:]:
                yield SimpleNamespace(filename=name.rsplit("/", 1)[-1], st_mode=stat.S_IFREG | 0o600,
                                      st_size=len(value))
        for name in self.created:
            if name.startswith(path + "/") and "/" not in name[len(path) + 1:]:
                yield SimpleNamespace(filename=name.rsplit("/", 1)[-1], st_mode=stat.S_IFDIR | 0o700, st_size=0)
        if self.extra_launch_file and not path.endswith("/editor-profile"):
            yield SimpleNamespace(filename="unexpected-runtime.dll", st_mode=stat.S_IFREG | 0o600, st_size=100)

    def open(self, path, mode):
        self.opens.append((path, mode))
        if mode == "wx" and (path in self.files or self.file_collision and path.endswith(".exe")):
            raise FileExistsError(path)
        if mode == "rb" and path not in self.files:
            raise FileNotFoundError(path)
        return FakeFile(self, path, mode)

    def close(self):
        self.closed = True


class FakeClient:
    def __init__(self):
        self.transport = FakeTransport()
        self.sftp = FakeSFTP()
        self.transport.sftp = self.sftp
        self.closed = False
        self.connected = False
        self.connect_error = None
        self.sftp_opened = False
        self.hang_sftp_ack = False
        self.hang_connect = False
        self.release = threading.Event()

    def set_missing_host_key_policy(self, policy):
        self.policy = policy

    def connect(self, **kwargs):
        self.connect_options = kwargs
        self.policy.missing_host_key(self, kwargs["hostname"], self.transport.get_remote_server_key())
        if self.connect_error:
            raise self.connect_error
        if self.hang_connect:
            self.release.wait()
        self.connected = True

    def get_transport(self):
        return self.transport

    def open_sftp(self):
        self.sftp_opened = True
        if self.hang_sftp_ack:
            self.release.wait()
        return self.sftp

    def close(self):
        self.closed = True
        self.release.set()


class XpVmTests(unittest.TestCase):
    def setUp(self):
        # Explicitly keep all generated fixture/output bytes beneath repo build/.
        fixture_root = ROOT / "build" / "validation"
        fixture_root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="xp-vm-controls-", dir=fixture_root)
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)
        self.artifact = self.repo / "probe.exe"
        self.artifact.write_bytes(sample_pe())
        self.args = VM.build_parser().parse_args([
            "--host", "fixture.invalid", "--user", "Fixture User", "--host-key-sha256", PIN,
            "--artifact", str(self.artifact), "--expected-arch", "x64", "--kind", "core-probe",
            "--out", "build/validation/run", "--remote-dir", "/virtual-home/Desktop"])
        self.client = FakeClient()
        self.clock = FakeClock()

    def run_smoke(self, password=PASSWORD):
        return VM.run_smoke(self.args, password, repo=self.repo, client_factory=lambda: self.client,
                            clock=self.clock, sleep=self.clock.sleep, nonce_factory=lambda: NONCE)

    def output(self):
        return self.repo / self.args.out

    def configure_native(self, graphics=False):
        self.args.kind = "native-probe"
        self.args.expect_marker = NATIVE_MARKER
        self.artifact.write_bytes(sample_pe(graphics=graphics))
        self.args.export_receipt = export_receipt(self.repo / "exports", self.artifact)
        self.client.transport.stdout = NATIVE_MARKER.encode() + b"\r\n"

    def assert_not_launched(self):
        self.assertFalse(any("artifact-" in text for text in self.client.transport.commands))

    def assert_explicit_wait(self, result):
        launch = self.client.transport.commands[-1]
        self.assertTrue(launch.startswith('cmd /d /q /v:on /s /c "'))
        self.assertIn('&& start "" /b /wait "', launch)
        self.assertEqual(result["launch"]["wait_policy"], "CMD_START_B_WAIT_THEN_EXIT_B_DELAYED_ERRORLEVEL")
        self.assertIn("NO_INDEPENDENT_CHILD_PID_OBSERVATION", result["launch"]["wait_scope"])
        self.assertTrue(result["launch"]["wait_contract_completed"])
        executable = result["delivery"]["windows_dir"] + "\\artifact-" + result["artifact"]["sha256"] + ".exe"
        self.assertTrue(launch.endswith('start "" /b /wait "' + executable + '" & exit /b !errorlevel!"'))

    def test_probe_success_binds_key_guest_desktop_and_readback_hash(self):
        result = self.run_smoke()
        self.assertTrue(result["passed"])
        self.assertEqual(result["state"], "SMOKE_PASSED")
        self.assertTrue(result["delivery"]["desktop_mapping_verified"])
        self.assertTrue(result["delivery"]["upload_verified"])
        self.assertEqual(result["artifact"]["sha256"], hashlib.sha256(self.artifact.read_bytes()).hexdigest())
        self.assertFalse(result["editor_graphics_verified"])
        self.assert_explicit_wait(result)
        self.assertEqual(result["artifact"]["pe"]["subsystem"], 3)
        self.assertEqual(result["artifact"]["pe"]["subsystem_name"], "WINDOWS_CUI")
        self.assertTrue(self.client.closed and self.client.sftp.closed)
        self.assertTrue(all(channel.closed for channel in self.client.transport.channels))
        self.assertFalse(self.client.connect_options["allow_agent"])
        self.assertFalse(self.client.connect_options["look_for_keys"])
        self.assertNotIn("set ", self.client.transport.commands[-1])
        self.assertIsNone(result["launch"]["profile_dir"])
        self.assertFalse(result["launch"]["profile_dir_fresh"])
        self.assertTrue(result["launch"]["inventory_verified"])
        self.assertEqual(len(result["launch"]["inventory"]), 1)
        receipt = json.loads((self.output() / "receipt.json").read_text())
        self.assertEqual(result, receipt)
        self.assertEqual((self.output() / "artifact.stdout.bin").read_bytes(), self.client.transport.stdout)

    def test_digest_collision_uses_verified_plain_executable_name(self):
        digest = hashlib.sha256(self.artifact.read_bytes()).hexdigest()
        fixture_secret = digest[11:19]
        result = self.run_smoke(password=fixture_secret)
        self.assertEqual(result["state"], "SMOKE_PASSED")
        self.assertEqual(result["artifact"]["sha256"], digest)
        self.assertTrue(result["delivery"]["upload_verified"])
        self.assertTrue(result["delivery"]["sftp_artifact"].endswith("/artifact.exe"))
        self.assertIn("\\artifact.exe", self.client.transport.commands[-1])
        self.assertNotIn(fixture_secret, self.client.transport.commands[-1])

    def test_x86_guest_positive_uses_nt51_and_x86_pe(self):
        self.args.expected_arch = "x86"
        self.artifact.write_bytes(sample_pe("x86"))
        self.client.transport.version = b"Microsoft Windows XP [Version 5.1.2600]\r\n"
        self.client.transport.arch = b"x86\r\n"
        self.assertTrue(self.run_smoke()["passed"])

    def test_wrong_host_key_fails_before_any_guest_or_sftp_work(self):
        self.client.transport.key = b"untrusted-key"
        result = self.run_smoke()
        self.assertEqual(result["state"], "HOST_KEY_MISMATCH")
        self.assertFalse(result["passed"] or self.client.sftp_opened)
        self.assertEqual(self.client.transport.commands, [])

    def test_known_key_bypass_is_still_rechecked_after_connect(self):
        original = self.client.connect

        def connect(**kwargs):
            original(**kwargs)
            self.client.transport.key = b"changed-key"
        self.client.connect = connect
        self.assertEqual(self.run_smoke()["state"], "HOST_KEY_MISMATCH")
        self.assertFalse(self.client.sftp_opened)

    def test_invalid_pins_rejected_including_noncanonical_padding(self):
        for pin in ("SHA256:abc", PIN + "==", "SHA256:" + "A" * 44, "md5:11:22"):
            with self.subTest(pin=pin), self.assertRaises(VM.SmokeError):
                VM.fingerprint(pin)
        self.assertEqual(VM.fingerprint(PIN.removeprefix("SHA256:") + "="), PIN)

    def test_wrong_artifact_architecture_is_local_failure_receipt(self):
        self.artifact.write_bytes(sample_pe("x86"))
        result = self.run_smoke()
        self.assertEqual(result["state"], "ARTIFACT_ARCH_MISMATCH")
        self.assertFalse(result["passed"] or self.client.connected)

    def test_wrong_artifact_header_floor_is_not_an_xp_pass(self):
        self.artifact.write_bytes(sample_pe(os_version=(6, 1)))
        self.assertEqual(self.run_smoke()["state"], "ARTIFACT_HEADER_FLOOR_MISMATCH")
        self.assertFalse(self.client.connected)

    def test_truncated_header_is_rejected_without_connecting(self):
        self.artifact.write_bytes(b"MZ" + b"\0" * 8)
        self.assertEqual(self.run_smoke()["state"], "INVALID_PE_HEADER")
        self.assertFalse(self.client.connected)

    def test_probe_static_policy_failure_is_retained(self):
        with mock.patch.object(VM, "_audit_probe", side_effect=ValueError(PASSWORD)):
            self.assertEqual(self.run_smoke()["state"], "PROBE_STATIC_AUDIT_FAILED")
        self.assertFalse(self.client.connected)

    def test_native_kind_uses_bound_complete_policy_marker_and_sole_exe(self):
        self.configure_native(graphics=True)
        with mock.patch.object(VM, "_audit_probe", side_effect=AssertionError("core policy must remain separate")):
            result = self.run_smoke()
        self.assertTrue(result["passed"])
        self.assertEqual(result["kind"], "native-probe")
        audit = result["artifact"]["pe"]["native_import_audit"]
        self.assertEqual(audit["state"], "STATIC_IMPORT_AUDIT_PASSED")
        self.assertEqual(audit["collector"]["host_key_sha256"], PIN)
        self.assertEqual(audit["collector"]["sha256"], hashlib.sha256(self.args.export_receipt.read_bytes()).hexdigest())
        for key in ("editor_graphics_verified", "graphics_context_verified", "physical_driver_verified", "interactive_graphics_verified"):
            self.assertFalse(result[key])
        self.assertIsNone(result["launch"]["profile_dir"])
        self.assertFalse(result["launch"]["profile_dir_fresh"])
        self.assertEqual([entry["kind"] for entry in result["launch"]["inventory"]], ["file"])
        self.assertNotIn("set ", self.client.transport.commands[-1])
        self.assert_explicit_wait(result)

    def test_native_kind_requires_export_receipt_before_transport(self):
        self.args.kind = "native-probe"
        self.args.expect_marker = NATIVE_MARKER
        result = self.run_smoke()
        self.assertEqual(result["state"], "NATIVE_PROBE_EXPORT_RECEIPT_REQUIRED")
        self.assertFalse(result["passed"] or self.client.connected)

    def test_native_kind_requires_an_explicit_marker(self):
        self.configure_native()
        self.args.expect_marker = None
        self.assertEqual(self.run_smoke()["state"], "NATIVE_PROBE_MARKER_REQUIRED")
        self.assertFalse(self.client.connected)

    def test_core_kind_still_refuses_graphics_imports(self):
        self.artifact.write_bytes(sample_pe(graphics=True))
        result = self.run_smoke()
        self.assertEqual(result["state"], "PROBE_STATIC_AUDIT_FAILED")
        self.assertFalse(self.client.connected)

    def test_native_policy_diagnostic_is_secret_free(self):
        self.configure_native()
        with mock.patch.object(VM, "_audit_native", side_effect=ValueError(PASSWORD)):
            result = self.run_smoke()
        self.assertEqual(result["state"], "NATIVE_STATIC_AUDIT_FAILED")
        self.assertNotIn(PASSWORD, (self.output() / "receipt.json").read_text())
        self.assertFalse(self.client.connected)

    def test_native_explicit_marker_is_distinct_and_complete(self):
        self.configure_native(graphics=True)
        for index, text in enumerate((VM.PROBE_MARKER, "prefix-" + NATIVE_MARKER)):
            with self.subTest(text=text):
                self.args.out = Path("build/validation/native-marker-" + str(index))
                self.client = FakeClient()
                self.client.transport.stdout = text.encode() + b"\r\n"
                result = self.run_smoke()
                self.assertEqual(result["state"], "MISSING_EXPECTED_MARKER")
                self.assertFalse(result["passed"])

    def test_native_export_target_mismatch_never_connects(self):
        self.configure_native()
        receipt = json.loads(self.args.export_receipt.read_text())
        receipt["host"] = "wrong.invalid"
        self.args.export_receipt.write_text(json.dumps(receipt))
        self.assertEqual(self.run_smoke()["state"], "NATIVE_STATIC_AUDIT_FAILED")
        self.assertFalse(self.client.connected)

    def test_invalid_kind_cannot_fall_through_to_editor_policy(self):
        self.args.kind = "unknown"
        self.assertEqual(self.run_smoke()["state"], "INVALID_ARTIFACT_KIND")
        self.assertFalse(self.client.connected)

    def test_wrong_guest_os_fails_before_upload(self):
        self.client.transport.version = b"Microsoft Windows [Version 6.1.7601]\r\n"
        result = self.run_smoke()
        self.assertEqual(result["state"], "GUEST_OS_MISMATCH")
        self.assertFalse(self.client.sftp_opened)
        self.assert_not_launched()

    def test_wrong_guest_arch_fails_before_upload(self):
        self.client.transport.arch = b"x86\r\n"
        self.assertEqual(self.run_smoke()["state"], "GUEST_ARCH_MISMATCH")
        self.assertFalse(self.client.sftp_opened)

    def test_resolved_shell_folders_is_selected_before_user_shell_folders(self):
        self.client.transport.desktop_user = (b"", b"ERROR: More data is available.\r\n", 1)
        result = self.run_smoke()
        self.assertTrue(result["passed"])
        self.assertTrue(result["guest"]["desktop_source"].endswith("\\Shell Folders"))
        self.assertEqual(len(result["guest"]["desktop_attempts"]), 1)
        self.assertEqual(result["guest"]["desktop_attempts"][0]["value_state"], "SELECTED")
        self.assertNotIn("guest-desktop-user", result["commands"])

    def test_failed_resolved_query_falls_back_to_expanded_user_value_with_evidence(self):
        error = b"ERROR: More data is available.\r\n"
        self.client.transport.desktop_resolved = (b"", error, 1)
        result = self.run_smoke()
        self.assertTrue(result["passed"])
        self.assertTrue(result["guest"]["desktop_source"].endswith("\\User Shell Folders"))
        self.assertEqual(result["guest"]["desktop"], "C:\\Documents and Settings\\Test User\\Desktop")
        self.assertEqual(result["commands"]["guest-desktop-resolved"]["exit_code"], 1)
        self.assertEqual((self.output() / "guest-desktop-resolved.stderr.bin").read_bytes(), error)
        self.assertIn("guest-desktop-user", result["commands"])
        self.assertTrue(result["delivery"]["desktop_mapping_verified"])

    def test_malformed_resolved_value_falls_back_without_guessing_profile_desktop(self):
        self.client.transport.desktop_resolved = (b"    Desktop    REG_SZ    relative-path\r\n", b"", 0)
        result = self.run_smoke()
        self.assertTrue(result["passed"])
        self.assertEqual(result["guest"]["desktop_attempts"][0]["value_state"], "UNSAFE_GUEST_PATH")
        self.assertTrue(result["guest"]["desktop_source"].endswith("\\User Shell Folders"))

    def test_both_missing_desktop_values_fail_with_both_attempts_retained(self):
        missing = (b"", b"ERROR: The system was unable to find the specified registry value.\r\n", 1)
        self.client.transport.desktop_resolved = missing
        self.client.transport.desktop_user = missing
        result = self.run_smoke()
        self.assertEqual(result["state"], "GUEST_DESKTOP_UNAVAILABLE")
        self.assertFalse(result["passed"] or self.client.sftp_opened)
        self.assertEqual(len(result["guest"]["desktop_attempts"]), 2)
        self.assertIsNone(result["guest"]["desktop_source"])
        self.assertIsNone(result["guest"]["desktop"])
        self.assertIn("guest-desktop-resolved", result["commands"])
        self.assertIn("guest-desktop-user", result["commands"])
        self.assert_not_launched()

    def test_malformed_or_unknown_expansions_cannot_become_desktop(self):
        self.client.transport.desktop_resolved = (b"    Desktop    REG_DWORD    0x1\r\n", b"", 0)
        self.client.transport.desktop_user = (b"    Desktop    REG_EXPAND_SZ    %UNKNOWN%\\Desktop\r\n", b"", 0)
        result = self.run_smoke()
        self.assertEqual(result["state"], "GUEST_DESKTOP_UNAVAILABLE")
        self.assertEqual([attempt["value_state"] for attempt in result["guest"]["desktop_attempts"]],
                         ["MALFORMED_REGISTRY_VALUE", "UNSAFE_GUEST_PATH"])
        self.assertFalse(self.client.sftp_opened)

    def test_duplicate_desktop_values_are_ambiguous_and_refused(self):
        value = b"    Desktop    REG_SZ    C:\\Desktop\r\n"
        self.client.transport.desktop_resolved = (value + value, b"", 0)
        self.client.transport.desktop_user = (value + value, b"", 0)
        result = self.run_smoke()
        self.assertEqual(result["state"], "GUEST_DESKTOP_UNAVAILABLE")
        self.assertTrue(all(attempt["value_state"] == "MALFORMED_REGISTRY_VALUE"
                            for attempt in result["guest"]["desktop_attempts"]))

    def test_desktop_mapping_must_be_observed_before_upload(self):
        self.client.transport.mapping = b"another-sftp-root"
        self.assertEqual(self.run_smoke()["state"], "SFTP_DESKTOP_MAPPING_UNVERIFIED")
        self.assertFalse(any(path.endswith(".exe") for path, mode in self.client.sftp.opens))
        self.assert_not_launched()

    def test_upload_corruption_cannot_become_a_process_pass(self):
        self.client.sftp.corrupt = True
        result = self.run_smoke()
        self.assertEqual(result["state"], "UPLOAD_HASH_MISMATCH")
        self.assertFalse(result["delivery"]["upload_verified"])
        self.assert_not_launched()

    def test_existing_remote_ownership_is_not_overwritten(self):
        self.client.sftp.collision = True
        self.assertEqual(self.run_smoke()["state"], "REMOTE_OWNERSHIP_COLLISION_OR_DENIED")
        self.assertEqual(self.client.sftp.opens, [])

    def test_existing_artifact_requires_exclusive_create(self):
        self.client.sftp.file_collision = True
        self.assertEqual(self.run_smoke()["state"], "REMOTE_ARTIFACT_EXISTS")
        self.assertTrue(all(mode in ("wx", "rb") for path, mode in self.client.sftp.opens))
        self.assert_not_launched()

    def test_timeout_is_distinct_and_closes_only_owned_channels(self):
        self.args.timeout = 1
        self.client.transport.hang = True
        result = self.run_smoke()
        self.assertEqual(result["state"], "TIMEOUT")
        self.assertIsNone(result["commands"]["artifact"]["exit_code"])
        self.assertEqual(result["modal_window_state"], "UNKNOWN")
        self.assertFalse(result["launch"]["wait_contract_completed"])
        self.assertIn("TERMINATION_UNVERIFIED", result["remote_process_cleanup"])
        self.assertTrue(all(channel.closed for channel in self.client.transport.channels))
        self.assertFalse(any("taskkill" in text.lower() for text in self.client.transport.commands))

    def test_exec_acknowledgement_wait_is_bounded_before_output_polling(self):
        self.args.timeout = 1
        self.client.transport.hang_exec_ack = True
        result = self.run_smoke()
        self.assertEqual(result["state"], "TIMEOUT")
        self.assertFalse(result["passed"])
        self.assertTrue(self.client.transport.channels[-1].closed)

    def test_sftp_handshake_wait_is_bounded_before_sftp_channel_exists(self):
        self.args.connect_timeout = 1
        self.client.hang_sftp_ack = True
        result = self.run_smoke()
        self.assertEqual(result["state"], "SFTP_HANDSHAKE_TIMEOUT")
        self.assertTrue(self.client.closed)
        self.assert_not_launched()

    def test_connect_and_authentication_wait_is_bounded(self):
        self.args.connect_timeout = 1
        self.client.hang_connect = True
        self.assertEqual(self.run_smoke()["state"], "SSH_TIMEOUT")
        self.assertTrue(self.client.closed)
        self.assertEqual(self.client.transport.commands, [])

    def test_nonzero_exit_is_not_timeout(self):
        self.client.transport.exit_code = 7
        result = self.run_smoke()
        self.assertEqual(result["state"], "NONZERO_EXIT")
        self.assertEqual(result["commands"]["artifact"]["exit_code"], 7)
        self.assert_explicit_wait(result)

    def test_exact_positive_marker_never_overrides_failed_child_exit(self):
        for index, kind in enumerate(("core-probe", "editor", "native-probe")):
            with self.subTest(kind=kind):
                self.args.kind = kind
                self.args.out = Path("build/validation/failed-child-marker-" + str(index))
                self.client = FakeClient()
                if kind == "native-probe":
                    self.configure_native(graphics=True)
                self.client.transport.exit_code = 1
                self.client.transport.stdout = (NATIVE_MARKER if kind == "native-probe" else VM.PROBE_MARKER).encode() + b"\n"
                result = self.run_smoke()
                self.assertEqual(result["state"], "NONZERO_EXIT")
                self.assertEqual(result["commands"]["artifact"]["exit_code"], 1)
                self.assertFalse(result["passed"])
                self.assert_explicit_wait(result)

    def test_failed_setup_exits_with_error_without_starting_child(self):
        self.client.transport.setup_exit_code = 5
        result = self.run_smoke()
        self.assertEqual(result["state"], "NONZERO_EXIT")
        self.assertEqual(result["commands"]["artifact"]["exit_code"], 5)
        self.assertEqual(self.client.transport.child_launches, 0)
        self.assertFalse(result["passed"])
        self.assert_explicit_wait(result)

    def test_gui_header_is_labeled_and_uses_same_explicit_wait_contract(self):
        self.args.kind = "editor"
        self.artifact.write_bytes(sample_pe(subsystem=2))
        result = self.run_smoke()
        self.assertTrue(result["passed"])
        self.assertEqual(result["artifact"]["pe"]["subsystem"], 2)
        self.assertEqual(result["artifact"]["pe"]["subsystem_name"], "WINDOWS_GUI")
        self.assert_explicit_wait(result)
        self.assertFalse(result["editor_graphics_verified"])

    def test_missing_or_broken_wait_wire_fails_ordinary_command_assertion(self):
        source = inspect.getsource(VM.launch_command)
        for index, replacement in enumerate(('start "" /b "', 'start "" /b /wait-broken "')):
            with self.subTest(replacement=replacement):
                tree = ast.parse(source)
                replaced = 0
                for node in ast.walk(tree):
                    if isinstance(node, ast.Constant) and node.value == 'start "" /b /wait "':
                        node.value = replacement
                        replaced += 1
                self.assertEqual(replaced, 1)
                namespace = dict(VM.__dict__)
                exec(compile(tree, "<mutated-launch-command>", "exec"), namespace)
                self.args.out = Path("build/validation/wait-control-" + str(index))
                self.client = FakeClient()
                self.clock = FakeClock()
                with mock.patch.object(VM, "launch_command", namespace["launch_command"]):
                    result = self.run_smoke()
                with self.assertRaises(AssertionError):
                    self.assert_explicit_wait(result)

    def test_missing_exit_or_disabled_delayed_expansion_fails_negative_exit_assertion(self):
        source = inspect.getsource(VM.launch_command)
        replacements = ((' & exit /b !errorlevel!', ''),
                        ('cmd /d /q /v:on /s /c "', 'cmd /d /q /v:off /s /c "'))
        for index, (original, replacement) in enumerate(replacements):
            with self.subTest(original=original):
                tree = ast.parse(source)
                replaced = 0
                for node in ast.walk(tree):
                    if isinstance(node, ast.Constant) and node.value == original:
                        node.value = replacement
                        replaced += 1
                self.assertEqual(replaced, 1)
                namespace = dict(VM.__dict__)
                exec(compile(tree, "<mutated-exit-propagation>", "exec"), namespace)
                self.args.out = Path("build/validation/exit-control-" + str(index))
                self.client = FakeClient()
                self.client.transport.exit_code = 1
                self.clock = FakeClock()
                with mock.patch.object(VM, "launch_command", namespace["launch_command"]):
                    result = self.run_smoke()
                self.assertEqual(result["commands"]["artifact"]["exit_code"], 0)
                with self.assertRaises(AssertionError):
                    self.assertEqual(result["state"], "NONZERO_EXIT")

    def test_missing_setup_guard_fails_ordinary_no_child_assertion(self):
        tree = ast.parse(inspect.getsource(VM.launch_command))
        replaced = 0
        for node in ast.walk(tree):
            if isinstance(node, ast.Constant) and node.value == " && ":
                node.value = " & "
                replaced += 1
        self.assertEqual(replaced, 1)
        namespace = dict(VM.__dict__)
        exec(compile(tree, "<mutated-setup-guard>", "exec"), namespace)
        self.client.transport.setup_exit_code = 5
        with mock.patch.object(VM, "launch_command", namespace["launch_command"]):
            result = self.run_smoke()
        with self.assertRaises(AssertionError):
            self.assertEqual(self.client.transport.child_launches, 0)
        with self.assertRaises(AssertionError):
            self.assertEqual(result["state"], "NONZERO_EXIT")

    def test_marker_must_be_exact_complete_line(self):
        self.client.transport.stdout = ("prefix " + VM.PROBE_MARKER + " suffix\n").encode()
        self.assertEqual(self.run_smoke()["state"], "MISSING_EXPECTED_MARKER")

    def test_marker_on_stderr_is_observed(self):
        self.client.transport.stdout = b""
        self.client.transport.stderr = VM.PROBE_MARKER.encode() + b"\n"
        self.assertTrue(self.run_smoke()["passed"])

    def test_combined_output_limit_retains_bounded_binary_prefix_and_fails(self):
        self.args.max_output_bytes = 256
        self.client.transport.stdout = b"\x00\xff" * 500
        self.client.transport.stderr = b"error" * 500
        result = self.run_smoke()
        artifact = result["commands"]["artifact"]
        self.assertEqual(result["state"], "OUTPUT_LIMIT")
        self.assertTrue(artifact["output_truncated"])
        self.assertLessEqual(artifact["stdout_bytes"] + artifact["stderr_bytes"], 256)
        self.assertEqual((self.output() / "artifact.stdout.bin").read_bytes(), b"\x00\xff" * 128)

    def test_editor_does_not_reuse_probe_allowlist_and_remains_graphics_unverified(self):
        self.args.kind = "editor"
        self.args.env = ["JCE_RENDERER=opengl"]
        with mock.patch.object(VM, "_audit_probe", side_effect=AssertionError("must not use probe policy")):
            result = self.run_smoke()
        self.assertTrue(result["passed"])
        self.assertFalse(result["editor_graphics_verified"])
        self.assertEqual(result["artifact"]["pe"]["import_policy"], "UNVERIFIED_EDITOR_REQUIRES_BROADER_POLICY")
        launch = self.client.transport.commands[-1]
        self.assertIn('set "JCE_WINDOW_HIDDEN=1"', launch)
        self.assertIn('set "JCE_WINDOW_NOT_FOCUSABLE=1"', launch)
        self.assertIn('set "JCE_MAX_SECONDS=55"', launch)
        profile_dir = result["delivery"]["windows_dir"].replace("\\", "/") + "/editor-profile"
        self.assertEqual(result["launch"]["profile_dir"], profile_dir)
        self.assertTrue(result["launch"]["profile_dir_fresh"])
        self.assertIn('set "JCE_EDITOR_PROFILE_DIR=' + profile_dir + '"', launch)
        self.assertTrue(result["launch"]["inventory_verified"])
        self.assertEqual(sorted(entry["kind"] for entry in result["launch"]["inventory"]), ["directory", "file"])

    def test_editor_locale_option_stages_exact_preferences_only_in_owned_profile(self):
        self.args = VM.build_parser().parse_args([
            "--host", "fixture.invalid", "--user", "Fixture User", "--host-key-sha256", PIN,
            "--artifact", str(self.artifact), "--expected-arch", "x64", "--kind", "editor",
            "--out", "build/validation/run", "--remote-dir", "/virtual-home/Desktop",
            "--editor-locale", "zh_cn"])
        existing = "/virtual-home/.jce/editor-preferences.json"
        self.client.sftp.files[existing] = b"previous user preferences\n"
        result = self.run_smoke()
        self.assertTrue(result["passed"], result["state"])
        entry = result["editor_locale"]
        relative = "editor-profile/editor-preferences.json"
        remote = result["delivery"]["sftp_dir"]
        self.assertEqual(self.client.sftp.files[remote + "/" + relative], b'{"language":"zh_cn"}\n')
        self.assertFalse(any(path == existing for path, _mode in self.client.sftp.opens))
        self.assertEqual(self.client.sftp.files[existing], b"previous user preferences\n")
        self.assertEqual(entry["relative_path"], relative)
        self.assertEqual(entry["effective_locale"], "UNOBSERVED")
        self.assertTrue(entry["upload_verified"] and entry["verified"])
        self.assertTrue(result["launch"]["profile_dir_fresh"])
        self.assertTrue(result["launch"]["inventory_verified"])
        self.assertEqual(self.client.transport.child_launches, 1)

    def test_editor_locale_rejects_unsupported_code_and_non_editor(self):
        self.args.editor_locale = "zh_cn"
        self.assertEqual(self.run_smoke()["state"], "EDITOR_LOCALE_EDITOR_REQUIRED")
        self.assertFalse(self.client.connected)
        self.args.kind = "editor"
        self.args.editor_locale = "../zh_cn"
        self.args.out = "build/validation/invalid-locale"
        self.assertEqual(self.run_smoke()["state"], "INVALID_EDITOR_LOCALE_REQUEST")
        self.assertFalse(self.client.connected)

    def test_editor_locale_rejects_profile_alias_before_upload(self):
        self.args.kind = "editor"
        self.args.editor_locale = "zh_cn"
        remote = self.args.remote_dir + "/jce-xp-smoke-" + NONCE + "/editor-profile"
        self.client.sftp.aliases[remote] = "/virtual-home/.jce"
        self.assertEqual(self.run_smoke()["state"], "EDITOR_PROFILE_PATH_ALIAS")
        self.assertFalse(any(name.endswith("/editor-preferences.json") for name in self.client.sftp.files))
        self.assert_not_launched()

    def test_editor_locale_rechecks_exact_profile_bytes_after_upload(self):
        self.args.kind = "editor"
        self.args.editor_locale = "zh_cn"
        remote = self.args.remote_dir + "/jce-xp-smoke-" + NONCE
        target = remote + "/editor-profile/editor-preferences.json"
        listdir_iter = self.client.sftp.listdir_iter

        def mutate_after_upload(path, read_aheads=1):
            if path == remote:
                self.client.sftp.files[target] = b'{"language":"en_us"}\n'
            yield from listdir_iter(path, read_aheads)

        self.client.sftp.listdir_iter = mutate_after_upload
        result = self.run_smoke()
        self.assertEqual(result["state"], "EDITOR_PROFILE_BYTES_UNVERIFIED")
        self.assertTrue(result["editor_locale"]["upload_verified"])
        self.assertFalse(result["editor_locale"]["verified"])
        self.assert_not_launched()

    def test_explicit_profile_outside_owned_directory_is_rejected_before_connect(self):
        self.args.kind = "editor"
        self.args.env = ["JCE_EDITOR_PROFILE_DIR=C:/Documents and Settings/Fixture User/ExistingPrefs"]
        result = self.run_smoke()
        self.assertEqual(result["state"], "EDITOR_PROFILE_OVERRIDE_FORBIDDEN")
        self.assertFalse(result["passed"] or self.client.connected)

    def test_profile_override_inside_owned_directory_is_also_tool_owned_only(self):
        self.args.kind = "editor"
        self.args.env = ["JCE_EDITOR_PROFILE_DIR=C:/Desktop/jce-xp-smoke-" + NONCE + "/editor-profile"]
        self.assertEqual(self.run_smoke()["state"], "EDITOR_PROFILE_OVERRIDE_FORBIDDEN")
        self.assertFalse(self.client.connected)

    def test_launch_changes_working_directory_to_verified_owned_folder(self):
        result = self.run_smoke()
        launch = self.client.transport.commands[-1]
        expected = 'cd /d "' + result["delivery"]["windows_dir"] + '" && '
        self.assertIn(expected, launch)
        self.assertEqual(result["launch"]["working_dir"], result["delivery"]["windows_dir"])
        self.assertEqual(self.client.sftp.removed, [result["delivery"]["sftp_dir"] + "/.jce-xp-owner"])
        self.assertTrue(result["delivery"]["mapping_marker_removed"])
        self.assertFalse(any(name.endswith(".jce-xp-owner") for name in self.client.sftp.files))

    def test_editor_profile_mkdir_collision_fails_without_launch(self):
        self.args.kind = "editor"
        self.client.sftp.profile_collision = True
        self.assertEqual(self.run_smoke()["state"], "EDITOR_PROFILE_COLLISION_OR_DENIED")
        self.assert_not_launched()

    def test_editor_profile_must_be_empty_before_launch(self):
        self.args.kind = "editor"
        self.client.sftp.profile_nonempty = True
        result = self.run_smoke()
        self.assertEqual(result["state"], "EDITOR_PROFILE_NOT_EMPTY")
        self.assertFalse(result["launch"]["profile_dir_fresh"])
        self.assert_not_launched()

    def test_extra_runtime_payload_fails_observed_launch_inventory(self):
        self.client.sftp.extra_launch_file = True
        result = self.run_smoke()
        self.assertEqual(result["state"], "UNEXPECTED_LAUNCH_INVENTORY")
        self.assertFalse(result["launch"]["inventory_verified"])
        self.assertIn("unexpected-runtime.dll", [item["name"] for item in result["launch"]["inventory"]])
        self.assert_not_launched()

    def test_optional_collection_keeps_exact_bytes_hash_and_metadata_after_exit(self):
        image = b"\x89PNG\r\n\x1a\nsynthetic-image-bytes"
        self.args.collect_relative = ["shot.png", "run.log"]
        self.client.transport.generated_files = {"shot.png": image, "run.log": b"completed\n"}
        result = self.run_smoke()
        self.assertTrue(result["passed"])
        self.assertEqual(result["process_state"], "PROCESS_PASSED")
        self.assertTrue(result["collection"]["verified"])
        self.assertFalse(result["editor_graphics_verified"])
        first = result["collection"]["files"][0]
        self.assertEqual(first["before"], first["after"])
        self.assertEqual(first["sha256"], hashlib.sha256(image).hexdigest())
        self.assertEqual(first["source_sha256"], first["sha256"])
        self.assertEqual((self.output() / "collected/shot.png").read_bytes(), image)
        self.assertEqual(result["collection"]["bytes_read"], len(image) + len(b"completed\n"))
        self.assertIn("NOT_ATOMIC", result["collection"]["path_observation"])

    def test_missing_requested_file_fails_collection_without_process_failure(self):
        self.args.collect_relative = ["missing.png"]
        result = self.run_smoke()
        self.assertEqual(result["state"], "COLLECTION_FAILED")
        self.assertEqual(result["process_state"], "PROCESS_PASSED")
        self.assertEqual(result["collection"]["files"][0]["state"], "COLLECTION_MISSING_FILE")
        self.assertFalse(result["passed"])

    def test_nonzero_exit_still_collects_evidence_and_preserves_original_failure(self):
        self.args.collect_relative = ["error.log", "missing.png"]
        self.client.transport.exit_code = 7
        self.client.transport.generated_files = {"error.log": b"original error evidence\n"}
        result = self.run_smoke()
        self.assertEqual(result["state"], "NONZERO_EXIT")
        self.assertEqual(result["process_state"], "NONZERO_EXIT")
        self.assertEqual(result["commands"]["artifact"]["exit_code"], 7)
        self.assertFalse(result["passed"] or result["collection"]["verified"])
        self.assertTrue(result["collection"]["files"][0]["verified"])
        self.assertEqual((self.output() / "collected/error.log").read_bytes(), b"original error evidence\n")

    def test_close_failure_does_not_mask_nonzero_process_and_collection_failure(self):
        self.args.collect_relative = ["missing.log"]
        self.client.transport.exit_code = 7
        with mock.patch.object(self.client.sftp, "close", side_effect=OSError(PASSWORD)):
            result = self.run_smoke()
        self.assertEqual(result["state"], "NONZERO_EXIT")
        self.assertEqual(result["process_state"], "NONZERO_EXIT")
        self.assertEqual(result["close_failures"], ["sftp"])
        self.assertFalse(result["collection"]["verified"] or result["passed"])
        self.assertNotIn(PASSWORD, (self.output() / "receipt.json").read_text())

    def test_close_failure_denies_otherwise_successful_collection(self):
        self.args.collect_relative = ["run.log"]
        self.client.transport.generated_files = {"run.log": b"completed\n"}
        with mock.patch.object(self.client.sftp, "close", side_effect=OSError(PASSWORD)):
            result = self.run_smoke()
        self.assertEqual(result["state"], "CLOSE_FAILURE")
        self.assertEqual(result["process_state"], "PROCESS_PASSED")
        self.assertTrue(result["collection"]["verified"])
        self.assertFalse(result["passed"])

    def test_timeout_does_not_collect_even_if_remote_files_exist(self):
        self.args.timeout = 1
        self.args.collect_relative = ["shot.png"]
        self.client.transport.hang = True
        self.client.transport.generated_files = {"shot.png": b"unverified-live-process-output"}
        result = self.run_smoke()
        self.assertEqual(result["state"], "TIMEOUT")
        self.assertEqual(result["collection"]["state"], "WAITING_PROCESS_EXIT")
        self.assertFalse((self.output() / "collected").exists())

    def test_collection_symlink_is_rejected_before_file_open(self):
        self.args.collect_relative = ["shot.png"]
        self.client.transport.generated_files = {"shot.png": b"outside data"}
        self.client.sftp.symlink_names.add("shot.png")
        result = self.run_smoke()
        self.assertEqual(result["collection"]["files"][0]["state"], "COLLECTION_NOT_REGULAR_FILE")
        self.assertFalse(any(path.endswith("/shot.png") and mode == "rb" for path, mode in self.client.sftp.opens))
        self.assertFalse(result["passed"])

    def test_collection_canonical_escape_is_rejected_even_with_regular_lstat(self):
        self.args.collect_relative = ["shot.png"]
        self.client.transport.generated_files = {"shot.png": b"outside data"}
        target = "/virtual-home/Desktop/jce-xp-smoke-" + NONCE + "/shot.png"
        self.client.sftp.aliases[target] = "/virtual-home/private-user-files/shot.png"
        result = self.run_smoke()
        self.assertEqual(result["collection"]["files"][0]["state"], "COLLECTION_PATH_ALIAS")
        self.assertFalse(any(path == target and mode == "rb" for path, mode in self.client.sftp.opens))

    def test_changed_metadata_retains_bytes_but_cannot_pass(self):
        self.args.collect_relative = ["run.log"]
        self.client.transport.generated_files = {"run.log": b"observed bytes\n"}
        self.client.sftp.changed_names.add("run.log")
        result = self.run_smoke()
        entry = result["collection"]["files"][0]
        self.assertEqual(entry["state"], "COLLECTION_FILE_CHANGED")
        self.assertNotEqual(entry["before"], entry["after"])
        self.assertEqual((self.output() / "collected/run.log").read_bytes(), b"observed bytes\n")
        self.assertFalse(entry["verified"] or result["passed"])

    def test_collection_count_and_path_requests_are_bounded_before_connect(self):
        bad = ["../shot.png", "/shot.png", "C:/shot.png", "profile/shot.png", "a\\shot.png",
               "shot.png.", "NUL.txt", "COM1.png", ".", ".."]
        for value in bad:
            with self.subTest(value=value), self.assertRaises(VM.SmokeError):
                VM.collection_names([value])
        for values in (["x.log"] * 2, ["shot.png", "SHOT.PNG"], [str(index) + ".log" for index in range(17)]):
            with self.subTest(values=values), self.assertRaises(VM.SmokeError):
                VM.collection_names(values)
        self.args.collect_relative = ["../shot.png"]
        self.assertEqual(self.run_smoke()["state"], "INVALID_COLLECTION_REQUEST")
        self.assertFalse(self.client.connected)

    def test_aggregate_collection_budget_is_enforced_before_second_read(self):
        self.assertEqual(VM.MAX_COLLECT_BYTES, 64 * 1024 * 1024)
        self.args.collect_relative = ["one.log", "two.log"]
        self.client.transport.generated_files = {"one.log": b"123456", "two.log": b"abcdef"}
        with mock.patch.object(VM, "MAX_COLLECT_BYTES", 10):
            result = self.run_smoke()
        self.assertEqual(result["collection"]["bytes_read"], 6)
        self.assertEqual(result["collection"]["files"][1]["state"], "COLLECTION_BYTE_LIMIT")
        self.assertFalse((self.output() / "collected/two.log").exists())
        self.assertFalse(result["passed"])

    def test_oversized_advertised_file_is_not_read(self):
        self.args.collect_relative = ["large.log"]
        self.client.transport.generated_files = {"large.log": b"tiny fixture"}
        self.client.sftp.advertised_sizes["large.log"] = VM.MAX_COLLECT_BYTES + 1
        result = self.run_smoke()
        self.assertEqual(result["collection"]["files"][0]["state"], "COLLECTION_BYTE_LIMIT")
        self.assertEqual(result["collection"]["bytes_read"], 0)
        self.assertFalse(any(path.endswith("/large.log") for path, mode in self.client.sftp.opens))

    def test_collection_redacts_credentials_and_refuses_byte_identity_pass(self):
        self.args.collect_relative = ["run.log"]
        self.client.transport.generated_files = {"run.log": PASSWORD.encode() + b"\n"}
        result = self.run_smoke()
        entry = result["collection"]["files"][0]
        self.assertEqual(entry["state"], "COLLECTION_CREDENTIAL_REDACTED")
        self.assertTrue(entry["credential_redacted"])
        self.assertNotEqual(entry["sha256"], entry["source_sha256"])
        self.assertFalse(result["passed"])
        for path in self.output().rglob("*"):
            if path.is_file():
                self.assertNotIn(PASSWORD.encode(), path.read_bytes())

    def test_default_none_has_no_collection_network_or_output(self):
        result = self.run_smoke()
        self.assertTrue(result["passed"])
        self.assertNotIn("collection", result)
        self.assertEqual(self.client.sftp.lstat_calls, {})
        self.assertFalse((self.output() / "collected").exists())

    def test_debug_pick_environment_admits_only_an_editor_name_and_frame(self):
        target = "High Joint.Target-1@80"
        env = VM.environment(["JCE_DBG_PICK=" + target], "editor", 60)
        self.assertEqual(env["JCE_DBG_PICK"], target)
        cases = [("JCE_OTHER=" + target, "editor"), ("JCE_DBG_PICK=" + target, "probe")]
        cases.extend(("JCE_DBG_PICK=" + value, "editor") for value in (
            "@80", "target", "target@-1", "target@1@2", "target@1234567890",
            "target@1&echo injected", "target%PATH%@80", "target\n@80",
            "x" * 64 + "@80",
        ))
        for value, kind in cases:
            with self.subTest(value=value, kind=kind):
                with self.assertRaisesRegex(VM.SmokeError, "^INVALID_ENVIRONMENT$"):
                    VM.environment([value], kind, 60)

    def test_illegal_environment_and_visible_editor_are_refused_before_output(self):
        for values, kind in ((["PATH=x"], "core-probe"), (["JCE_X=a&whoami"], "core-probe"),
                             (["JCE_X=%PASSWORD%"], "core-probe"), (["JCE_TOKEN=x"], "core-probe"),
                             (["JCE_X=!ERRORLEVEL!"], "core-probe"),
                             (["JCE_X=a", "JCE_X=b"], "core-probe"),
                             (["JCE_WINDOW_HIDDEN=0"], "editor"), (["JCE_MAX_SECONDS=999"], "editor")):
            with self.subTest(values=values), self.assertRaises(VM.SmokeError):
                VM.environment(values, kind, 60)

    def test_output_escape_occupied_and_unbounded_root_refused(self):
        for path, output_root in (("build/validation/../source", None), ("source/out", None),
                                  ("build/validation", None), ("build/other/run", self.repo.parent),
                                  ("build/other/run", "build")):
            with self.subTest(path=path), self.assertRaises(VM.SmokeError):
                VM.output_path(path, output_root, self.repo)
        self.output().mkdir(parents=True)
        with self.assertRaises(VM.SmokeError):
            self.run_smoke()

    def test_explicit_output_root_is_bounded_inside_build(self):
        selected = VM.output_path("build/compat-smoke/run", "build/compat-smoke", self.repo)
        self.assertEqual(selected, self.repo / "build/compat-smoke/run")

    def test_default_sftp_resolver_observes_home_and_verifies_mapping(self):
        self.args.remote_dir = None
        result = self.run_smoke()
        self.assertTrue(result["passed"])
        self.assertTrue(result["delivery"]["sftp_dir"].startswith("/virtual-home/Desktop/"))

    def test_safe_output_retains_final_false_receipt_for_invalid_request(self):
        for index, (name, value, expected) in enumerate((
                ("host_key_sha256", "bad", "INVALID_HOST_KEY_PIN"),
                ("env", ["PATH=bad"], "INVALID_ENVIRONMENT"),
                ("guest_encoding", "not-a-codec", "INVALID_GUEST_ENCODING"),
                ("timeout", 0, "INVALID_BUDGET"))):
            previous = getattr(self.args, name)
            self.args.out = Path("build/validation/run-" + str(index))
            setattr(self.args, name, value)
            try:
                result = self.run_smoke()
                self.assertEqual(result["state"], expected)
                self.assertFalse(result["passed"])
                self.assertTrue((self.output() / "receipt.json").is_file())
            finally:
                setattr(self.args, name, previous)
        self.assertFalse(self.client.connected)

    def test_unsafe_guest_and_sftp_paths_rejected(self):
        for path in ('C:\\Desktop&whoami', 'C:\\Desktop\\..\\Windows', '\\\\server\\share',
                     'C:\\%TEMP%', 'C:\\Desk!top'):
            with self.subTest(path=path), self.assertRaises(VM.SmokeError):
                VM._windows_path(path)
        for path in ("/Desktop/../Windows", "/Desktop;\nwhoami", "%USERPROFILE%/Desktop"):
            with self.subTest(path=path), self.assertRaises(VM.SmokeError):
                VM._sftp_path(path)

    def test_password_is_absent_from_receipt_raw_bytes_and_exception_repr(self):
        self.client.transport.stdout = PASSWORD.encode() + b"\n" + VM.PROBE_MARKER.encode() + b"\n"
        result = self.run_smoke()
        self.assertTrue(result["passed"])
        self.assertTrue(result["commands"]["artifact"]["stdout_redacted"])
        for path in self.output().iterdir():
            self.assertNotIn(PASSWORD.encode(), path.read_bytes())

    def test_authentication_exception_diagnostic_is_never_persisted(self):
        self.client.connect_error = RuntimeError("native password error: " + PASSWORD)
        result = self.run_smoke()
        self.assertEqual(result["state"], "SSH_OR_LOCAL_FAILURE")
        self.assertNotIn(PASSWORD, (self.output() / "receipt.json").read_text())
        self.assertFalse(result["passed"])

    def test_numeric_password_redaction_preserves_valid_json(self):
        result = self.run_smoke(password="1")
        self.assertTrue(result["passed"])
        receipt = json.loads((self.output() / "receipt.json").read_text())
        self.assertEqual(receipt["schema"], "jce.xp.vm-smoke.v1")
        self.assertEqual(receipt["host_key_sha256"], PIN)
        self.assertEqual(receipt["artifact"]["sha256"], hashlib.sha256(self.artifact.read_bytes()).hexdigest())

    def test_native_guest_encoding_password_bytes_are_redacted(self):
        password = "caf\u00e9-private-password"
        self.args.guest_encoding = "cp1252"
        self.client.transport.stdout = password.encode("cp1252") + b"\n" + VM.PROBE_MARKER.encode() + b"\n"
        self.assertTrue(self.run_smoke(password=password)["passed"])
        for path in self.output().iterdir():
            self.assertNotIn(password.encode("cp1252"), path.read_bytes())

    def test_module_import_and_help_do_not_import_paramiko(self):
        real_import = __import__

        def import_without_network(name, *args, **kwargs):
            if name == "paramiko" or name.startswith("paramiko."):
                raise AssertionError("network dependency imported before connection")
            return real_import(name, *args, **kwargs)
        with mock.patch("builtins.__import__", side_effect=import_without_network):
            spec = importlib.util.spec_from_file_location("xp_vm_lazy_control", ROOT / "tools/build/xp_vm.py")
            owner = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(owner)
            self.assertIn("--host-key-sha256", owner.build_parser().format_help())

    def test_cli_does_not_accept_password_argument(self):
        parser = VM.build_parser()
        self.assertNotIn("--password", parser._option_string_actions)
        self.assertIn("--password-env", parser._option_string_actions)

    def test_only_three_kinds_and_shared_wrapper_forwards_explicit_inputs(self):
        self.assertEqual(set(VM.build_parser()._option_string_actions["--kind"].choices), {"core-probe", "native-probe", "editor"})
        tree = ast.parse((ROOT / "tools/build/jce.py").read_text(encoding="utf-8"))
        function = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "cmd_xp_vm")
        calls = []
        namespace = {"sys": SimpleNamespace(executable="python"), "ROOT": ROOT, "_run_xp_tool": lambda label, command: calls.append(command)}
        exec(compile(ast.Module(body=[function], type_ignores=[]), "actual-xp-vm-forwarder", "exec"), namespace)
        self.args.export_receipt = self.repo / "collector/receipt.json"
        namespace["cmd_xp_vm"](self.args)
        command = calls[0]
        self.assertIn("--export-receipt", command)
        self.assertEqual(command[command.index("--export-receipt") + 1], str(self.args.export_receipt))
        self.assertNotIn(PASSWORD, command)
        self.assertNotIn("--editor-locale", command)

        self.args.kind = "editor"
        self.args.editor_locale = "zh_cn"
        namespace["cmd_xp_vm"](self.args)
        command = calls[1]
        self.assertEqual(command.count("--editor-locale"), 1)
        self.assertEqual(command[command.index("--editor-locale") + 1], "zh_cn")


if __name__ == "__main__":
    unittest.main()
