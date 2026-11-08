"""Pure collector-bound native PE policy controls; no SSH/native execution."""
import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value
POLICY = load("xp_native_policy_controls", ROOT / "tools/build/xp_native_probe.py")
CORE = load("xp_native_core_controls", ROOT / "tools/build/xp_compat.py")
STANDALONE = load("xp_native_pe_reader", ROOT / "tools/build/standalone.py")
HOST, USER = "fixture.invalid", "Fixture User"
PIN = "SHA256:" + base64.b64encode(hashlib.sha256(b"public native fixture key").digest()).decode().rstrip("=")


def sample_pe(arch="x64", dlls=None, delay=False, floor=None):
    """Synthetic PE imports, never executable machine instructions."""
    dlls = dict(dlls) if dlls is not None else {"kernel32.dll": ["GetProcAddress"], "msvcrt.dll": ["printf"]}
    assert len(dlls) <= 8 and sum(len(value) for value in dlls.values()) <= 32
    raw = bytearray(8192); raw[:2] = b"MZ"
    struct.pack_into("<I", raw, 0x3c, 0x80); raw[0x80:0x84] = b"PE\0\0"
    wide = arch == "x64"; size = 240 if wide else 224
    struct.pack_into("<HHIIIHH", raw, 0x84, 0x8664 if wide else 0x14c, 1, 0, 0, 0, size, 0)
    opt = 0x98; struct.pack_into("<H", raw, opt, 0x20b if wide else 0x10b)
    version = floor or ((5, 2) if wide else (5, 1))
    struct.pack_into("<HH", raw, opt + 40, *version); struct.pack_into("<HH", raw, opt + 48, *version)
    struct.pack_into("<H", raw, opt + 68, 3)
    struct.pack_into("<I", raw, opt + (108 if wide else 92), 16)
    data = opt + (112 if wide else 96)
    struct.pack_into("<II", raw, data + 8, 0x1000, (len(dlls) + 1) * 20)
    section = opt + size; raw[section:section + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", raw, section + 8, 0x1800, 0x1000, 0x1800, 0x200)
    def off(rva): return rva - 0xe00
    symbol_index = 0
    for index, (dll, symbols) in enumerate(dlls.items()):
        name, thunk = 0x1100 + index * 0x40, 0x1400 + index * 0x100
        struct.pack_into("<IIIII", raw, off(0x1000 + index * 20), thunk, 0, 0, name, thunk)
        encoded = dll.encode() + b"\0"; raw[off(name):off(name) + len(encoded)] = encoded
        for number, symbol in enumerate(symbols):
            if type(symbol) is int:
                value = (1 << (63 if wide else 31)) | symbol
            else:
                hint = 0x2000 + symbol_index * 0x40; symbol_index += 1
                encoded = b"\0\0" + symbol.encode() + b"\0"
                raw[off(hint):off(hint) + len(encoded)] = encoded; value = hint
            struct.pack_into("<Q" if wide else "<I", raw, off(thunk) + number * (8 if wide else 4), value)
    if delay:
        struct.pack_into("<II", raw, data + 13 * 8, 0x2500, 64)
        struct.pack_into("<IIIIIIII", raw, off(0x2500), 1, 0x2600, 0, 0x2640, 0x2640, 0, 0, 0)
        raw[off(0x2600):off(0x2600) + 11] = b"user32.dll\0"
        struct.pack_into("<Q" if wide else "<I", raw, off(0x2640), 0x2680)
        raw[off(0x2680):off(0x2680) + 14] = b"\0\0MessageBoxW\0"
    return bytes(raw)


def write_export_receipt(directory, artifact, arch="x64", host=HOST, user=USER, pin=PIN):
    """Synthetic collector evidence for injected policy/transport controls only."""
    directory.mkdir(parents=True, exist_ok=True)
    dlls = {}
    for row in STANDALONE._pe_import_records(artifact.read_bytes()):
        entry = dlls.setdefault(row["dll"], {"sha256": "a" * 64, "names": [], "ordinals": [], "forwarders": {}})
        for symbol in row["symbols"]:
            entry["ordinals" if type(symbol) is int else "names"].append(symbol)
    guest = {"arch": arch, "os_version": [5, 2] if arch == "x64" else [5, 1]}
    raw = (json.dumps({"schema": "jce.xp.guest-exports.v1", "guest": guest, "dlls": dlls}) + "\n").encode()
    (directory / "inventory.json").write_bytes(raw)
    receipt = {"schema": "jce.xp.guest-export-collection.v1", "state": "GUEST_EXPORTS_COLLECTED", "collected": True,
        "timestamp_utc": "2026-01-01T00:00:00+00:00", "host": host, "user": user, "host_key_sha256": pin,
        "expected_arch": arch, "guest": guest, "inventory": {"local_relative": "inventory.json", "sha256": hashlib.sha256(raw).hexdigest()}}
    path = directory / "receipt.json"; path.write_text(json.dumps(receipt), encoding="utf-8", newline="\n")
    return path


class NativePolicyControls(unittest.TestCase):
    def setUp(self):
        root = ROOT / "build/validation"; root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="xp-native-policy-", dir=root)
        self.addCleanup(self.temporary.cleanup); self.path = Path(self.temporary.name)
        self.artifact = self.path / "synthetic.exe"; self.artifact.write_bytes(sample_pe())
        self.receipt = write_export_receipt(self.path / "exports", self.artifact)
    def audit(self, arch="x64", **changes):
        args = {"host": HOST, "user": USER, "host_key_sha256": PIN}; args.update(changes)
        return POLICY.audit_pe(self.artifact, arch, self.receipt, **args)
    def edit_receipt(self, **changes):
        value = json.loads(self.receipt.read_text()); value.update(changes); self.receipt.write_text(json.dumps(value))
    def edit_inventory(self, edit):
        path = self.receipt.parent / "inventory.json"; value = json.loads(path.read_text()); edit(value)
        path.write_text(json.dumps(value)); receipt = json.loads(self.receipt.read_text())
        receipt["inventory"]["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest(); self.receipt.write_text(json.dumps(receipt))
    def test_console_both_arches_and_no_artificial_entropy_requirement(self):
        for arch in ("x86", "x64"):
            with self.subTest(arch=arch):
                self.artifact.write_bytes(sample_pe(arch)); self.receipt = write_export_receipt(self.path / arch, self.artifact, arch)
                result = self.audit(arch)
                self.assertEqual(result["state"], "STATIC_IMPORT_AUDIT_PASSED")
                self.assertEqual(result["pe"]["subsystem"], "CONSOLE")
                self.assertFalse(result["graphics_context_verified"] or result["physical_driver_verified"])
                with self.assertRaisesRegex(ValueError, "CryptoAPI"):
                    CORE.audit_pe(self.artifact, arch)
    def test_unknown_symbol_cannot_pass_a_hash_valid_inventory(self):
        self.edit_inventory(lambda v: v["dlls"]["kernel32.dll"].update(names=[]))
        with self.assertRaisesRegex(ValueError, "absent from exact XP"):
            self.audit()
    def test_psapi_component_is_admitted_only_by_captured_exports(self):
        self.artifact.write_bytes(sample_pe(dlls={"kernel32.dll": ["GetProcAddress"], "msvcrt.dll": ["printf"], "psapi.dll": ["GetProcessMemoryInfo"]}))
        self.receipt = write_export_receipt(self.path / "psapi", self.artifact)
        self.assertEqual(self.audit()["state"], "STATIC_IMPORT_AUDIT_PASSED")
        with self.assertRaisesRegex(ValueError, "PSAPI"):
            CORE.audit_pe(self.artifact, "x64")
    def test_host_user_pin_arch_and_capture_state_are_bound(self):
        for change in ({"host": "wrong.invalid"}, {"user": "wrong"},
                       {"host_key_sha256": "SHA256:" + base64.b64encode(b"x" * 32).decode().rstrip("=")}):
            with self.subTest(change=change), self.assertRaises(ValueError): self.audit(**change)
        for change in ({"expected_arch": "x86"}, {"collected": False}, {"state": "COLLECTION_CLOSE_FAILURE"}):
            self.receipt = write_export_receipt(self.path / "exports", self.artifact); self.edit_receipt(**change)
            with self.subTest(change=change), self.assertRaises(ValueError): self.audit()
    def test_missing_receipt_wrong_hash_and_nonadjacent_inventory_fail(self):
        with self.assertRaises((ValueError, TypeError)):
            POLICY.audit_pe(self.artifact, "x64", None, host=HOST, user=USER, host_key_sha256=PIN)
        self.edit_receipt(inventory={"local_relative": "../inventory.json", "sha256": "a" * 64})
        with self.assertRaisesRegex(ValueError, "path mismatch"): self.audit()
        self.edit_receipt(inventory={"local_relative": "inventory.json", "sha256": "a" * 64})
        with self.assertRaisesRegex(ValueError, "hash mismatch"): self.audit()
    def test_alias_and_oversize_evidence_are_refused(self):
        real_lstat = Path.lstat
        def alias(path):
            result = real_lstat(path)
            if path == self.receipt.absolute():
                return type("Alias", (), {"st_mode": result.st_mode, "st_file_attributes": 0x400})()
            return result
        with mock.patch.object(Path, "lstat", alias), self.assertRaisesRegex(ValueError, "alias"): self.audit()
        with mock.patch.object(POLICY, "MAX_RECEIPT_BYTES", 1), self.assertRaisesRegex(ValueError, "size"): self.audit()
    def test_delayed_ordinal_and_forwarder_imports_use_full_owner(self):
        self.artifact.write_bytes(sample_pe(dlls={"kernel32.dll": [42], "msvcrt.dll": ["printf"]}, delay=True))
        self.receipt = write_export_receipt(self.path / "complex", self.artifact)
        self.assertEqual(self.audit()["state"], "STATIC_IMPORT_AUDIT_PASSED")
        self.edit_inventory(lambda v: v["dlls"]["user32.dll"].update(names=[]))
        with self.assertRaisesRegex(ValueError, "MessageBoxW"): self.audit()
        self.receipt = write_export_receipt(self.path / "complex", self.artifact)
        self.edit_inventory(lambda v: v["dlls"]["kernel32.dll"]["forwarders"].update({"42": {"dll": "ntdll.dll", "symbol": "LdrGetProcedureAddress"}}))
        with self.assertRaisesRegex(ValueError, "provenance"): self.audit()
    def test_wrong_artifact_arch_or_floor_and_cng_still_fail(self):
        self.artifact.write_bytes(sample_pe("x86"))
        with self.assertRaises(ValueError): self.audit()
        self.artifact.write_bytes(sample_pe(floor=(6, 1)))
        with self.assertRaises(ValueError): self.audit()
        self.artifact.write_bytes(sample_pe(dlls={"msvcrt.dll": ["printf"], "bcrypt.dll": ["BCryptGenRandom"]}))
        self.receipt = write_export_receipt(self.path / "cng", self.artifact)
        with self.assertRaises(ValueError): self.audit()
    def test_static_path_has_no_conan_or_network_dependency(self):
        original = __import__
        def no_runtime_service(name, *args, **kwargs):
            if name == "conan" or name.startswith("conan.") or name == "paramiko" or name.startswith("paramiko."):
                raise AssertionError("optional service imported during static native admission")
            return original(name, *args, **kwargs)
        with mock.patch("builtins.__import__", side_effect=no_runtime_service):
            self.assertEqual(self.audit()["state"], "STATIC_IMPORT_AUDIT_PASSED")
    def test_evidence_change_during_real_delegated_audit_is_refused(self):
        original = POLICY._module
        for target in ("receipt.json", "inventory.json"):
            self.receipt = write_export_receipt(self.path / "exports", self.artifact)
            changed = self.receipt.parent / target
            def module(name):
                owner = original(name)
                if name == "xp_editor":
                    audit = owner.audit_pe
                    def mutate(*args, **kwargs):
                        result = audit(*args, **kwargs)
                        with changed.open("ab") as stream: stream.write(b" ")
                        return result
                    owner.audit_pe = mutate
                return owner
            with self.subTest(target=target), mock.patch.object(POLICY, "_module", side_effect=module), self.assertRaisesRegex(ValueError, "changed during audit"):
                self.audit()


if __name__ == "__main__": unittest.main()
