"""Compilerless seal owner controls; never compile or execute an application.

The actual delivery inspector/admission owner consumes fresh JSON from a counted
native-tool port. PE imports/resource reads are typed ports, not a native witness.
--mutant selects an old ordinary implementation; unchanged tests must fail.
"""
from __future__ import annotations
import copy
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from tools.build import stock_app as stock
from tools.build import stock_runtime as producer

CUSTOM_TARGET_OLD = "--old-custom-target" in sys.argv
if CUSTOM_TARGET_OLD:
    sys.argv.remove("--old-custom-target")

if "--mutant" in sys.argv:
    index = sys.argv.index("--mutant")
    name = sys.argv[index + 1]
    del sys.argv[index:index + 2]
    source = Path(stock.__file__).read_text(encoding="utf-8")
    replacements = {
        "old-unchecked-payload": (
            "if fnv64(host) != host_hash or fnv64(bundle) != bundle_hash:",
            "if fnv64(host) != host_hash:"),
        "old-native-admission": (
            'languages = delivery.admit({}, [asset["path"] for asset in plan["assets"]],\n'
            '        "embedded", available=("lua", "js"), typed_assets=plan["assets"])',
            'languages = []'),
    }
    if name == "old-secure-report":
        start = source.index("def _secure_report(")
        stop = source.index("\ndef _secure_project(", start)
        replacements[name] = (source[start:stop],
            "def _secure_report(path, archive, inner_size):\n"
            "    return _json(_read(path, 16 * 1024 * 1024))\n\n")
    replacements["old-source-parity"] = (
        'packed.get("sources_verified") is not True', 'False')
    replacements["old-key-path"] = (
        'if _sha(_key_bytes(_read(runtime.key_file, 128))) != runtime.manifest["protection"]["key_sha256"]:',
        'if (runtime.key_file != Path(runtime.manifest["producer"]["sealing_key_path"]) or\n'
        '            _sha(_key_bytes(_read(runtime.key_file, 128))) != runtime.manifest["protection"]["key_sha256"]):')
    if name not in replacements:
        raise SystemExit("unknown stock mutant")
    old, new = replacements[name]
    if source.count(old) != 1:
        raise SystemExit("stock mutant source anchor changed")
    candidate = types.ModuleType("jce_stock_app_ordinary_mutant")
    candidate.__file__ = stock.__file__
    exec(compile(source.replace(old, new), stock.__file__, "exec"), candidate.__dict__)
    stock = candidate


def sha(data):
    return hashlib.sha256(data).hexdigest()


def host_bytes():
    blob = bytearray(512)
    blob[:2] = b"MZ"
    struct.pack_into("<I", blob, 60, 64)
    blob[64:68] = b"PE\0\0"
    struct.pack_into("<H", blob, 68, 0x8664)
    struct.pack_into("<H", blob, 84, 240)
    struct.pack_into("<H", blob, 88, 0x20b)
    struct.pack_into("<H", blob, 156, 2)
    return bytes(blob)


def asset(path, kind, representation, identity):
    return {"path": path, "size": 3, "asset_id": "%016x" % identity,
            "content_id": "%016x" % (identity + 10), "type": kind,
            "representation": representation, "dependencies": []}


class Tree:
    def __init__(self):
        self.temp = tempfile.TemporaryDirectory(prefix="jce-stock-control-")
        self.root = Path(self.temp.name).resolve()
        self.runtime = self.root / "runtime"
        self.runtime.mkdir()
        self.host = host_bytes()
        self.inspector = host_bytes() + b"inspector-port"
        self.notice = {"LICENSE": b"JCE test license", "THIRD_PARTY_LICENSES.md": b"test third-party notices"}
        self.key_path = self.root / "producer-cache/.jce/pak_key.hex"
        self.key_path.parent.mkdir(parents=True)
        self.key_path.write_text("00" * 32)
        self.key_sha = sha(bytes(32))
        self.manifest = {"schema": 2, "scope": "compilerless-lua-js-stock-runtime",
            "target": dict(stock._TARGET), "languages": ["lua", "js"],
            "sdk": {"product_sha256": "a" * 64, "jce_version": "0.13.4"},
            "producer": {"receipt_sha256": "b" * 64, "compile_input_sha256": "c" * 64, "output_sha256": sha(self.host),
                         "sealing_key_sha256": self.key_sha, "sealing_key_path": str(self.key_path)},
            "protection": dict(stock._PROTECTION, key_sha256=self.key_sha),
            "files": {"runtime": stock._row("stock.exe", self.host),
                      "inspector": stock._row("jce_pak.exe", self.inspector)},
            "notices": {name: stock._row(name, value) for name, value in self.notice.items()}}
        (self.runtime / "stock.exe").write_bytes(self.host)
        (self.runtime / "jce_pak.exe").write_bytes(self.inspector)
        for name, value in self.notice.items():
            (self.runtime / name).write_bytes(value)
        self.manifest_path = self.runtime / stock.MANIFEST_NAME
        self.save_manifest()
        self.bundle = self.root / "scene.jbundle"
        self.bundle.write_bytes(b"counted-public-inspector-payload")
        self.assets = [asset("scenes/main.scene.json", "scene", "scene.json", 1),
                       asset("scripts/main.lua", "script", "lua.source", 2)]
        self.assets[0]["dependencies"] = [{"address": self.assets[1]["path"], "asset_id": self.assets[1]["asset_id"]}]
        self.mutate_report = None
        self.mutate_outer = None
        self.outer_bytes = None
        self.after_inspection = None
        self.commands = []
        self.patches = [
            patch.object(stock.standalone, "verify", side_effect=self.imports),
            patch.object(stock.standalone, "verify_notices", side_effect=self.notices),
            patch.object(stock._InspectionDriver, "run", side_effect=self.inspect),
        ]
        for item in self.patches:
            item.start()

    def close(self):
        for item in reversed(self.patches):
            item.stop()
        self.temp.cleanup()

    def save_manifest(self):
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def imports(self, path):
        return {"imports": ["kernel32.dll"]}

    def notices(self, exe, root):
        return {name: sha(value) for name, value in self.notice.items()}

    def inspect(self, command, cwd):
        self.commands.append((list(command), Path(cwd)))
        if "--resource-dir" in command or ("--inspect" in command and
                Path(command[command.index("--inspect") + 1]).suffix == ".pak"):
            return self.secure_tool(command, cwd)
        if command[0] != str(self.runtime / "jce_pak.exe") or command[1] != "--inspect" or command[-2:] != ["--bundle-target", "windows"]:
            raise AssertionError("seal bypassed the public installed inspector")
        path = Path(command[command.index("--inspect") + 1])
        count = len(self.assets) + 1
        report = {"schema": "jce.pakbom.v1", "file": str(path), "file_size": path.stat().st_size,
            "header": {"header_verified": True},
            "totals": {"entries": count, "verify_ran": True, "verified_count": count,
                       "corrupt_count": 0, "skipped_count": 0},
            "bundle": {"verified": True, "content_id": "1" * 16,
                "sidecar": {"present": False, "verified": False, "compatible": False},
                "manifest": {"contract": {"name": "jce.bundle", "major": 1, "minor": 1},
                    "id": "scene", "target_profile": "windows", "scene_path": self.assets[0]["path"],
                    "depends_on": [], "assets": copy.deepcopy(self.assets)}}}
        if self.mutate_report:
            self.mutate_report(report)
        output = Path(command[command.index("--json") + 1])
        output.write_text(json.dumps(report), encoding="utf-8")
        if self.after_inspection:
            self.after_inspection()

    def secure_tool(self, command, cwd):
        if command[0] != str(self.runtime / "jce_pak.exe") or Path(cwd) != stock.ROOT:
            raise AssertionError("secure pack escaped the bound public tool owner")
        packing = "--resource-dir" in command
        key_flag = "--encrypt-key-file" if packing else "--key-file"
        if command[command.index(key_flag) + 1] != str(self.key_path):
            raise AssertionError("secure pack used an unbound key")
        if packing:
            stage = Path(command[command.index("--resource-dir") + 1])
            inner = stage / stock.BUNDLE_ENTRY
            if [path for path in stage.rglob("*") if path.is_file()] != [inner]:
                raise AssertionError("secure stage is not the exact single Bundle input")
            self.staged = inner.read_bytes()
            path = Path(command[command.index("--pak-file") + 1])
            # Opaque counted native-tool output; not a Python PAK/crypto encoder.
            self.outer_bytes = b"counted-secure-archive-port:" + sha(self.staged).encode()
            path.write_bytes(self.outer_bytes)
        else:
            path = Path(command[command.index("--inspect") + 1])
            inner = path.parent / "secure-stage" / stock.BUNDLE_ENTRY
            if "--verify" not in command:
                raise AssertionError("secure archive omitted fresh deep verification")
        report = {"schema": "jce.pakbom.v1", "file": str(path), "file_size": path.stat().st_size,
            "sources_verified": packing,
            "header": {key: True for key in
                ("header_verified", "encrypted", "secure_index", "authenticated", "auth_verified")},
            "totals": {"entries": 1, "verify_ran": True, "verified_count": 1, "corrupt_count": 0,
                       "skipped_count": 0, "encrypted_count": 1, "authenticated_count": 1},
            "entries": [{"path": stock.BUNDLE_ENTRY if packing else None,
                "path_hash": "0x0123456789abcdef", "original_size": len(self.staged),
                "encrypted": True, "authenticated": True, "verified": True}],
            "sources": [{"path": stock.BUNDLE_ENTRY, "source": str(inner), "selected": False,
                         "size": len(self.staged)}] if packing else []}
        report["header"].update(entry_count=1, dict_count=0, index_original_size=32)
        if self.mutate_outer:
            self.mutate_outer(report, packing)
        Path(command[command.index("--json") + 1]).write_text(json.dumps(report))

    def output(self, name="game.exe"):
        return self.root / "final" / Path(name).stem / name

    def receipt(self, name="game.exe"):
        return self.root / "evidence" / (name + ".json")

    def seal(self, name="game.exe"):
        return stock.seal(runtime_manifest=self.manifest_path, bundle=self.bundle, out=self.output(name),
            work_dir=self.root / "work" / name, receipt=self.receipt(name), pak_key_file=self.key_path)


class EnvelopeControls(unittest.TestCase):
    def test_exact_layout_roundtrip_and_borrowed_bytes(self):
        host, bundle = host_bytes(), b"exact Bundle bytes"
        footer = stock.footer_for(host, bundle)
        self.assertEqual(len(footer), 64)
        self.assertEqual(footer[:16], b"JCE_STOCK_APP_V2")
        self.assertEqual(struct.unpack_from("<IIQQ", footer, 16), (2, 64, len(host), len(bundle)))
        self.assertEqual(struct.unpack_from("<QQQ", footer, 40),
            (stock.fnv64(host), stock.fnv64(bundle), stock.fnv64(footer[:56])))
        self.assertEqual(stock.inspect_envelope(host + bundle + footer), (host, bundle))
        self.assertEqual(stock.fnv64(b"hello"), 0xa430d84680aabd0b)

    def test_corruption_at_host_payload_footer_and_trailing_bytes_fails(self):
        host, bundle = host_bytes(), b"payload"
        image = host + bundle + stock.footer_for(host, bundle)
        for position in (10, len(host), len(image) - 64, len(image) - 40, len(image) - 1):
            with self.subTest(position=position):
                changed = bytearray(image)
                changed[position] ^= 1
                with self.assertRaises(ValueError):
                    stock.inspect_envelope(changed)
        for changed in (image[:-1], image + b"x", b"", image[:63]):
            with self.assertRaises(ValueError):
                stock.inspect_envelope(changed)

    def test_unsigned_sizes_version_and_nested_seal_fail_closed(self):
        host, bundle = host_bytes(), b"payload"
        footer = stock.footer_for(host, bundle)
        for offset, value, fmt in ((16, 1, "I"), (20, 63, "I"), (24, 2**64 - 1, "Q"), (32, 0, "Q")):
            changed = bytearray(footer)
            struct.pack_into("<" + fmt, changed, offset, value)
            struct.pack_into("<Q", changed, 56, stock.fnv64(changed[:56]))
            with self.assertRaises(ValueError):
                stock.inspect_envelope(host + bundle + changed)
        with self.assertRaises(ValueError):
            stock.footer_for(host + bundle + footer, b"nested")

    def test_raw_v1_is_refused_even_with_consistent_footer_checksums(self):
        host, raw = host_bytes(), b"old unprotected Bundle"
        footer = bytearray(stock.footer_for(host, raw))
        footer[:16] = b"JCE_STOCK_APP_V1"
        struct.pack_into("<I", footer, 16, 1)
        struct.pack_into("<Q", footer, 56, stock.fnv64(footer[:56]))
        with self.assertRaises(ValueError):
            stock.inspect_envelope(host + raw + footer)
        with self.assertRaises(ValueError):
            stock.footer_for(host + raw + footer, b"nested")

    def test_pe_architecture_subsystem_and_truncation_are_admitted(self):
        for offset, value in ((68, 0x14c), (88, 0x10b), (156, 3), (84, 65535)):
            changed = bytearray(host_bytes())
            struct.pack_into("<H", changed, offset, value)
            with self.assertRaises(ValueError):
                stock.footer_for(changed, b"payload")
        with self.assertRaises(ValueError):
            stock.footer_for(host_bytes()[:100], b"payload")


class ProducerDriver:
    HOST = "windows"
    DRY_RUN = False
    def __init__(self, tree):
        self.tree = tree
        self.ROOT = tree.root / "producer-root"
        (self.ROOT / "tools/runtime").mkdir(parents=True)
        (self.ROOT / "tools/build").mkdir(parents=True)
        for source in (ROOT / "tools/runtime").rglob("*"):
            if source.is_file():
                destination = self.ROOT / "tools/runtime" / source.relative_to(ROOT / "tools/runtime")
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, destination)
        for name in ("stock_runtime.py", "stock_app.py"):
            shutil.copyfile(ROOT / "tools/build" / name, self.ROOT / "tools/build" / name)
        (self.ROOT / "LICENSE").write_bytes(tree.notice["LICENSE"])
        self.sdk = tree.root / "producer-sdk"
        (self.sdk / "bin").mkdir(parents=True)
        (self.sdk / "share/jce/engine_ui").mkdir(parents=True)
        (self.sdk / "share/jce/THIRD_PARTY_LICENSES.md").write_bytes(tree.notice["THIRD_PARTY_LICENSES.md"])
        (self.sdk / stock.sdk_product.PRODUCT_FILE).write_text('{"typed_sdk_identity_port":true}')
        self.tools = {name: self.sdk / ("bin/" + name + ".exe") for name in ("jce_pak", "jce_bin2obj")}
        for path in self.tools.values():
            path.write_bytes(b"counted SDK executable port")
        self.commands = []
        self.held = False
        self.resources = None
        self.fail_configure = False
        self.omit_log = False
        self.change_source = False

    def resolve_target(self, name):
        if name != "x64":
            raise AssertionError("producer invented target selection")
        return {"host": "windows", "arch": "x86_64"}

    @contextmanager
    def _cache_scope(self, resources):
        if self.held:
            raise AssertionError("unexpected nested cache acquisition")
        self.held = True
        self.resources = resources
        try:
            yield
        finally:
            self.held = False

    def _sdk_host_tools(self, sdk, target):
        return self.tools

    def msvc_env(self, target):
        return {"CMAKE_BUILD_PARALLEL_LEVEL": "9", "JCE_BGFX_CPU_PROFILER": "on"}

    def build_jobs(self, env):
        return env["CMAKE_BUILD_PARALLEL_LEVEL"]

    def _project_pak_key(self, directory):
        path = directory / ".jce/pak_key.hex"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("0" * 64)
        return bytes(32)

    def _write_pak_key_shares_c(self, path, key):
        path.write_text("/* counted shared key-source owner port */\n")

    def run(self, command, *, env, cwd):
        if not self.held or Path(cwd) != self.ROOT:
            raise AssertionError("producer command escaped shared ownership/root")
        if env["JCE_BGFX_CPU_PROFILER"] != "on":
            raise AssertionError("producer replaced caller toolchain policy")
        self.commands.append(list(command))
        directory = self.ROOT / producer.CACHE
        if command[0] == str(self.tools["jce_pak"]) and "--inspect" in command:
            path = Path(command[command.index("--inspect") + 1])
            report = json.loads((directory / "stock-generated/stock_assets.bom.json").read_text())
            report["sources_verified"] = False
            Path(command[command.index("--json") + 1]).write_text(json.dumps(report))
        elif command[0] == str(self.tools["jce_pak"]):
            path = Path(command[command.index("--pak-file") + 1])
            path.write_bytes(b"verified-stock-archive-port")
            report = {"schema": "jce.pakbom.v1", "file": str(path), "file_size": path.stat().st_size,
                "sources_verified": True, "header": {key: True for key in
                    ("header_verified", "encrypted", "secure_index", "authenticated", "auth_verified")},
                "totals": {"entries": 2, "verify_ran": True, "verified_count": 2, "corrupt_count": 0,
                    "skipped_count": 0, "encrypted_count": 2, "authenticated_count": 2},
                "selected_sources": [], "sources": [
                    {"path": "licenses/license", "selected": False},
                    {"path": "licenses/third_party_licenses.md", "selected": False}]}
            Path(command[command.index("--json") + 1]).write_text(json.dumps(report))
        elif command[0] == str(self.tools["jce_bin2obj"]):
            Path(command[command.index("--output") + 1]).write_bytes(b"counted COFF wrapper port")
        elif command[:2] == ["cmake", "-S"]:
            if self.fail_configure:
                raise subprocess.CalledProcessError(1, command)
        elif command[:2] == ["cmake", "--build"]:
            archive = (directory / "stock-generated/stock_assets.pak").read_bytes()
            (directory / "jce_stock_runtime.exe").write_bytes(host_bytes() + archive)
            if not self.omit_log:
                log = directory / "Testing/Temporary/LastTest.log"
                log.parent.mkdir(parents=True, exist_ok=True)
                log.write_text("counted native CTest log port; no compiler or native test executed\n")
            if self.change_source:
                path = self.ROOT / "tools/runtime/main.c"
                path.write_bytes(path.read_bytes() + b"\n/* concurrent source change */\n")
        else:
            raise AssertionError("producer invoked an unrelated recipe/tool: " + repr(command))


class ProducerControls(unittest.TestCase):
    def setUp(self):
        self.tree = Tree()
        self.addCleanup(self.tree.close)
        self.driver = ProducerDriver(self.tree)
        self.sdk_checks = []
        def verify(driver, sdk):
            self.assertTrue(driver.held)
            self.sdk_checks.append(sdk)
        self.verify = patch.object(producer, "_verify_sdk", side_effect=verify)
        self.verify.start()
        self.addCleanup(self.verify.stop)

    def run_owner(self, jobs=2):
        return producer.build(self.driver, types.SimpleNamespace(sdk=str(self.driver.sdk), jobs=jobs))

    def test_actual_producer_plan_uses_one_cache_sdk_tools_and_cpu_target(self):
        result = self.run_owner()
        self.assertEqual(result["status"], "STOCK_COMPONENT_BUILT_CPU_CONTROLS_PASSED_NOT_AN_APPLICATION")
        self.assertEqual(result["returncode"], 0)
        self.assertFalse(result["runnable_application"])
        self.assertFalse(result["runtime_execution_verified"])
        self.assertEqual(len(self.sdk_checks), 2)
        self.assertEqual(self.driver.resources, [self.driver.ROOT / producer.CACHE, self.driver.sdk])
        self.assertEqual(len(self.driver.commands), 4)
        pack, embed, configure, build = self.driver.commands
        self.assertNotIn("--selected-archive", pack)
        self.assertIn("--encrypt-key-file", pack)
        self.assertEqual([pack[index + 1] for index, value in enumerate(pack) if value == "--exclude-segment"],
                         ["AGENTS.md", "agents.md"])
        self.assertEqual(embed[-4:], ["--format", "coff", "--arch", "x64"])
        self.assertTrue(any(str(value).startswith("-DJCE_PROJECT_PREBUILT_ASSETS_OBJ=") for value in configure))
        self.assertEqual(build[-4:], ["--target", producer.TARGET, "--parallel", "2"])
        self.assertEqual(result["inputs_before"], result["inputs_after"])
        self.assertEqual(result["sealing_key"]["sha256"], sha(bytes(32)))
        self.assertTrue(Path(result["native_cpu_log"]["path"]).is_file())
        receipt_raw = Path(result["receipt_path"]).read_bytes()
        self.assertEqual(result["receipt_sha256"], sha(receipt_raw))
        self.assertNotIn("jce/runtime_boot.json", (self.driver.ROOT / producer.CACHE / "stock-generated/stock_assets.bom.json").read_text())

    def test_same_inputs_reuse_exact_assets_key_and_real_compile_fingerprint(self):
        first = self.run_owner()
        directory = self.driver.ROOT / producer.CACHE / "stock-generated"
        paths = [directory / name for name in ("stock_assets.pak", "stock_assets.obj", "stock_pak_key.c")]
        before = [(path.read_bytes(), path.stat().st_mtime_ns) for path in paths]
        old_count = len(self.driver.commands)
        second = self.run_owner()
        self.assertTrue(second["assets_reused"])
        self.assertEqual(first["compile_input_sha256"], second["compile_input_sha256"])
        self.assertEqual(before, [(path.read_bytes(), path.stat().st_mtime_ns) for path in paths])
        self.assertEqual(len(self.driver.commands) - old_count, 3)
        self.assertIn("--inspect", self.driver.commands[old_count])
        self.assertIn("--verify", self.driver.commands[old_count])

    def test_changed_sdk_or_source_enters_the_actual_compile_command(self):
        first = self.run_owner()
        product = self.driver.sdk / stock.sdk_product.PRODUCT_FILE
        product.write_text('{"typed_sdk_identity_port":2}')
        second = self.run_owner()
        self.assertNotEqual(first["compile_input_sha256"], second["compile_input_sha256"])
        self.assertFalse(second["assets_reused"])
        source = self.driver.ROOT / "tools/runtime/main.c"
        source.write_bytes(source.read_bytes() + b"\n/* next source revision */\n")
        third = self.run_owner()
        self.assertNotEqual(second["compile_input_sha256"], third["compile_input_sha256"])
        self.assertTrue(third["assets_reused"])
        self.assertIn("-DJCE_STOCK_INPUT_SHA256=" + third["compile_input_sha256"], third["commands"][-2])

    def test_producer_preserves_failed_configure_receipt_and_does_not_build(self):
        self.driver.fail_configure = True
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_owner()
        receipts = list((self.driver.ROOT / producer.CACHE / "receipts").glob("*/receipt.json"))
        self.assertEqual(len(receipts), 1)
        self.assertEqual(json.loads(receipts[0].read_text())["status"], "FAILED")
        self.assertEqual(len(self.driver.commands), 3)

    def test_producer_missing_fresh_cpu_log_is_not_accepted(self):
        self.driver.omit_log = True
        with self.assertRaises(ValueError):
            self.run_owner()

    def test_producer_actual_source_change_fails_after_counted_build(self):
        self.driver.change_source = True
        with self.assertRaises(ValueError):
            self.run_owner()

    def test_missing_sdk_tool_never_rebuilds_a_fallback(self):
        self.driver.tools.pop("jce_bin2obj")
        with self.assertRaises(ValueError):
            self.run_owner()
        self.assertFalse(self.driver.commands)

    def test_producer_dry_run_and_unsupported_host_do_not_create_evidence(self):
        self.driver.DRY_RUN = True
        with self.assertRaises(ValueError):
            self.run_owner()
        self.assertFalse((self.driver.ROOT / producer.CACHE).exists())
        self.driver.DRY_RUN = False
        self.driver.HOST = "linux"
        with self.assertRaises(ValueError):
            self.run_owner()
        self.assertFalse(self.driver.commands)


class NativeGenerationControls(unittest.TestCase):
    def test_actual_custom_target_is_parseable_ninja_without_a_compiler(self):
        cmake, ninja = shutil.which("cmake"), shutil.which("ninja")
        if not cmake or not ninja:
            self.skipTest("actual CMake/Ninja generation port unavailable; native retry remains required")
        source = (ROOT / "tools/runtime/CMakeLists.txt").read_text(encoding="utf-8")
        start = source.index("add_custom_target(jce_stock_runtime_verify\n")
        owner = source[start:]
        self.assertEqual(owner.count("add_custom_target("), 1)
        if CUSTOM_TARGET_OLD:
            self.assertEqual(owner.count("    VERBATIM"), 1)
            owner = owner.replace("    VERBATIM", "")
        # Only dependencies are empty ports; the complete production command,
        # quoting, regex and options are retained in this real generated graph.
        dependencies = owner.split("    DEPENDS ", 1)[1].split("    WORKING_DIRECTORY", 1)[0].split()
        preamble = "cmake_minimum_required(VERSION 3.20)\nproject(StockTargetGeneration LANGUAGES NONE)\n"
        preamble += "".join("add_custom_target(" + name + ")\n" for name in dependencies)
        with tempfile.TemporaryDirectory(prefix="jce-stock-ninja-port-") as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text(preamble + owner, encoding="utf-8")
            build = root / "generated"
            generated = subprocess.run([cmake, "-S", str(root), "-B", str(build), "-G", "Ninja"],
                text=True, capture_output=True, timeout=30)
            self.assertEqual(generated.returncode, 0, generated.stdout + generated.stderr)
            parsed = subprocess.run([ninja, "-C", str(build), "-n", "jce_stock_runtime_verify"],
                text=True, capture_output=True, timeout=30)
            self.assertEqual(parsed.returncode, 0, parsed.stdout + parsed.stderr)
            self.assertIn("ctest", parsed.stdout.lower())
            self.assertIn("policy_old_scene)$", parsed.stdout)


class RealCliControls(unittest.TestCase):
    def test_shared_cli_help_from_an_unrelated_cwd_without_pythonpath(self):
        environment = os.environ.copy()
        environment.pop("PYTHONPATH", None)
        with tempfile.TemporaryDirectory(prefix="jce-stock-cli-") as directory:
            result = subprocess.run([sys.executable, "-B", str(ROOT / "scripts/jce.py"),
                "stock-app", "--help"], cwd=directory, env=environment,
                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("build-runtime", result.stdout)
        self.assertIn("publish-runtime", result.stdout)
        self.assertIn("seal", result.stdout)

    def test_shared_cli_bad_seal_input_fails_before_native_tool_or_output(self):
        environment = os.environ.copy()
        environment.pop("PYTHONPATH", None)
        with tempfile.TemporaryDirectory(prefix="jce-stock-cli-bad-") as directory:
            root = Path(directory)
            result = subprocess.run([sys.executable, "-B", str(ROOT / "scripts/jce.py"),
                "stock-app", "seal", "--runtime-manifest", str(root / "missing.json"),
                "--bundle", str(root / "missing.jbundle"), "--out", str(root / "final/must-not-exist.exe"),
                "--work-dir", str(root / "work"), "--receipt", str(root / "evidence/receipt.json")],
                cwd=directory, env=environment, text=True, capture_output=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((root / "final/must-not-exist.exe").exists())
            self.assertFalse((root / "evidence/receipt.json").exists())
            self.assertNotIn("Traceback", result.stdout + result.stderr)


class OwnerControls(unittest.TestCase):
    def setUp(self):
        self.tree = Tree()
        self.addCleanup(self.tree.close)

    def test_actual_owner_seals_exact_input_and_keeps_public_inspection(self):
        tree = self.tree
        before = tree.bundle.read_bytes()
        receipt = tree.seal()
        self.assertEqual(receipt["status"], "SEALED_NOT_EXECUTED")
        self.assertFalse(receipt["runtime_execution_verified"])
        self.assertFalse(receipt["relocation_verified"])
        self.assertEqual(receipt["languages"], ["lua"])
        self.assertEqual(len(tree.commands), 3)
        self.assertIn("--verify", tree.commands[0][0])
        produced = Path(receipt["output"]["path"]).read_bytes()
        self.assertEqual(stock.inspect_envelope(produced), (tree.host, tree.outer_bytes))
        self.assertEqual(tree.staged, before)
        self.assertEqual(receipt["project_protection"]["inner_sha256"], sha(before))
        self.assertEqual(receipt["project_protection"]["archive_sha256"], sha(tree.outer_bytes))
        self.assertEqual(receipt["envelope"]["version"], 2)
        self.assertTrue(receipt["project_protection"]["authenticated"])
        self.assertNotIn(tree.key_path.read_text(), json.dumps(receipt))
        self.assertEqual(tree.bundle.read_bytes(), before)
        self.assertEqual((tree.runtime / "stock.exe").read_bytes(), tree.host)
        self.assertEqual(receipt["output"]["sha256"], sha(produced))
        self.assertEqual(list(tree.output().parent.iterdir()), [tree.output()])
        self.assertEqual(json.loads(tree.receipt().read_text()), receipt)
        self.assertTrue(Path(receipt["bundle"]["inspection_path"]).is_file())
        with self.assertRaises(ValueError):
            tree.seal()

    def test_final_directory_rejects_colocated_evidence_and_extra_files(self):
        tree = self.tree
        for name in ("work", "receipt"):
            kwargs = {"runtime_manifest": tree.manifest_path, "bundle": tree.bundle,
                "out": tree.output(), "work_dir": tree.root / "separate-work",
                "receipt": tree.root / "separate-receipt.json", "pak_key_file": tree.key_path}
            kwargs["work_dir" if name == "work" else "receipt"] = tree.output().parent / name
            with self.assertRaises(ValueError):
                stock.seal(**kwargs)
        tree.output().parent.mkdir(parents=True)
        (tree.output().parent / "unrelated.txt").write_text("preserve")
        with self.assertRaises(ValueError):
            tree.seal()
        self.assertEqual((tree.output().parent / "unrelated.txt").read_text(), "preserve")

    def test_authentication_receipt_cannot_be_false(self):
        self.tree.mutate_outer = lambda report, packing: report["header"].update(auth_verified=False)
        with self.assertRaises(ValueError):
            self.tree.seal()
        self.assertFalse(self.tree.output().exists())

    def test_public_source_memcmp_is_required(self):
        self.tree.mutate_outer = lambda report, packing: report.update(sources_verified=False) if packing else None
        with self.assertRaises(ValueError):
            self.tree.seal()
        self.assertFalse(self.tree.output().exists())

    def test_key_changed_after_pack_cannot_seal(self):
        def change(report, packing):
            if not packing:
                self.tree.key_path.write_text("11" * 32)
        self.tree.mutate_outer = change
        with self.assertRaises(ValueError):
            self.tree.seal()
        self.assertFalse(self.tree.output().exists())

    def test_secure_pack_and_fresh_report_must_prove_all_protection_fields(self):
        tree = self.tree
        changes = [lambda r: r["header"].update(encrypted=False),
            lambda r: r["header"].update(secure_index=False),
            lambda r: r["header"].update(authenticated=False),
            lambda r: r["header"].update(auth_verified=False),
            lambda r: r["header"].update(entry_count=2),
            lambda r: r["header"].update(dict_count=1),
            lambda r: r["header"].update(index_original_size=64),
            lambda r: r["totals"].update(encrypted_count=0),
            lambda r: r["totals"].update(authenticated_count=0),
            lambda r: r["totals"].update(skipped_count=1),
            lambda r: r["totals"].update(corrupt_count=1),
            lambda r: r["entries"][0].update(authenticated=False),
            lambda r: r["entries"][0].update(encrypted=False),
            lambda r: r["entries"][0].update(original_size=stock.BUNDLE_LIMIT + 1),
            lambda r: r["entries"].append(copy.deepcopy(r["entries"][0]))]
        for mode in (True, False):
            for index, mutate in enumerate(changes):
                tree.mutate_outer = lambda report, packing: mutate(report) if packing == mode else None
                name = "bad-secure-%s-%d.exe" % (mode, index)
                with self.subTest(pack=mode, index=index), self.assertRaises(ValueError):
                    tree.seal(name)
                self.assertFalse(tree.output(name).exists())
                self.assertFalse(tree.receipt(name).exists())

    def test_secure_pack_requires_actual_exact_source_byte_verification(self):
        tree = self.tree
        changes = [lambda r: r.update(sources_verified=False),
            lambda r: r.update(sources=[]),
            lambda r: r["sources"][0].update(path="stock/other.jbundle"),
            lambda r: r["sources"][0].update(size=1),
            lambda r: r["sources"][0].update(source=str(tree.bundle))]
        for index, mutate in enumerate(changes):
            tree.mutate_outer = lambda report, packing: mutate(report) if packing else None
            name = "bad-source-%d.exe" % index
            with self.subTest(index=index), self.assertRaises(ValueError):
                tree.seal(name)
            self.assertFalse(tree.output(name).exists())

    def test_relocated_runtime_and_key_content_seal_without_original_key_path(self):
        tree = self.tree
        original_key = tree.key_path
        original_manifest = tree.manifest_path.read_bytes()
        destination = tree.root / "relocated runtime"
        shutil.copytree(tree.runtime, destination)
        copied_key = tree.root / "external keys" / "selected.hex"
        copied_key.parent.mkdir()
        shutil.copy2(original_key, copied_key)
        original_key.unlink()
        tree.runtime = destination
        tree.manifest_path = destination / stock.MANIFEST_NAME
        tree.key_path = copied_key
        self.assertFalse(original_key.exists())
        self.assertNotEqual(str(copied_key), tree.manifest["producer"]["sealing_key_path"])
        receipt = tree.seal()
        self.assertEqual(tree.manifest_path.read_bytes(), original_manifest)
        self.assertEqual(receipt["status"], "SEALED_NOT_EXECUTED")
        self.assertTrue(receipt["project_protection"]["authenticated"])
        self.assertTrue(receipt["project_protection"]["exact_source_verified"])
        self.assertEqual(receipt["project_protection"]["key_sha256"], tree.key_sha)
        self.assertEqual(stock.inspect_envelope(tree.output().read_bytes()), (tree.host, tree.outer_bytes))
        self.assertEqual(len(tree.commands), 3)
        self.assertTrue(all(command[0] == str(destination / "jce_pak.exe")
                            for command, _ in tree.commands))
        self.assertNotIn(copied_key.read_text(), tree.receipt().read_text())
        self.assertEqual(list(tree.output().parent.iterdir()), [tree.output()])

    def test_wrong_or_malformed_explicit_keys_reject_before_output_at_both_locations(self):
        tree = self.tree
        copied_key = tree.root / "external keys" / "selected.hex"
        copied_key.parent.mkdir()
        shutil.copy2(tree.key_path, copied_key)
        copied_runtime = tree.root / "relocated runtime"
        shutil.copytree(tree.runtime, copied_runtime)
        manifests = (tree.manifest_path, copied_runtime / stock.MANIFEST_NAME)
        for manifest in manifests:
            for location in (tree.key_path, copied_key):
                for raw in (b"11" * 32, b"not a key", b"ff" * 33):
                    location.write_bytes(raw)
                    with self.subTest(runtime=manifest.parent.name, location=location.parent.name,
                                      size=len(raw)), self.assertRaises(ValueError):
                        stock.seal(runtime_manifest=manifest, bundle=tree.bundle, out=tree.output(),
                            work_dir=tree.root / "work", receipt=tree.receipt(), pak_key_file=location)
                    self.assertFalse(tree.commands)
                    self.assertFalse(tree.output().exists())
                    self.assertFalse(tree.receipt().exists())
        self.assertFalse(tree.commands)
        self.assertFalse(tree.output().exists())

    def test_legacy_runtime_package_and_changed_key_binding_are_rejected(self):
        tree = self.tree
        original = copy.deepcopy(tree.manifest)
        changes = [lambda m: m.update(schema=1),
            lambda m: m["protection"].update(envelope_version=1),
            lambda m: m["protection"].update(key_sha256="d" * 64),
            lambda m: m["producer"].update(sealing_key_sha256="d" * 64)]
        for change in changes:
            tree.manifest = copy.deepcopy(original)
            change(tree.manifest)
            tree.save_manifest()
            with self.assertRaises(ValueError):
                stock.load_runtime(tree.manifest_path)

    def test_candidate_and_key_changed_by_outer_tool_are_rejected_before_output(self):
        tree = self.tree
        def change(report, packing):
            if not packing:
                tree.bundle.write_bytes(b"x" * tree.bundle.stat().st_size)
        tree.mutate_outer = change
        with self.assertRaises(ValueError):
            tree.seal()
        self.assertFalse(tree.output().exists())

    def test_javascript_uses_same_owner_without_new_pack_recipe(self):
        tree = self.tree
        tree.assets[1].update(path="scripts/main.jcejs", representation="js.source")
        tree.assets[0]["dependencies"][0]["address"] = tree.assets[1]["path"]
        self.assertEqual(tree.seal()["languages"], ["js"])

    def test_native_and_optional_vm_scripts_are_not_silently_dropped(self):
        tree = self.tree
        for extension, representation in (("jcec", "c.class-ref"), ("jcecpp", "cpp.class-ref"),
                ("py", "python.source"), ("jcejs", "lua.source")):
            tree.assets[1].update(path="scripts/main." + extension, representation=representation)
            tree.assets[0]["dependencies"][0]["address"] = tree.assets[1]["path"]
            with self.subTest(extension=extension), self.assertRaises(ValueError):
                tree.seal("bad-" + extension + ".exe")
            self.assertFalse(tree.output("bad-" + extension + ".exe").exists())

    def test_mislabelled_native_source_as_data_is_rejected(self):
        tree = self.tree
        tree.assets[1].update(path="scripts/native.cpp", type="data", representation="binary")
        tree.assets[0]["dependencies"][0]["address"] = tree.assets[1]["path"]
        with self.assertRaises(ValueError):
            tree.seal()

    def test_mislabelled_script_representations_match_native_reader_admission(self):
        tree = self.tree
        for index, (path, representation) in enumerate((
                ("data.bin", "c.class-ref"), ("data.bin", "python.source"),
                ("main.lua", "binary"), ("data.bin", "lua.source"))):
            tree.assets[1].update(path=path, type="data", representation=representation)
            tree.assets[0]["dependencies"][0]["address"] = path
            with self.subTest(index=index), self.assertRaises(ValueError):
                tree.seal("bad-data-%d.exe" % index)

    def test_future_bundle_minor_is_not_sealed_for_a_stricter_native_reader(self):
        self.tree.mutate_report = lambda report: report["bundle"]["manifest"]["contract"].update(minor=2)
        with self.assertRaises(ValueError):
            self.tree.seal()

    def test_incomplete_deep_hash_and_graph_evidence_is_rejected(self):
        tree = self.tree
        cases = [lambda r: r["totals"].update(corrupt_count=1),
                 lambda r: r["totals"].update(skipped_count=1),
                 lambda r: r["header"].update(header_verified=False),
                 lambda r: r["bundle"]["manifest"].update(depends_on=["missing"]),
                 lambda r: r["bundle"]["manifest"].update(scene_path="missing.scene.json"),
                 lambda r: r["bundle"]["manifest"]["assets"][0]["dependencies"][0].update(asset_id="f" * 16)]
        for index, mutate in enumerate(cases):
            tree.mutate_report = mutate
            with self.subTest(index=index), self.assertRaises(ValueError):
                tree.seal("bad-%d.exe" % index)
            self.assertFalse(tree.output("bad-%d.exe" % index).exists())

    def test_missing_fresh_report_is_not_success(self):
        with patch.object(stock._InspectionDriver, "run", return_value=None):
            with self.assertRaises(ValueError):
                self.tree.seal()

    def test_input_changed_during_inspection_is_rejected_before_output(self):
        tree = self.tree
        tree.after_inspection = lambda: tree.bundle.write_bytes(b"x" * tree.bundle.stat().st_size)
        with self.assertRaises(ValueError):
            tree.seal()
        self.assertFalse(tree.output().exists())

    def test_runtime_changed_after_inspection_is_rejected(self):
        tree = self.tree
        def change():
            path = tree.runtime / "stock.exe"
            changed = bytearray(path.read_bytes())
            changed[-1] ^= 1
            path.write_bytes(changed)
        tree.after_inspection = change
        with self.assertRaises(ValueError):
            tree.seal()
        self.assertFalse(tree.output().exists())

    def test_runtime_manifest_identity_types_and_duplicates_are_strict(self):
        tree = self.tree
        original = copy.deepcopy(tree.manifest)
        changes = [lambda m: m.update(schema=True),
                   lambda m: m["target"].update(arch="arm64"),
                   lambda m: m.update(languages=["lua", "js", "c"]),
                   lambda m: m["files"]["runtime"].update(path="../stock.exe"),
                   lambda m: m["files"]["runtime"].update(size=True),
                   lambda m: m["sdk"].update(product_sha256="unbound")]
        for change in changes:
            tree.manifest = copy.deepcopy(original)
            change(tree.manifest)
            tree.save_manifest()
            with self.assertRaises(ValueError):
                stock.load_runtime(tree.manifest_path)
        tree.manifest_path.write_text('{"schema":1,"schema":1}', encoding="utf-8")
        with self.assertRaises(ValueError):
            stock.load_runtime(tree.manifest_path)

    def test_embedded_notice_failure_cannot_seal(self):
        tree = self.tree
        with patch.object(stock.standalone, "verify_notices", return_value={}):
            with self.assertRaises(ValueError):
                tree.seal()
        self.assertFalse(tree.output().exists())

    def test_relocated_runtime_package_uses_only_its_bound_tool(self):
        tree = self.tree
        destination = tree.root / "relocated"
        shutil.copytree(tree.runtime, destination)
        runtime = stock.load_runtime(destination / stock.MANIFEST_NAME)
        self.assertEqual(runtime.host, tree.host)
        self.assertEqual(runtime.inspector, destination / "jce_pak.exe")
        # No SDK lookup, build recipe or compiler port is consulted by load.
        self.assertEqual(runtime.manifest["sdk"]["product_sha256"], "a" * 64)

    def test_publication_binds_sdk_and_real_input_bytes_without_building(self):
        tree = self.tree
        sdk = tree.root / "sdk"
        (sdk / "bin").mkdir(parents=True)
        (sdk / "share/jce").mkdir(parents=True)
        (sdk / "bin/jce_pak.exe").write_bytes(tree.inspector)
        (sdk / "share/jce/THIRD_PARTY_LICENSES.md").write_bytes(tree.notice["THIRD_PARTY_LICENSES.md"])
        product = {"schema": 1, "scope": "counted-sdk-product-port"}
        product_raw = json.dumps(product).encode()
        (sdk / stock.sdk_product.PRODUCT_FILE).write_bytes(product_raw)
        target = tree.root / "published"
        inputs = producer._snapshot(ROOT)
        compile_identity = sha(json.dumps({"sdk": sha(product_raw), "sources": inputs},
            sort_keys=True, separators=(",", ":")).encode())
        native_log = tree.root / "counted-native-log.txt"
        native_log.write_bytes(b"typed trusted producer receipt port; not native execution")
        build_receipt = tree.root / "component-receipt.json"
        build_value = {"schema": 1, "scope": "stock-runtime-component-producer",
            "status": "STOCK_COMPONENT_BUILT_CPU_CONTROLS_PASSED_NOT_AN_APPLICATION", "returncode": 0,
            "runnable_application": False, "runtime_execution_verified": False,
            "target": dict(stock._TARGET), "sdk": {"path": str(sdk), "product_sha256": sha(product_raw)},
            "output": {"path": str(tree.runtime / "stock.exe"), "size": len(tree.host), "sha256": sha(tree.host)},
            "assets": {"has_runtime_boot": False},
            "cache": str(tree.key_path.parent.parent),
            "sealing_key": {"path": str(tree.key_path), "sha256": tree.key_sha}, "inputs_before": inputs, "inputs_after": inputs,
            "compile_input_sha256": compile_identity,
            "native_cpu_log": {"path": str(native_log), "sha256": sha(native_log.read_bytes())}}
        build_receipt.write_text(json.dumps(build_value))
        trusted_sha = sha(build_receipt.read_bytes())
        with patch.object(stock.sdk_product, "verify_product", return_value=product) as verify:
            result = stock.publish_runtime(exe=tree.runtime / "stock.exe", sdk=sdk,
                license_file=tree.runtime / "LICENSE", out=target,
                build_receipt=build_receipt, build_receipt_sha256=trusted_sha)
        self.assertEqual(verify.call_count, 1)
        self.assertEqual(verify.call_args.kwargs["delivery"], "embedded")
        self.assertEqual(verify.call_args.kwargs["arch"], "x86_64")
        self.assertEqual(result["sdk"]["product_sha256"], sha(product_raw))
        self.assertEqual((target / "jce_stock_runtime.exe").read_bytes(), tree.host)
        self.assertEqual(set(result["files"]), {"runtime", "inspector"})
        self.assertEqual(result["protection"]["key_sha256"], tree.key_sha)
        self.assertEqual({x.name for x in target.iterdir()},
            {"jce_stock_runtime.exe", "jce_pak.exe", "LICENSE", "THIRD_PARTY_LICENSES.md", stock.MANIFEST_NAME})
        self.assertEqual(stock.load_runtime(target / stock.MANIFEST_NAME).host, tree.host)
        self.assertFalse(tree.commands)
        with patch.object(stock.sdk_product, "verify_product", return_value=product):
            with self.assertRaises(FileExistsError):
                stock.publish_runtime(exe=tree.runtime / "stock.exe", sdk=sdk,
                    license_file=tree.runtime / "LICENSE", out=target,
                build_receipt=build_receipt, build_receipt_sha256=trusted_sha)

    def test_parser_dispatch_is_explicit_and_has_no_implicit_build(self):
        parser = stock.argparse.ArgumentParser()
        stock.add_arguments(parser)
        args = parser.parse_args(["seal", "--runtime-manifest", str(self.tree.manifest_path),
            "--bundle", str(self.tree.bundle), "--pak-key-file", str(self.tree.key_path), "--out", str(self.tree.output("cli.exe")),
            "--work-dir", str(self.tree.root / "work/cli.exe"), "--receipt", str(self.tree.receipt("cli.exe"))])
        self.assertEqual(stock.execute(args)["status"], "SEALED_NOT_EXECUTED")
        self.assertEqual(len(self.tree.commands), 3)


if __name__ == "__main__":
    unittest.main()
