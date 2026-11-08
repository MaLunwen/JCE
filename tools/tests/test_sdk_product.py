"""Real temporary SDK identity controls; no compiler, executable or GPU.

Archives/header/tools are byte fixtures, not native ABI or execution evidence.
Full-owner historical mutants fail ordinary unchanged contract assertions.
"""
from __future__ import annotations
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path: sys.path.insert(0, str(ROOT))
from tools.build import sdk_product as product
from tools.tests.test_sdk_outputs import Fixture as OutputFixture, STANDALONE, OPTIONS

VERSION = re.search(r"project\(JCE VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C\)",
    (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")).group(1)


def load_owner(name, path, source=None):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); sys.modules[name] = module
    if source is None: spec.loader.exec_module(module)
    else: exec(compile(source, str(path), "exec"), module.__dict__)
    return module


class Tree:
    def __init__(self, *, system="Windows", arch="x86_64", config="Release",
                 variant="dist", delivery="embedded"):
        base = ROOT / "build/tests/sdk-product"; base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="owned-", dir=base)
        self.root = Path(self.temp.name).resolve()
        self.sdk, self.build = self.root / "sdk", self.root / "build"
        self.request = dict(system=system, arch=arch, config=config, variant=variant,
            delivery=delivery, expected_version=VERSION)
        self.populate()

    def put(self, relative, content):
        path = self.sdk / relative; path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def populate(self):
        (self.root / "CMakeLists.txt").write_text("project(JCE VERSION " + VERSION + " LANGUAGES C)\n", encoding="utf-8")
        major, minor, patch_version = VERSION.split(".")
        self.put("include/jce/jce_version.h", (f"#define JCE_VERSION_MAJOR {major}\n"
            f"#define JCE_VERSION_MINOR {minor}\n#define JCE_VERSION_PATCH {patch_version}\n"
            f"#define JCE_VERSION_STR \"{VERSION}\"\n"
            "#define JCE_API_VERSION ((JCE_VERSION_MAJOR << 24) | (JCE_VERSION_MINOR << 16) | (JCE_VERSION_PATCH << 8))\n").encode())
        self.put("include/jce/api.h", b"uncompiled public header\n")
        for name in ("JCEConfig.cmake", "JCESDKTarget.cmake", "JCESDKHelpers.cmake"):
            self.put("lib/cmake/JCE/" + name, b"unexecuted CMake bytes\n")
        request = self.request
        windows = request["system"] == "Windows"
        archive = "jce_engine_core.lib" if windows else "jce_engine_core.a"
        self.library = self.put("lib/" + request["config"] + "/" + archive, b"unlinked library data; identity only")
        crt = ("MultiThreaded" if request["delivery"] == "embedded" else
               "MultiThreadedDebugDLL" if request["config"] == "Debug" else "MultiThreadedDLL") if windows else "none"
        target = dict(schema=1, system=request["system"], arch=request["arch"],
            pointer_bits=64 if request["arch"] in ("x86_64", "aarch64") else 32,
            config=request["config"], variant=request["variant"], delivery=request["delivery"], crt=crt, archive=archive)
        self.put("lib/" + request["config"] + "/jce-sdk-target.json", json.dumps(target).encode())
        self.put("delivery-profile.json", json.dumps(dict(schema=1, profile=request["delivery"],
            languages=["lua", "c", "cpp", "js"], external_vm_allowed=request["delivery"] == "authoring")).encode())
        self.put("VERSION.txt", b"byte fixture, no execution claim\n")
        self.tool = self.put("bin/host/jce_pak.exe", b"unexecuted host tool")
        self.asset = self.put("share/jce/sample.bin", bytes(range(251)) * 3)
        self.roster()

    def roster(self):
        self.build.mkdir(parents=True, exist_ok=True)
        files = sorted(str(p.resolve()) for p in self.sdk.rglob("*") if p.is_file() and p.name != product.PRODUCT_FILE)
        (self.build / "install_manifest.txt").write_text("\n".join(files) + "\n", encoding="utf-8")

    def close(self): self.temp.cleanup()
    def write(self, owner=product, **changes): return owner.write_product(self.sdk, **dict(self.request, **changes))
    def verify(self, owner=product, **changes): return owner.verify_product(self.sdk, **dict(self.request, **changes))


class SDKProductTests(unittest.TestCase):
    def tree(self, **selection):
        tree = Tree(**selection); self.addCleanup(tree.close); return tree

    def test_real_tree_hashes_all_files_excludes_self_and_keeps_truthful_flags(self):
        tree = self.tree()
        manifest = tree.write(required_paths=["bin/host/jce_pak.exe"])
        self.assertEqual(manifest, tree.verify(required_paths=["bin/host/jce_pak.exe"]))
        self.assertEqual(manifest["api_version"], (int(VERSION.split(".")[0]) << 24) |
            (int(VERSION.split(".")[1]) << 16) | (int(VERSION.split(".")[2]) << 8))
        rows = {row["path"]: row for row in manifest["files"]}
        self.assertNotIn(product.PRODUCT_FILE, rows)
        for address, row in rows.items():
            data = (tree.sdk / address).read_bytes()
            self.assertEqual(row["size"], len(data)); self.assertEqual(row["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(rows["include/jce/api.h"]["role"], "header")
        self.assertEqual(rows["lib/Release/jce_engine_core.lib"]["role"], "library")
        self.assertEqual(rows["bin/host/jce_pak.exe"]["role"], "host-tool-or-helper")
        for flag in ("build_payload_available", "compiled_abi_verified", "execution_verified"):
            self.assertIs(manifest[flag], False)

    def test_target_matrix_uses_exact_actual_metadata_and_not_directory_name(self):
        for system, arch, variant, config, delivery in (("Windows", "i686", "release", "Release", "authoring"),
                ("Windows", "x86_64", "debug", "Debug", "authoring"),
                ("Linux", "x86_64", "dist", "Release", "embedded"),
                ("Darwin", "aarch64", "release", "Release", "authoring"),
                ("Emscripten", "wasm32", "dist", "Release", "embedded")):
            with self.subTest(system=system, config=config):
                tree = self.tree(system=system, arch=arch, variant=variant, config=config, delivery=delivery)
                tree.write(); self.assertEqual(tree.verify()["sdk_target"]["system"], system)
                with self.assertRaises(product.ProductError): tree.verify(arch="aarch64" if arch != "aarch64" else "x86_64")

    def test_relocation_and_identical_republication_do_not_churn_identity(self):
        tree = self.tree(); manifest = tree.write()
        path = tree.sdk / product.PRODUCT_FILE; before = path.stat().st_mtime_ns
        with patch.object(product.os, "replace", wraps=os.replace) as replace:
            self.assertEqual(manifest, tree.write()); replace.assert_not_called()
        self.assertEqual(before, path.stat().st_mtime_ns)
        moved = tree.root / "moved SDK with spaces"; tree.sdk.rename(moved); tree.sdk = moved
        self.assertEqual(manifest, tree.verify())

    def test_same_size_library_tool_and_resource_tampering_is_rejected(self):
        for field in ("library", "tool", "asset"):
            with self.subTest(field=field):
                tree = self.tree(); tree.write(); path = getattr(tree, field)
                data = path.read_bytes(); path.write_bytes(bytes([data[0] ^ 1]) + data[1:])
                with self.assertRaises(product.ProductError): tree.verify()

    def test_missing_added_files_and_planned_install_inputs_are_rejected(self):
        tree = self.tree(); tree.write(); tree.tool.unlink()
        with self.assertRaises(product.ProductError): tree.verify()
        tree = self.tree(); tree.write(); tree.put("unplanned.bin", b"extra")
        with self.assertRaises(product.ProductError): tree.verify()
        tree = self.tree(); tree.roster(); tree.tool.unlink()
        with self.assertRaises(product.ProductError): product.install_required_paths(tree.build, tree.sdk)
        self.assertFalse((tree.sdk / product.PRODUCT_FILE).exists())

    def test_install_roster_binds_real_files_and_refuses_outside_relative_empty_directory(self):
        tree = self.tree(); required = product.install_required_paths(tree.build, tree.sdk)
        self.assertIn("include/jce/api.h", required)
        self.assertEqual(required, sorted(required))
        tree.write(required_paths=required); tree.verify(required_paths=required)
        roster = tree.build / "install_manifest.txt"
        outside = tree.root / "outside.bin"; outside.write_bytes(b"outside")
        for value in (str(outside), "relative/file", "", str(tree.sdk / "include")):
            with self.subTest(value=value):
                roster.write_text(value + "\n", encoding="utf-8")
                with self.assertRaises(product.ProductError): product.install_required_paths(tree.build, tree.sdk)

    def test_source_version_and_generated_version_api_are_bound(self):
        tree = self.tree(); self.assertEqual(VERSION, product.source_version(tree.root))
        with self.assertRaises(product.ProductError): tree.write(expected_version="99.99.99")
        version = tree.sdk / "include/jce/jce_version.h"
        version.write_text(version.read_text().replace("JCE_VERSION_MINOR << 16", "JCE_VERSION_MINOR << 15"))
        with self.assertRaises(product.ProductError): tree.write()
        self.assertFalse((tree.sdk / product.PRODUCT_FILE).exists())
        (tree.root / "CMakeLists.txt").write_text("project(OTHER VERSION 1.2.3 LANGUAGES C)\n")
        with self.assertRaises(product.ProductError): product.source_version(tree.root)

    def test_actual_generated_header_template_accepts_utf8_comments_and_continued_api(self):
        tree = self.tree()
        text = (ROOT / "engine/include/jce/jce_version.h.in").read_text(encoding="utf-8")
        major, minor, patch_version = VERSION.split(".")
        for token, value in (("PROJECT_VERSION_MAJOR", major), ("PROJECT_VERSION_MINOR", minor),
                             ("PROJECT_VERSION_PATCH", patch_version), ("PROJECT_VERSION", VERSION),
                             ("JCE_GIT_COMMIT", "byte-fixture"), ("JCE_BUILD_TIMESTAMP_UTC", "fixture UTC")):
            text = text.replace("@" + token + "@", value)
        self.assertNotIn("@PROJECT_VERSION", text)
        text = text.replace("\r\n", "\n")
        for header in (text, text.replace("\n", "\r\n")):
            with self.subTest(crlf="\r\n" in header):
                tree.put("include/jce/jce_version.h", header.encode("utf-8"))
                tree.write(); self.assertEqual(VERSION, tree.verify()["jce_version"])

    def test_metadata_types_false_claims_duplicate_keys_and_nonfinite_are_rejected(self):
        tree = self.tree(); manifest = tree.write(); path = tree.sdk / product.PRODUCT_FILE
        for flag in ("build_payload_available", "compiled_abi_verified", "execution_verified"):
            bad = copy.deepcopy(manifest); bad[flag] = True; path.write_text(json.dumps(bad))
            with self.subTest(flag=flag), self.assertRaises(product.ProductError): tree.verify()
        for field in ("sdk_target", "delivery_profile"):
            bad = copy.deepcopy(manifest); bad[field]["schema"] = True; path.write_text(json.dumps(bad))
            with self.subTest(field=field), self.assertRaises(product.ProductError): tree.verify()
        for raw in ('{"schema":1,"schema":1}', '{"schema":NaN}', '{broken'):
            path.write_text(raw)
            with self.subTest(raw=raw), self.assertRaises(product.ProductError): tree.verify()

    def test_noncanonical_self_duplicate_addresses_and_manifest_escape_fail_before_hashing(self):
        tree = self.tree()
        for paths in (("../outside",), ("/absolute",), ("a\\b",), ("a//b",), (product.PRODUCT_FILE,),
                      ("include/jce/api.h", "include/jce/api.h")):
            with self.subTest(paths=paths), self.assertRaises(product.ProductError): tree.write(required_paths=paths)
        manifest = tree.write(); manifest["files"][0]["path"] = "../outside"
        (tree.root / "outside").write_bytes(b"real outside witness")
        (tree.sdk / product.PRODUCT_FILE).write_text(json.dumps(manifest))
        with patch.object(product, "_describe", wraps=product._describe) as describe:
            with self.assertRaises(product.ProductError): tree.verify()
            describe.assert_not_called()

    def test_reparse_attribute_and_available_real_symlink_are_rejected(self):
        tree = self.tree(); original = Path.lstat
        class Reparse:
            st_file_attributes = 0x400
            def __init__(self, info): self.info = info
            def __getattr__(self, key): return getattr(self.info, key)
        def lstat(path, *args, **kwargs):
            info = original(path, *args, **kwargs)
            return Reparse(info) if path == tree.tool else info
        with patch.object(Path, "lstat", lstat), self.assertRaises(product.ProductError): tree.write()
        link = tree.sdk / "alias.bin"
        try: link.symlink_to(tree.asset)
        except OSError as error: self.skipTest("host cannot create real symlink: " + str(error))
        with self.assertRaises(product.ProductError): tree.write()

    def test_streaming_hash_and_observed_same_size_source_change_reject_publication(self):
        tree = self.tree(); sizes = []; original_open = Path.open
        class Reader:
            def __init__(self, stream): self.stream = stream
            def __enter__(self): self.stream.__enter__(); return self
            def __exit__(self, *args): return self.stream.__exit__(*args)
            def fileno(self): return self.stream.fileno()
            def read(self, size): sizes.append(size); return self.stream.read(size)
        def opened(path, *args, **kwargs):
            stream = original_open(path, *args, **kwargs)
            return Reader(stream) if path == tree.asset and args and args[0] == "rb" else stream
        with patch.object(product, "_CHUNK_BYTES", 7), patch.object(Path, "open", opened): tree.write()
        self.assertTrue(len(sizes) > 2); self.assertTrue(all(size == 7 for size in sizes))
        before = (tree.sdk / product.PRODUCT_FILE).read_bytes(); original_hash = product._hash_file
        changed = False
        def hash_then_change(path, *args):
            nonlocal changed
            result = original_hash(path, *args)
            if path == tree.library and not changed:
                changed = True; path.write_bytes(path.read_bytes() + b"grew")
            return result
        with patch.object(product, "_hash_file", hash_then_change), self.assertRaises(product.ProductError): tree.write()
        self.assertEqual(before, (tree.sdk / product.PRODUCT_FILE).read_bytes())
        with self.assertRaises(product.ProductError): tree.verify()

    def test_caps_and_atomic_replace_failure_preserve_old_manifest_and_clean_owned_temp(self):
        tree = self.tree(); tree.write(); path = tree.sdk / product.PRODUCT_FILE; before = path.read_bytes()
        for setting, limit in (("MAX_FILES", 1), ("MAX_DIRECTORIES", 1), ("MAX_MANIFEST_BYTES", 10), ("_METADATA_CAP", 10)):
            with self.subTest(setting=setting), patch.object(product, setting, limit), self.assertRaises(product.ProductError): tree.write()
        tree.asset.write_bytes(b"new asset")
        with patch.object(product.os, "replace", side_effect=OSError("atomic replace refused")), self.assertRaises(OSError): tree.write()
        self.assertEqual(before, path.read_bytes()); self.assertFalse(list(tree.sdk.glob(".jce-sdk-product-*.tmp")))
        with self.assertRaises(product.ProductError): tree.verify()

    def test_old_omitted_hash_and_address_guard_fail_ordinary_unchanged_contracts(self):
        source = (ROOT / "tools/build/sdk_product.py").read_text()
        self.assertEqual(1, source.count("digest.update(block)"))
        owner = load_owner("sdk_product_old_hash", ROOT / "tools/build/sdk_product.py",
            source.replace("digest.update(block)", "pass # Historical digest omission"))
        tree = self.tree(); tree.write(owner)
        data = tree.library.read_bytes(); tree.library.write_bytes(b"X" + data[1:])
        def hash_contract():
            try: tree.verify(owner)
            except product.ProductError: return
            except owner.ProductError: return
            raise AssertionError("same-size library tamper must be rejected")
        with self.assertRaisesRegex(AssertionError, "tamper"): hash_contract()
        start = source.index('    if (value.startswith("/")'); end = source.index('    return value', start)
        old = source[:start] + '    if False:\n        raise ProductError("old address admission")\n' + source[end:]
        owner = load_owner("sdk_product_old_address", ROOT / "tools/build/sdk_product.py", old)
        tree = self.tree(); manifest = tree.write(owner); manifest["files"][0]["path"] = "../outside"
        (tree.root / "outside").write_bytes(b"outside")
        (tree.sdk / product.PRODUCT_FILE).write_text(json.dumps(manifest))
        with patch.object(owner, "_describe", wraps=owner._describe) as describe:
            with self.assertRaises(owner.ProductError): tree.verify(owner)
            with self.assertRaises(AssertionError): describe.assert_not_called()

    def test_source_mutation_during_stream_and_new_inventory_entries_reject_publication(self):
        for mode in ("same-size-read", "late-file"):
            with self.subTest(mode=mode):
                tree = self.tree(); original_open = Path.open; original_hash = product._hash_file
                changed = False
                class Reader:
                    def __init__(self, stream): self.stream = stream
                    def __enter__(self): self.stream.__enter__(); return self
                    def __exit__(self, *args): return self.stream.__exit__(*args)
                    def fileno(self): return self.stream.fileno()
                    def read(self, size):
                        nonlocal changed
                        block = self.stream.read(size)
                        if block and not changed:
                            changed = True
                            with original_open(tree.asset, "rb") as witness: data = witness.read()
                            with original_open(tree.asset, "wb") as writer: writer.write(b"X" + data[1:])
                        return block
                def opened(path, *args, **kwargs):
                    stream = original_open(path, *args, **kwargs)
                    return Reader(stream) if mode == "same-size-read" and path == tree.asset and args and args[0] == "rb" else stream
                def after_hash(path, *args):
                    nonlocal changed
                    result = original_hash(path, *args)
                    if mode == "late-file" and path == tree.library and not changed:
                        changed = True; tree.put("late-added.bin", b"new input")
                    return result
                with patch.object(product, "_CHUNK_BYTES", 17), patch.object(Path, "open", opened), \
                     patch.object(product, "_hash_file", after_hash), self.assertRaises(product.ProductError): tree.write()
                self.assertTrue(changed); self.assertFalse((tree.sdk / product.PRODUCT_FILE).exists())

    def test_install_parent_alias_and_root_symlink_are_not_sdk_addresses(self):
        tree = self.tree(); outside = tree.root / "outside-directory"; outside.mkdir()
        (outside / "file.bin").write_bytes(b"outside")
        alias = tree.sdk / "directory-alias"
        try: alias.symlink_to(outside, target_is_directory=True)
        except OSError as error: self.skipTest("host cannot create real directory symlink: " + str(error))
        (tree.build / "install_manifest.txt").write_text(str(alias / "file.bin") + "\n")
        with self.assertRaises(product.ProductError): product.install_required_paths(tree.build, tree.sdk)
        root_alias = tree.root / "sdk-alias"; root_alias.symlink_to(tree.sdk, target_is_directory=True)
        with self.assertRaises(product.ProductError): product.write_product(root_alias, **tree.request)


class SDKDirectoryDiagnosticTests(unittest.TestCase):
    """Real owned directory mutations exercise the final canonical rejection guard."""
    FIELDS = ('st_dev', 'st_ino', 'st_mode', 'st_size', 'st_mtime_ns', 'st_ctime_ns', 'st_file_attributes')
    PREFIX = 'SDK directory changed during observation'

    def tree(self):
        t = Tree()
        self.addCleanup(t.close)
        self.assertTrue(t.sdk.resolve().is_relative_to((ROOT / 'build/tests').resolve()))
        return t

    def case(self, relative, *, all_fields=False):
        t = self.tree()
        targets = [t.sdk / part if part != '.' else t.sdk for part in relative]
        for d in targets:
            d.mkdir(parents=True, exist_ok=True)
        manifest = t.write(product)
        manifest_path = t.sdk / product.PRODUCT_FILE
        manifest_before = manifest_path.read_bytes()
        files, directories = product._inventory(t.sdk)
        before = {d: directories[d] for d in targets}
        after = {}
        bytes_before = {address: hashlib.sha256((t.sdk / address).read_bytes()).hexdigest() for address, snapshot in files}
        first = next((d for d in directories if d in targets))
        hash_original = product._hash_file
        stat_original = product._safe_stat
        changed = False

        class StatPort:

            def __init__(self, info, values):
                self.info = info
                self.values = values

            def __getattr__(self, key):
                return self.values[key] if key in self.values else getattr(self.info, key)

        def guarded_stat(p, *, directory=False):
            info = stat_original(p, directory=directory)
            if all_fields and changed and (p in after) and directory:
                return StatPort(info, dict(zip(self.FIELDS, after[p])))
            return info

        def during_hash(p, *args):
            nonlocal changed
            result = hash_original(p, *args)
            if p == t.asset and (not changed):
                changed = True
                for d in targets:
                    info = d.stat()
                    os.utime(d, ns=(info.st_atime_ns, info.st_mtime_ns + 2000000000))
                    after[d] = product._snapshot(stat_original(d, directory=True))
                    if all_fields:
                        v = before[d]
                        after[d] = ((1 << 60) + 3, (1 << 60) + 5, v[2] ^ 128, v[3] + 1, (1 << 61) + 7, (1 << 61) + 11, v[6] ^ 32)
            return result
        with patch.object(product, '_hash_file', during_hash), patch.object(product, '_safe_stat', guarded_stat):
            with self.assertRaises(product.ProductError) as caught:
                t.verify(product)
        self.assertIs(type(caught.exception), product.ProductError)
        self.assertTrue(changed)
        self.assertEqual(files, product._inventory(t.sdk)[0])
        self.assertEqual(bytes_before, {address: hashlib.sha256((t.sdk / address).read_bytes()).hexdigest() for address, snapshot in files})
        self.assertEqual(manifest_before, manifest_path.read_bytes())
        message = str(caught.exception)
        expected = {n: {'before': str(x), 'after': str(y)} for n, x, y in zip(self.FIELDS, before[first], after[first]) if x != y}
        self.assertTrue(message.startswith(self.PREFIX + ': '), 'required directory/stat witness absent; canonical rejection alone is insufficient')
        witness = json.loads(message[len(self.PREFIX) + 2:])
        self.assertEqual(set(witness), {'directory', 'changed'})
        self.assertEqual(witness['directory'], first.relative_to(t.sdk).as_posix())
        self.assertEqual(witness['changed'], expected)
        self.assertGreater(len(expected), 0)
        self.assertLessEqual(len(expected), 7)
        self.assertLessEqual(len(message.encode('utf-8')), 8192)
        self.assertNotIn(str(t.sdk), message)
        for change in witness['changed'].values():
            for value in change.values():
                self.assertIs(type(value), str)
                self.assertRegex(value, '^-?[0-9]+$')
        return witness

    def test_nested_directory_mtime_reports_exact_guard_witness(self):
        witness = self.case(('share/jce',))
        self.assertIn('st_mtime_ns', witness['changed'])

    def test_root_directory_uses_relative_dot(self):
        self.assertEqual(self.case(('.',))['directory'], '.')

    def test_only_first_changed_directory_is_reported(self):
        self.assertEqual(self.case(('share/jce', 'lib/Release'))['directory'], 'lib/Release')

    def test_all_seven_fields_retain_exact_large_decimal_strings(self):
        witness = self.case(('share/jce',), all_fields=True)
        self.assertEqual(set(witness['changed']), set(self.FIELDS))
        self.assertEqual(witness['changed']['st_ino']['after'], str((1 << 60) + 5))
        self.assertEqual(witness['changed']['st_mtime_ns']['after'], str((1 << 61) + 7))

    def test_unicode_relative_address_is_escaped_and_bounded(self):
        self.assertEqual(self.case(('share/jce/diag-\u03bb',))['directory'], 'share/jce/diag-\u03bb')

    def test_unchanged_tree_still_verifies_identical_product(self):
        t = self.tree()
        m = t.write(product)
        self.assertEqual(t.verify(product), m)


class SDKProducerIdentityTests(unittest.TestCase):
    def output_fixture(self):
        fixture = OutputFixture(); self.addCleanup(fixture.close); return fixture

    def test_actual_isolated_embedded_hook_publishes_once_after_complete_install(self):
        fixture = self.output_fixture(); original = fixture.driver._publish_sdk_product
        with fixture.producer_ports(), patch.object(fixture.driver, "_publish_sdk_product", wraps=original) as publish:
            fixture.driver.cmd_sdk(fixture.args)
        publish.assert_called_once()
        manifest = product.verify_product(fixture.plan["install"], system="Windows", arch="x86_64",
            config="Release", variant="dist", delivery="embedded", expected_version=VERSION)
        self.assertIn("bin/jce_bundle_pack.exe", manifest["required_paths"])
        self.assertIs(manifest["execution_verified"], False)

    def test_failed_install_and_identity_mismatch_do_not_publish_success(self):
        for fault in ("install", "identity"):
            with self.subTest(fault=fault):
                fixture = self.output_fixture(); original = fixture.capture
                def run(command, **kwargs):
                    if "--install" in command and fault == "install": raise RuntimeError("install failed")
                    original(command, **kwargs)
                    if "--install" in command and fault == "identity":
                        (fixture.plan["install"] / "include/jce/jce_version.h").write_text("wrong generated header")
                with fixture.producer_ports(run=run), self.assertRaises((RuntimeError, SystemExit)):
                    fixture.driver.cmd_sdk(fixture.args)
                self.assertFalse((fixture.plan["install"] / product.PRODUCT_FILE).exists())
                state = json.loads((fixture.plan["build"] / OPTIONS.REQUEST_FILE).read_text())["state"]
                self.assertEqual(state, "install_failed")

    def test_old_missing_embedded_publication_fails_ordinary_product_existence(self):
        fixture = self.output_fixture(); source = (ROOT / "tools/build/standalone.py").read_text()
        wire = 'driver._publish_sdk_product(sdk, target, "Release", args.variant, "embedded", build)'
        self.assertEqual(1, source.count(wire))
        owner = load_owner("sdk_standalone_missing_product", ROOT / "tools/build/standalone.py", source.replace(wire, "None # old omitted file identity publication"))
        with fixture.producer_ports(standalone=owner): fixture.driver.cmd_sdk(fixture.args)
        with self.assertRaises(AssertionError): self.assertTrue((fixture.plan["install"] / product.PRODUCT_FILE).is_file())

    def test_standard_native_and_web_completion_hooks_use_real_closed_file_trees(self):
        for web, config in ((False, "Release"), (False, "Debug"), (True, "Release")):
            with self.subTest(web=web, config=config):
                tree = Tree(system="Emscripten" if web else "Windows", arch="wasm32" if web else "x86_64",
                    variant="dist" if web else "debug" if config == "Debug" else "release",
                    config=config, delivery="embedded" if web else "authoring")
                self.addCleanup(tree.close)
                driver = load_owner("sdk_product_web_driver" if web else "sdk_product_native_driver", ROOT / "tools/build/jce.py")
                driver.ROOT = tree.root; driver.HOST = "windows"; driver.DRY_RUN = False
                target = dict(driver.TARGETS["wasm" if web else "windows-x64"], key="wasm" if web else "windows-x64", cross=False)
                tools = {}
                for name in ("jce_pak", "jce_cook", "jce_bin2obj", "jce_bundle_pack"):
                    path = tree.root / "host-tools" / (name + ".exe"); path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(b"unexecuted build-host tool bytes"); tools[name] = path
                install_base = tree.sdk
                if config == "Debug": tree.sdk = install_base.parent / (install_base.name + "-debug")
                actual_build = tree.root / "build/web/wasm-dist-sdk" if web else tree.build
                def run(command, **kwargs):
                    if "--install" in command:
                        tree.populate()
                        if not web:
                            for name, tool in tools.items(): tree.put("bin/" + tool.name, tool.read_bytes())
                        tree.roster()
                        actual_build.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(tree.build / "install_manifest.txt", actual_build / "install_manifest.txt") if actual_build != tree.build else None
                original = driver._publish_sdk_product
                with patch.object(driver, "_cache_scope", return_value=contextlib.nullcontext()), \
                     patch.object(driver, "preset_name", return_value="presence-fixture"), \
                     patch.object(driver, "preset_binary_dir", return_value=tree.build), \
                     patch.object(driver, "sdk_install_dir", return_value=install_base), \
                     patch.object(driver, "msvc_env", return_value={}), patch.object(driver, "conan_install"), \
                     patch.object(driver, "toolchain_path", return_value=tree.root / "toolchain.cmake"), \
                     patch.object(driver, "ensure_host_tools", return_value=tools), \
                     patch.object(driver, "find_host_shaderc", return_value=tree.tool), \
                     patch.object(driver, "_tool_runs", return_value=True), \
                     patch.object(driver, "git_short_sha", return_value="byte-fixture"), \
                     patch.object(driver, "sdk_shipped_script_backends", return_value=["c", "cpp", "js"]), \
                     patch.object(driver, "run", side_effect=run), patch.object(driver, "build_jobs", return_value="2"), \
                     patch.object(driver, "_publish_sdk_product", wraps=original) as publish, \
                     contextlib.redirect_stdout(io.StringIO()):
                    driver._sdk_one(target, "dist" if web else "release", config, False, delivery_profile=tree.request["delivery"])
                publish.assert_called_once(); tree.verify()

    def test_missing_host_tools_require_explicit_degraded_policy_for_native_and_web(self):
        for web in (False, True):
            with self.subTest(web=web):
                tree = Tree(system="Emscripten" if web else "Windows", arch="wasm32" if web else "x86_64")
                self.addCleanup(tree.close)
                driver = load_owner("sdk_product_tool_policy_web" if web else "sdk_product_tool_policy_native", ROOT / "tools/build/jce.py")
                driver.ROOT = tree.root; driver.HOST = "windows"; driver.DRY_RUN = False
                target = dict(driver.TARGETS["wasm" if web else "windows-x64"], cross=False)
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit):
                        driver._publish_sdk_product(tree.sdk, target, "Release", "dist", "embedded", tree.build)
                    self.assertFalse((tree.sdk / product.PRODUCT_FILE).exists())
                    observed = driver._publish_sdk_product(tree.sdk, target, "Release", "dist", "embedded", tree.build, require_tools=False)
                self.assertIs(observed["build_payload_available"], False)
                self.assertEqual(tree.verify()["sdk_target"]["system"], tree.request["system"])

    def test_dry_run_product_bridge_does_not_touch_files_or_claim_identity(self):
        tree = Tree(); self.addCleanup(tree.close)
        driver = load_owner("sdk_product_dry_driver", ROOT / "tools/build/jce.py")
        driver.ROOT = tree.root; driver.DRY_RUN = True
        self.assertIsNone(driver._publish_sdk_product(tree.sdk, {}, "Release", "dist", "embedded", tree.build))
        self.assertFalse((tree.sdk / product.PRODUCT_FILE).exists())


if __name__ == "__main__": unittest.main()
