"""Real metadata/file admission controls; placeholders are not compiled archives."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("sdk_target_reader", ROOT / "tools/build/sdk_target.py")
SDK_TARGET = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SDK_TARGET)


class SdkTargetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.sdk = Path(self.temp.name) / "misleading-linux-arm64-sdk"

    def metadata(self, **changes):
        data = dict(schema=1, system="Windows", arch="x86_64", pointer_bits=64,
                    config="Release", variant="dist", delivery="embedded",
                    crt="MultiThreaded", archive="jce_engine_core.lib")
        data.update(changes)
        return data

    def write(self, data=None, *, selected=None, archive=True):
        data = self.metadata() if data is None else data
        folder = self.sdk / "lib" / (selected or data["config"])
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / "jce-sdk-target.json"
        path.write_text(json.dumps(data), encoding="utf-8")
        if archive:
            # Presence-only test fixture: these bytes cannot certify linkage/ABI.
            name = "jce_engine_core.lib" if data["system"] == "Windows" else "jce_engine_core.a"
            (folder / name).write_bytes(b"metadata-reader archive placeholder")
        return path

    def require(self, data=None, *, sdk=None, **changes):
        data = self.metadata() if data is None else data
        requested = {key: data[key] for key in ("system", "arch", "config", "variant", "delivery")}
        requested.update(changes)
        return SDK_TARGET.require_sdk_target(sdk or self.sdk, **requested)

    def test_valid_windows_delivery_config_and_crt_matrix(self):
        for variant in ("debug", "release", "dist"):
            config = "Debug" if variant == "debug" else "Release"
            static = "MultiThreadedDebug" if config == "Debug" else "MultiThreaded"
            for delivery, crt in (("embedded", static), ("authoring", static),
                                  ("authoring", static + "DLL")):
                with self.subTest(variant=variant, delivery=delivery, crt=crt):
                    data = self.metadata(variant=variant, config=config, delivery=delivery, crt=crt)
                    self.write(data)
                    self.assertEqual(data, self.require(data))

    def test_valid_non_windows_system_and_pointer_width_matrix(self):
        for system, arch, bits in (("Linux", "i686", 32), ("Linux", "x86_64", 64),
                                   ("Darwin", "aarch64", 64), ("Android", "armv7", 32),
                                   ("iOS", "aarch64", 64), ("Emscripten", "wasm32", 32),
                                   ("Emscripten", "wasm64", 64)):
            with self.subTest(system=system, arch=arch):
                data = self.metadata(system=system, arch=arch, pointer_bits=bits,
                                     crt="none", archive="jce_engine_core.a")
                self.write(data)
                self.assertEqual(data, self.require(data))

    def test_root_relocation_does_not_change_target(self):
        self.write()
        relocated = self.sdk.parent / "renamed SDK with spaces"
        self.sdk.rename(relocated)
        self.assertEqual(self.metadata(), self.require(sdk=relocated))

    def test_missing_metadata_does_not_borrow_sibling_release(self):
        self.write()
        with self.assertRaisesRegex(ValueError, "metadata unreadable"):
            self.require(config="Debug", variant="debug")

    def test_missing_archive_does_not_borrow_sibling_release(self):
        self.write()
        debug = self.metadata(config="Debug", variant="debug", crt="MultiThreadedDebug")
        self.write(debug, archive=False)
        with self.assertRaisesRegex(ValueError, "archive missing"):
            self.require(debug)

    def test_archive_directory_is_not_an_archive(self):
        path = self.write(archive=False)
        (path.parent / "jce_engine_core.lib").mkdir()
        with self.assertRaisesRegex(ValueError, "archive missing"):
            self.require()

    def test_old_debug_path_cannot_claim_release_target(self):
        self.write(selected="Debug")
        with self.assertRaisesRegex(ValueError, "config mismatch"):
            self.require(config="Debug", variant="debug")

    def test_arch_and_system_mismatch(self):
        self.write()
        for field, value in (("arch", "aarch64"), ("system", "Linux")):
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, field + " mismatch"):
                self.require(**{field: value})

    def test_delivery_and_variant_match_exactly(self):
        self.write()
        for field, value in (("delivery", "authoring"), ("variant", "release")):
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, field + " mismatch"):
                self.require(**{field: value})

    def test_malformed_json_root_and_utf8(self):
        path = self.write()
        for content in (b"not JSON", b"[1]", b"null", b"\xff",
                        json.dumps(self.metadata()).replace('"schema": 1', '"schema": NaN').encode(),
                        (json.dumps(self.metadata())[:-1] + ',"extension":Infinity}').encode()):
            with self.subTest(content=content):
                path.write_bytes(content)
                with self.assertRaises(ValueError):
                    self.require()

    def test_metadata_size_is_bounded_in_bytes(self):
        path = self.write()
        encoded = path.read_bytes()
        path.write_bytes(encoded + b" " * (16384 - len(encoded)))
        self.assertEqual(self.metadata(), self.require())
        path.write_bytes(encoded + b" " * (16385 - len(encoded)))
        with self.assertRaisesRegex(ValueError, "exceeds 16384 bytes"):
            self.require()

    def test_schema_rejects_unknown_fields(self):
        data = self.metadata(compiler_guess="host", extension={"nested": "not schema 1"})
        self.write(data)
        with self.assertRaisesRegex(ValueError, "unknown fields"):
            self.require()

    def test_generated_schema_rejects_escaped_aliases_and_embedded_nul(self):
        path = self.write()
        base = json.dumps(self.metadata())
        controls = (
            json.dumps(self.metadata(arch="aarch64"))[:-1] + ',"ar\\u0063h":"x86_64"}',
            base.replace('"arch"', '"ar\\u0063h"'),
            base.replace('MultiThreaded', 'MultiThreaded\\u0000DLL'))
        for text in controls:
            with self.subTest(text=text):
                path.write_text(text, encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "unescaped ASCII"):
                    self.require()

    def test_duplicate_json_keys_rejected(self):
        path = self.write()
        for key in ("schema", "arch", "crt"):
            with self.subTest(key=key):
                content = json.dumps(self.metadata())
                path.write_text(content[:-1] + "," + json.dumps(key) + ":null}", encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
                    self.require()

    def test_all_required_fields_checked(self):
        for field in self.metadata():
            with self.subTest(field=field):
                data = self.metadata()
                del data[field]
                self.write(data, selected="Release", archive=False)
                with self.assertRaisesRegex(ValueError, "lacks"):
                    self.require()

    def test_schema_is_integer_one(self):
        for schema in (True, False, 1.0, 0, 2, "1", None):
            with self.subTest(schema=schema):
                self.write(self.metadata(schema=schema))
                with self.assertRaisesRegex(ValueError, "schema"):
                    self.require()

    def test_pointer_bits_matches_arch_and_rejects_bool_float(self):
        for bits in (32, True, 64.0, "64", None):
            with self.subTest(bits=bits):
                self.write(self.metadata(pointer_bits=bits))
                with self.assertRaisesRegex(ValueError, "pointer_bits"):
                    self.require()

    def test_field_types_and_unknown_enum_values_rejected(self):
        for field, value in (("system", "windows"), ("arch", "x64"), ("config", "release"),
                              ("variant", "unknown"), ("delivery", []), ("crt", False)):
            with self.subTest(field=field):
                self.write(self.metadata(**{field: value}), selected="Release")
                with self.assertRaisesRegex(ValueError, field):
                    self.require()

    def test_variant_requires_its_actual_configuration(self):
        for variant, config in (("release", "Debug"), ("dist", "Debug"),
                                 ("debug", "Release")):
            with self.subTest(variant=variant, config=config):
                data = self.metadata(variant=variant, config=config)
                self.write(data, selected="Release")
                with self.assertRaisesRegex(ValueError, "requires config"):
                    self.require()

    def test_crt_admission_rejects_dynamic_embedded_and_wrong_debug(self):
        for crt in ("MultiThreadedDLL", "MultiThreadedDebug", "none", "multithreaded"):
            with self.subTest(crt=crt):
                self.write(self.metadata(crt=crt))
                with self.assertRaisesRegex(ValueError, "CRT|crt"):
                    self.require()
        data = self.metadata(config="Debug", variant="debug", crt="MultiThreaded")
        self.write(data)
        with self.assertRaisesRegex(ValueError, "crt"):
            self.require(data)
        data = self.metadata(delivery="authoring", crt="none")
        self.write(data)
        with self.assertRaisesRegex(ValueError, "crt"):
            self.require(data)

    def test_non_windows_requires_none_crt(self):
        data = self.metadata(system="Linux", crt="MultiThreaded", archive="jce_engine_core.a")
        self.write(data)
        with self.assertRaisesRegex(ValueError, "crt"):
            self.require(data)

    def test_archive_basename_is_fixed_and_cannot_escape_selected_config(self):
        for archive in ("../jce_engine_core.lib", "other.lib", "jce_engine_core.a",
                        "/tmp/jce_engine_core.lib", "C:\\outside\\jce_engine_core.lib", None):
            with self.subTest(archive=archive):
                self.write(self.metadata(archive=archive))
                with self.assertRaisesRegex(ValueError, "archive must|unescaped ASCII"):
                    self.require()

    def test_noncanonical_request_is_rejected_without_metadata(self):
        for field, value in (("system", "windows"), ("arch", "arm64"),
                              ("config", "release"), ("variant", "ship"),
                              ("delivery", None)):
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, field):
                self.require(**{field: value})
        with self.assertRaisesRegex(ValueError, "invalid config"):
            self.require(config="RelWithDebInfo")
        with self.assertRaisesRegex(ValueError, "invalid SDK root"):
            SDK_TARGET.require_sdk_target(None, "Windows", "x86_64", "Release", "dist", "embedded")


if __name__ == "__main__":
    unittest.main()
