"""Negative controls for native single-file delivery; runs without MSVC."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("standalone_test", ROOT / "tools/build/standalone.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def pe_fixture(imports, delayed=()):
    """Small PE32+ file with ordinary and delay-load dependency directories."""
    blob = bytearray(4096)
    blob[:2] = b"MZ"
    struct.pack_into("<I", blob, 0x3c, 0x80)
    blob[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", blob, 0x84, 0x8664, 1)
    struct.pack_into("<H", blob, 0x94, 240)
    struct.pack_into("<H", blob, 0x98, 0x20b)
    struct.pack_into("<Q", blob, 0x98 + 24, 0x140000000)
    struct.pack_into("<I", blob, 0x98 + 108, 16)
    section = 0x98 + 240
    struct.pack_into("<IIII", blob, section + 8, 3584, 0x1000, 3584, 512)
    cursor = 2048
    for directory, offset, width, names in ((1, 512, 20, imports), (13, 1024, 32, delayed)):
        if not names:
            continue
        struct.pack_into("<II", blob, 0x98 + 112 + directory * 8,
                         offset - 512 + 0x1000, (len(names) + 1) * width)
        for i, name in enumerate(names):
            encoded = name.encode("ascii") + b"\0"
            blob[cursor:cursor + len(encoded)] = encoded
            descriptor = offset + i * width
            rva = cursor - 512 + 0x1000
            if directory == 1:
                struct.pack_into("<I", blob, descriptor + 12, rva)
            else:
                struct.pack_into("<II", blob, descriptor, 1, rva)
            cursor += len(encoded)
    return blob


class NativeStandaloneTests(unittest.TestCase):
    def test_editor_asset_pack_must_match_verified_inventory_and_exe(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            (build / "reports").mkdir()
            exe = build / "editor.exe"
            pack = build / "editor_assets.pak"
            bom = build / "reports/editor_assets_bom.json"
            pack.write_bytes(b"JPAK-asset-payload")
            exe.write_bytes(b"prefix" + pack.read_bytes() + b"suffix")
            inventory = {"file_size": pack.stat().st_size,
                         "totals": {"entries": 1, "verified_count": 1,
                                    "corrupt_count": 0, "original_size": 100,
                                    "stored_size": 20},
                         "entries": [{"path": "shaders/vs.bin", "verified": True,
                                      "original_size": 100, "stored_size": 20}]}
            bom.write_text(json.dumps(inventory), encoding="utf-8")
            result = MODULE.verify_assets(exe, build)
            self.assertEqual(result["groups"]["shaders"]["entries"], 1)
            exe.write_bytes(b"prefix only")
            with self.assertRaisesRegex(ValueError, "not embedded"):
                MODULE.verify_assets(exe, build)
            exe.write_bytes(pack.read_bytes())
            inventory["totals"]["verified_count"] = 0
            bom.write_text(json.dumps(inventory), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "incomplete"):
                MODULE.verify_assets(exe, build)

    def test_os_only_exe_is_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "editor.exe"
            exe.write_bytes(pe_fixture(["KERNEL32.dll", "USER32.dll"], ["d3d11.dll"]))
            result = MODULE.verify(exe)
            self.assertEqual(result["imports"], ["d3d11.dll", "kernel32.dll", "user32.dll"])
            self.assertEqual(result["embedded_vms"], ["lua", "js"])
            self.assertFalse(result["project_modules_embedded"])
            self.assertFalse(result["sdk_embedded"])
            self.assertFalse(result["self_extracting"])

    def test_external_runtime_import_is_rejected_in_either_table(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "editor.exe"
            for name in ("python312.dll", "nethost.dll", "MSVCP140.dll", "VCRUNTIME140.dll", "jce_script_api.dll"):
                for delayed in (False, True):
                    exe.write_bytes(pe_fixture(["KERNEL32.dll"] + ([] if delayed else [name]), [name] if delayed else []))
                    with self.subTest(name=name, delayed=delayed), self.assertRaisesRegex(ValueError, "external DLLs"):
                        MODULE.verify(exe)

    def test_truncated_or_wrong_image_cannot_be_delivered(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "editor.exe"
            for blob in (b"MZ", b"not an executable", pe_fixture([])):
                exe.write_bytes(blob)
                with self.assertRaises(ValueError):
                    MODULE.verify(exe)

    def test_recipe_isolated_static_graph_and_explicit_languages(self):
        driver = SimpleNamespace(ROOT=ROOT, HOST="windows", DRY_RUN=True,
                                 sync_conan_hooks=Mock(), export_conan_recipes=Mock(),
                                 find_vs_install=lambda arch: None,
                                 host_build_profile=lambda: "windows-x64",
                                 bgfx_graphics_conan_args=lambda env: [],
                                 run=Mock(), build_jobs=lambda: "2", log=Mock())
        args = SimpleNamespace(variant="dist", clean=False)
        target = {"host": "windows", "arch": "x86_64", "profile": "windows-x64", "key": "windows-x64"}
        exe = MODULE.build_editor(driver, args, target, {})
        commands = [call.args[0] for call in driver.run.call_args_list]
        self.assertIn("compiler.runtime=static", commands[0])
        self.assertIn("*:shared=False", commands[0])
        self.assertIn("-DJCE_EDITOR_STANDALONE=ON", commands[1])
        for language in ("VM_PYTHON", "CSHARP", "JAVA"):
            self.assertIn("-DJCE_BUILD_SCRIPT_" + language + "=OFF", commands[1])
        self.assertIn("-DJCE_ENABLE_SDK_INSTALL=OFF", commands[1])
        self.assertIn("-DJCE_BUILD_TESTS=OFF", commands[1])
        self.assertIn("windows-x64-standalone", str(exe))
        cmake = (ROOT / "editor/CMakeLists.txt").read_text()
        self.assertIn("LANGUAGES c cpp js REQUIRED NO_STAGE", cmake)


if __name__ == "__main__":
    unittest.main()
