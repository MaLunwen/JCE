"""Actual native inspector export plus exclusive-output failure controls."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools import shader_payload


def container(payload):
    # A fixture writer, not a parser: no uniforms; header, code and terminator.
    return b"FSH\x0b" + struct.pack("<IIHI", 0x44332211, 0, 0, len(payload)) + payload + b"\0"


class PayloadFixture(unittest.TestCase):
    def setUp(self):
        work = ROOT / "build/tests/shader-payload"
        work.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=work)
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.source = self.work / "input.bin"
        self.output = self.work / "payload.dxbc"
        self.inspector = self.work / "inspector.fake"
        self.inspector.write_bytes(b"identity")
        self.payload = b"DXBC\r\n\x00\xffbinary"
        self.original = container(self.payload)
        self.source.write_bytes(self.original)
        self.report = dict(container_size=len(self.original), code_offset=18,
                           code_size=len(self.payload), payload_hex=self.payload.hex())

    def export(self, report=None, **kwargs):
        result = subprocess.CompletedProcess([], 0, json.dumps(report or self.report).encode(), b"")
        with mock.patch.object(shader_payload.subprocess, "run", return_value=result):
            return shader_payload.export_payload(self.inspector, self.source, self.output, **kwargs)


class PayloadControls(PayloadFixture):
    def test_exact_bytes_and_hashes(self):
        result = self.export()
        self.assertEqual(self.output.read_bytes(), self.payload)
        self.assertEqual(result["container_sha256"], hashlib.sha256(self.original).hexdigest())
        self.assertEqual(result["payload_sha256"], hashlib.sha256(self.payload).hexdigest())

    def test_existing_file_and_input_alias_survive(self):
        self.output.write_bytes(b"existing")
        with self.assertRaises(FileExistsError):
            self.export()
        self.assertEqual(self.output.read_bytes(), b"existing")
        self.output.unlink()
        os.link(self.source, self.output)
        with self.assertRaises(FileExistsError):
            self.export()
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_input_path_survives(self):
        self.output = self.source
        with self.assertRaises(FileExistsError):
            self.export()
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_competing_creator_survives(self):
        def run(*_args, **_kwargs):
            self.output.write_bytes(b"competing owner")
            return subprocess.CompletedProcess([], 0, json.dumps(self.report).encode(), b"")
        with mock.patch.object(shader_payload.subprocess, "run", side_effect=run):
            with self.assertRaises(FileExistsError):
                shader_payload.export_payload(self.inspector, self.source, self.output)
        self.assertEqual(self.output.read_bytes(), b"competing owner")

    def test_directory_and_symlink_survive(self):
        self.output.mkdir()
        with self.assertRaises(OSError):
            self.export()
        self.output.rmdir()
        try:
            self.output.symlink_to(self.source)
        except OSError as error:
            self.skipTest(f"host symlink creation unavailable: {error}")
        with self.assertRaises(FileExistsError):
            self.export()
        self.assertTrue(self.output.is_symlink())
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_ranges_hex_and_whole_container_control(self):
        changes = [dict(code_offset=-1), dict(code_offset=True), dict(code_size=True),
                   dict(container_size=True), dict(code_size=len(self.original) + 1),
                   dict(payload_hex="00"), dict(payload_hex="z" * len(self.payload) * 2),
                   dict(payload_hex="00" * len(self.payload)),
                   dict(code_offset=0, code_size=len(self.original), payload_hex=self.original.hex())]
        for change in changes:
            report = dict(self.report, **change)
            if change.get("code_offset") == 0:
                # This ordinary old-export control has a valid byte range but
                # contains the whole container; the caller's byte assertion fails.
                self.export(report)
                with self.assertRaises(AssertionError):
                    self.assertEqual(self.output.read_bytes(), self.payload)
                self.output.unlink()
            else:
                with self.assertRaises(ValueError, msg=change):
                    self.export(report)
                self.assertFalse(self.output.exists())

    def test_limits_and_tool_failure_create_nothing(self):
        for limit in (True, 0, -1, shader_payload.MAX_LIMIT + 1, len(self.original) - 1):
            with self.assertRaises(ValueError):
                self.export(max_bytes=limit)
        with mock.patch.object(shader_payload.subprocess, "run", side_effect=subprocess.CalledProcessError(2, [])):
            with self.assertRaises(subprocess.CalledProcessError):
                shader_payload.export_payload(self.inspector, self.source, self.output)
        self.assertFalse(self.output.exists())

    def test_changed_input_refuses_before_creation(self):
        def run(*_args, **_kwargs):
            self.source.write_bytes(self.original + b"changed")
            return subprocess.CompletedProcess([], 0, json.dumps(self.report).encode(), b"")
        with mock.patch.object(shader_payload.subprocess, "run", side_effect=run):
            with self.assertRaisesRegex(ValueError, "changed"):
                shader_payload.export_payload(self.inspector, self.source, self.output)
        self.assertFalse(self.output.exists())

    def test_changed_inspector_refuses_before_creation(self):
        def run(*_args, **_kwargs):
            self.inspector.write_bytes(b"different inspector")
            return subprocess.CompletedProcess([], 0, json.dumps(self.report).encode(), b"")
        with mock.patch.object(shader_payload.subprocess, "run", side_effect=run):
            with self.assertRaisesRegex(ValueError, "inspector changed"):
                shader_payload.export_payload(self.inspector, self.source, self.output)
        self.assertFalse(self.output.exists())

    def test_partial_write_and_close_failure_remove_only_owned_file(self):
        original_open = Path.open
        for failure in ("short", "close", "replacement"):
            output = self.output

            class Stream:
                def __init__(self, actual):
                    self.actual = actual
                def __enter__(self):
                    return self
                def fileno(self):
                    return self.actual.fileno()
                def write(self, data):
                    return self.actual.write(data[:-1] if failure == "short" else data)
                def flush(self):
                    self.actual.flush()
                def __exit__(self, *_args):
                    self.actual.close()
                    if failure == "replacement":
                        output.unlink()
                        with original_open(output, "wb") as replacement:
                            replacement.write(b"other owner")
                    if failure != "short":
                        raise OSError("forced close failure after actual close")

            def opening(path, mode="r", *args, **kwargs):
                actual = original_open(path, mode, *args, **kwargs)
                return Stream(actual) if path == output and mode == "xb" else actual

            with mock.patch.object(Path, "open", opening):
                with self.assertRaises(OSError):
                    self.export()
            if failure == "replacement":
                self.assertEqual(output.read_bytes(), b"other owner")
                output.unlink()
            else:
                self.assertFalse(output.exists())
            self.assertEqual(self.source.read_bytes(), self.original)


@unittest.skipUnless(os.environ.get("JCE_TEST_SHADER_INSPECT"), "explicit native inspector required")
class NativeInspector(PayloadFixture):
    def native(self, *args, check=True):
        return subprocess.run([os.environ["JCE_TEST_SHADER_INSPECT"], str(self.source), *args],
                              capture_output=True, check=check, timeout=30)

    def test_native_payload_exact(self):
        call = self.native("--json", "--payload-hex", check=False)
        self.assertEqual(call.returncode, 0, call.stderr.decode(errors="replace"))
        report = json.loads(call.stdout)
        self.assertEqual(bytes.fromhex(report["payload_hex"]), self.payload)
        self.assertEqual(report["code_offset"], 18)
        result = shader_payload.export_payload(os.environ["JCE_TEST_SHADER_INSPECT"], self.source, self.output)
        self.assertEqual(self.output.read_bytes(), self.payload)
        self.assertEqual(result["payload_bytes"], len(self.payload))

    def test_native_default_json_and_code_unchanged(self):
        baseline = os.environ.get("JCE_TEST_SHADER_INSPECT_BASELINE")
        self.assertNotIn("payload_hex", json.loads(self.native("--json").stdout))
        self.assertNotIn("code_offset", json.loads(self.native("--json").stdout))
        for payload in (self.payload, b"void main(){}"):
            self.source.write_bytes(container(payload))
            for flags in (("--json",), ("--json", "--code"), ("--code",)):
                current = self.native(*flags)
                if baseline:
                    old = subprocess.run([baseline, str(self.source), *flags], capture_output=True, check=True, timeout=30)
                    self.assertEqual(current.stdout, old.stdout)

    def test_native_refuses_invalid_and_truncated_containers(self):
        for bad in (b"XYZ" + self.original[3:], self.original[:-1], self.original[:18]):
            self.source.write_bytes(bad)
            result = self.native("--json", "--payload-hex", check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(result.stdout)
            with self.assertRaises(subprocess.CalledProcessError):
                shader_payload.export_payload(os.environ["JCE_TEST_SHADER_INSPECT"], self.source, self.output)
            self.assertFalse(self.output.exists())

    def test_native_requires_json(self):
        self.assertNotEqual(self.native("--payload-hex", check=False).returncode, 0)


if __name__ == "__main__":
    unittest.main()
