"""CPU controls for the real XP Editor owner; no native tools or guest runs."""
import argparse
import contextlib
import importlib.util
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import types
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("jce_xp_editor_controls", ROOT / "tools/build/xp_editor.py")
XP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(XP)
PE_SPEC = importlib.util.spec_from_file_location("jce_xp_editor_synthetic_pe", ROOT / "tools/tests/test_xp_compat.py")
PE = importlib.util.module_from_spec(PE_SPEC)
PE_SPEC.loader.exec_module(PE)
RUNTIME_IDENTITY = XP.runtime_identity


def editor_pe(arch="x64", *, ordinal=False, delay=False, version=None):
    blob = bytearray(PE.sample_pe(arch, os_version=version))
    optional = 0x98
    struct.pack_into("<H", blob, optional + 68, 2)
    width = 8 if arch == "x64" else 4
    if ordinal:
        struct.pack_into("<Q" if width == 8 else "<I", blob, 0x400,
                         (1 << (width * 8 - 1)) | 42)
    if delay:
        directory = optional + (112 if width == 8 else 96)
        struct.pack_into("<II", blob, directory + 13 * 8, 0x1500, 64)
        # One RVA-based descriptor and its complete terminating descriptor.
        struct.pack_into("<IIIIIIII", blob, 0x700, 1, 0x1600, 0, 0x1680, 0x1680, 0, 0, 0)
        blob[0x800:0x80b] = b"user32.dll\0"
        struct.pack_into("<Q" if width == 8 else "<I", blob, 0x880, 0x16c0)
        blob[0x8c0:0x8cf] = b"\0\0MessageBoxW\0"
    return bytes(blob)


def guest_inventory(arch="x64"):
    return {"schema": "jce.xp.guest-exports.v1",
            "guest": {"arch": arch, "os_version": [5, 2] if arch == "x64" else [5, 1]},
            "dlls": {dll: {"sha256": "a" * 64, "names": names,
                            "ordinals": [42] if dll == "kernel32.dll" else [], "forwarders": {}}
                     for dll, names in {"kernel32.dll": ["GetProcAddress"], "msvcrt.dll": ["printf"],
                                        "advapi32.dll": ["CryptGenRandom"], "user32.dll": ["MessageBoxW"]}.items()}}


class XpEditorImportControls(unittest.TestCase):
    def audit(self, blob, arch="x64", inventory=True, require_gui=True):
        with tempfile.TemporaryDirectory() as temp:
            exe, exports = Path(temp) / "editor.exe", Path(temp) / "exports.json"
            exe.write_bytes(blob)
            value = guest_inventory(arch) if inventory is True else inventory
            if value is not None:
                exports.write_text(json.dumps(value), encoding="utf-8")
            return XP.audit_pe(exe, arch, exports if value is not None else None, require_gui=require_gui)

    def test_console_is_explicit_and_does_not_relax_default_editor_gui_gate(self):
        for arch in ("x86", "x64"):
            blob = bytearray(editor_pe(arch))
            struct.pack_into("<H", blob, 0x98 + 68, 3)
            with self.subTest(arch=arch), self.assertRaisesRegex(ValueError, "GUI subsystem"):
                self.audit(bytes(blob), arch)
            result = self.audit(bytes(blob), arch, require_gui=False)
            self.assertEqual(result["state"], "STATIC_IMPORT_AUDIT_PASSED")
            self.assertEqual(result["subsystem"], "CONSOLE")

    def test_console_opt_in_is_strict_bool_and_admits_only_gui_or_console(self):
        for flag in (0, 1, None, "false"):
            with self.subTest(flag=flag), self.assertRaisesRegex(ValueError, "strict boolean"):
                self.audit(editor_pe(), require_gui=flag)
        for subsystem in (0, 1, 7):
            blob = bytearray(editor_pe())
            struct.pack_into("<H", blob, 0x98 + 68, subsystem)
            with self.subTest(subsystem=subsystem), self.assertRaisesRegex(ValueError, "GUI or console"):
                self.audit(bytes(blob), require_gui=False)
        self.assertEqual(self.audit(editor_pe(), require_gui=False)["subsystem"], "GUI")

    def test_x86_and_x64_have_independent_exact_pe_floors(self):
        for arch, minimum in (("x86", [5, 1]), ("x64", [5, 2])):
            with self.subTest(arch=arch):
                report = self.audit(editor_pe(arch), arch)
                self.assertEqual(report["state"], "STATIC_IMPORT_AUDIT_PASSED")
                self.assertEqual(report["os_version"], minimum)
                self.assertEqual(report["runtime_execution"], "NOT_EXECUTED")
        with self.assertRaisesRegex(ValueError, "OS/subsystem"):
            self.audit(editor_pe(version=(6, 1)))
        with self.assertRaisesRegex(ValueError, "architecture"):
            self.audit(editor_pe("x86"))

    def test_header_and_small_dll_list_cannot_certify_full_imports(self):
        report = self.audit(editor_pe(), inventory=None)
        self.assertEqual(report["state"], "STATIC_UNVERIFIED")
        bad = guest_inventory()
        bad["dlls"]["kernel32.dll"]["names"] = []
        with self.assertRaisesRegex(ValueError, "absent from exact XP"):
            self.audit(editor_pe(), inventory=bad)

    def test_delayed_symbols_and_ordinals_are_checked_without_promotion(self):
        self.assertEqual(self.audit(editor_pe(delay=True))["imports"][-1]["kind"], "delayed")
        bad = guest_inventory()
        bad["dlls"]["user32.dll"]["names"] = []
        with self.assertRaisesRegex(ValueError, "MessageBoxW"):
            self.audit(editor_pe(delay=True), inventory=bad)
        self.audit(editor_pe(ordinal=True))
        bad = guest_inventory()
        bad["dlls"]["kernel32.dll"]["ordinals"] = []
        with self.assertRaisesRegex(ValueError, "kernel32.dll!42"):
            self.audit(editor_pe(ordinal=True), inventory=bad)

    def test_no_modern_runtime_or_external_dll_is_accepted(self):
        blob = bytearray(editor_pe())
        blob[0x300:0x30d] = b"libc++.dll\0\0\0"
        with self.assertRaisesRegex(ValueError, "external DLL"):
            self.audit(bytes(blob))
        bad = guest_inventory()
        bad["guest"]["os_version"] = [6, 1]
        with self.assertRaisesRegex(ValueError, "exact XP guest"):
            self.audit(editor_pe(), inventory=bad)

    def test_forwarded_exports_require_their_actual_destination(self):
        inventory = guest_inventory()
        inventory["dlls"]["kernel32.dll"]["forwarders"] = {
            "GetProcAddress": {"dll": "ntdll.dll", "symbol": "LdrGetProcedureAddress"}}
        with self.assertRaisesRegex(ValueError, "provenance"):
            self.audit(editor_pe(), inventory=inventory)
        inventory["dlls"]["ntdll.dll"] = {"sha256": "b" * 64,
            "names": ["LdrGetProcedureAddress"], "ordinals": [], "forwarders": {}}
        self.audit(editor_pe(), inventory=inventory)
        inventory["dlls"]["ntdll.dll"]["forwarders"] = {
            "LdrGetProcedureAddress": {"dll": "kernel32.dll", "symbol": "GetProcAddress"}}
        with self.assertRaisesRegex(ValueError, "cyclic"):
            self.audit(editor_pe(), inventory=inventory)

    def test_winsock_extension_forwarder_requires_exact_guest_image_and_symbol(self):
        inventory = guest_inventory()
        inventory["dlls"]["kernel32.dll"]["forwarders"] = {
            "GetProcAddress": {"dll": "mswsock.dll", "symbol": "AcceptEx"}}
        with self.assertRaisesRegex(ValueError, "provenance"):
            self.audit(editor_pe(), inventory=inventory)
        inventory["dlls"]["mswsock.dll"] = {"sha256": "b" * 64,
            "names": ["AcceptEx"], "ordinals": [], "forwarders": {}}
        self.assertEqual(self.audit(editor_pe(), inventory=inventory)["state"],
                         "STATIC_IMPORT_AUDIT_PASSED")
        inventory["dlls"]["mswsock.dll"]["names"] = []
        with self.assertRaisesRegex(ValueError, "mswsock.dll!AcceptEx"):
            self.audit(editor_pe(), inventory=inventory)

    def test_resource_wrapper_and_real_rc_join_each_runtime_fingerprint(self):
        xp = XP._module("xp_compat")
        with mock.patch.object(XP, "_module", return_value=xp), mock.patch.object(
                Path, "is_file", return_value=True), mock.patch.object(XP, "_sha", return_value="a" * 64), mock.patch.object(
                XP, "_toolchain_inventory", return_value={"files": 0, "sha256": "b" * 64}):
            for arch, triple in (("x86", "i686-w64-mingw32"), ("x64", "x86_64-w64-mingw32")):
                actual = RUNTIME_IDENTITY(ROOT / "synthetic-msvcrt", arch)
                self.assertIn("bin/" + triple + "-windres.exe", actual)
                self.assertIn("bin/llvm-rc.exe", actual)

    def test_toolchain_inventory_includes_header_and_import_archive_contents(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for directory in ("bin", "include/c++/v1", "i686-w64-mingw32/include",
                              "i686-w64-mingw32/lib", "lib/clang/22/include", "lib/clang/22/lib/windows"):
                (root / directory).mkdir(parents=True)
            files = {"bin/common.cfg": b"cfg", "bin/compiler.dll": b"compiler",
                "include/c++/v1/__config": b"libc++ header", "i686-w64-mingw32/include/windows.h": b"Windows header",
                "i686-w64-mingw32/lib/libuser32.a": b"import archive", "lib/clang/22/include/stddef.h": b"clang header",
                "lib/clang/22/lib/windows/builtins.a": b"runtime"}
            for relative, content in files.items():
                (root / relative).write_bytes(content)
            baseline = XP._toolchain_inventory(root, "i686-w64-mingw32")
            self.assertEqual(baseline["files"], len(files))
            self.assertEqual(baseline["bytes"], sum(len(content) for content in files.values()))
            for relative in ("include/c++/v1/__config", "i686-w64-mingw32/include/windows.h", "i686-w64-mingw32/lib/libuser32.a"):
                (root / relative).write_bytes(files[relative] + b" changed")
                self.assertNotEqual(XP._toolchain_inventory(root, "i686-w64-mingw32")["sha256"], baseline["sha256"])
                (root / relative).write_bytes(files[relative])
            with self.assertRaisesRegex(ValueError, "budget"):
                XP._toolchain_inventory(root, "i686-w64-mingw32", max_files=1)
            with self.assertRaisesRegex(ValueError, "budget"):
                XP._toolchain_inventory(root, "i686-w64-mingw32", max_bytes=1)
            # A typed reparse metadata port avoids creating an actual system link.
            original_lstat = Path.lstat
            def reparse(path):
                status = original_lstat(path)
                if path == root / "include":
                    return types.SimpleNamespace(st_file_attributes=0x400, st_mode=status.st_mode)
                return status
            with mock.patch.object(Path, "lstat", reparse):
                with self.assertRaisesRegex(ValueError, "reparse/link"):
                    XP._toolchain_inventory(root, "i686-w64-mingw32")


    def test_global_header_layout_records_absent_optional_target_include(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for directory in ("bin", "include/c++/v1", "x86_64-w64-mingw32/lib", "lib/clang/22/include"):
                (root / directory).mkdir(parents=True)
            (root / "include/windows.h").write_bytes(b"global WinAPI headers")
            (root / "include/c++/v1/__config").write_bytes(b"global libc++ headers")
            (root / "x86_64-w64-mingw32/lib/libuser32.a").write_bytes(b"import archive")
            (root / "lib/clang/22/include/stddef.h").write_bytes(b"clang header")
            absent = XP._toolchain_inventory(root, "x86_64-w64-mingw32")
            self.assertTrue(absent["root_states"]["x86_64-w64-mingw32/include"].startswith("absent"))
            self.assertEqual(absent["root_states"]["include"], "directory")
            self.assertEqual(absent["files"], 4)
            (root / "include/windows.h").write_bytes(b"changed global WinAPI headers")
            self.assertNotEqual(XP._toolchain_inventory(root, "x86_64-w64-mingw32")["sha256"], absent["sha256"])
            (root / "include/windows.h").write_bytes(b"global WinAPI headers")
            target_include = root / "x86_64-w64-mingw32/include"
            target_include.mkdir()
            present = XP._toolchain_inventory(root, "x86_64-w64-mingw32")
            self.assertEqual(present["root_states"]["x86_64-w64-mingw32/include"], "directory")
            self.assertNotEqual(present["sha256"], absent["sha256"])
            # The missing-target fallback cannot mask missing global headers.
            target_include.rmdir()
            (root / "include/windows.h").unlink()
            with self.assertRaisesRegex(ValueError, r"requires global Windows/libc\+\+ headers"):
                XP._toolchain_inventory(root, "x86_64-w64-mingw32")


class XpEditorBuildControls(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name) / "repo"
        self.toolchain = Path(self.temporary.name) / "llvm-mingw-xp-22.1.7-msvcrt"
        self.desktop_target = self.repo / "build/desktop/windows-x64-xp"
        # JCE_EDITOR_VARIANT_LAYOUT_V4: XP fixture uses the canonical Dist tree by default.
        self.desktop_cache = self.desktop_target / "obj/dist"
        self.desktop_exe = self.desktop_target / "dist/jce_editor.exe"
        self.legacy_cache = self.repo / "build/compat/windows-x64-xp-editor-llvm-msvcrt"
        self.cache = self.desktop_cache
        self.home = self.repo / "build/xp-conan-home"
        self.store = self.repo / "build/compat/conan-xp-llvm-msvcrt-store"
        self.base = Path(self.temporary.name) / "original-conan"
        for directory in (self.repo / "cmake/toolchains", self.repo / "conan/profiles",
                          self.toolchain / "bin", self.home, self.base):
            directory.mkdir(parents=True)
        for name in ("windows-x64-xp-llvm.cmake", "windows-x86-xp-llvm.cmake", "windows-xp-llvm-common.cmake"):
            (self.repo / "cmake/toolchains" / name).write_text("# input\n", encoding="utf-8")
        for name in ("windows-xp-bgfx-isa.cmake", "windows-xp-bgfx-simd.hpp",
                     "windows-xp-bgfx-vulkan.cpp"):
            (self.repo / "cmake/toolchains" / name).write_bytes(b"# owned BX adapter\n")
        (self.repo / "conanfile.py").write_text("# root recipe\n", encoding="utf-8")
        self.stock_catalog = self.repo / "engine/shaders/stock_profiles.json"
        self.stock_catalog.parent.mkdir(parents=True)
        self.stock_catalog.write_bytes((ROOT / "engine/shaders/stock_profiles.json").read_bytes())
        self.stock_digest = hashlib.sha256(self.stock_catalog.read_bytes()).hexdigest()
        self.shader_directory = self.cache / "generated" / ("engine_assets-compat-" + self.stock_digest[:32]) / "shaders"
        # Declared outputs of the counted external CMake configure port.
        self.stock_metadata = {
            "JCE_SHADER_STOCK_POLICY_NAME": "compatibility",
            "JCE_SHADER_STOCK_POLICY_SHA256": self.stock_digest,
            "JCE_SHADER_OUTPUT_DIR": str(self.shader_directory),
        }
        ports = self.repo / "engine/src/os/platform/compatibility"
        ports.mkdir(parents=True)
        for name in ("jce_windows_xp_sync_internal.h", "jce_windows_xp_mimalloc_internal.h",
                     "jce_windows_xp_codec_sync_internal.h", "jce_windows_xp_codecs_internal.h",
                     "jce_windows_xp_stdio_internal.h", "jce_windows_xp_bx_memory_internal.h"):
            (ports / name).write_bytes(b"/* owned platform header */\n")
        (self.repo / "conan/hooks").mkdir()
        (self.repo / "conan/hooks/hook_fixture.py").write_text("# configuration hook v1\n", encoding="utf-8")
        (self.repo / "conan/profiles/windows-x64").write_text("include(base)\n[settings]\ncompiler=msvc\n", encoding="utf-8")
        (self.repo / "conan/profiles/base").write_text("[settings]\nbuild_type=Release\n", encoding="utf-8")
        (self.home / "settings.yml").write_text("compiler: {}\n", encoding="utf-8")
        (self.home / "settings_user.yml").write_text('{"compiler":{"clang":{"version":["22"]}}}\n', encoding="utf-8")
        (self.home / "global.conf").write_text("core.cache:storage_path=" + self.store.as_posix() + "\n", encoding="utf-8")
        (self.home / ".jce-toolchain-overlay.json").write_text(json.dumps({"home": str(self.home),
            "clang_major": "22", "storage": str(self.store), "scope": "independent selected package store"}), encoding="utf-8")
        (self.base / "settings.yml").write_text("compiler:\n  clang:\n    version: [\"21\"]\n", encoding="utf-8")
        (self.base / "global.conf").write_text("tools.build:jobs=2\n", encoding="utf-8")
        (self.base / "p").mkdir()
        (self.base / "p/cache.sqlite3").write_bytes(b"original store sentinel")
        self.host_tools = {}
        for key in ("shaderc", "host_pak", "host_bin2obj"):
            path = self.repo / (key + ".exe")
            path.write_bytes(b"CPU fixture, not executable")
            self.host_tools[key] = path
        self.calls = []
        self.compiler_info = {"clang_version": "clang version 22.1.7", "target": "x86_64-w64-windows-gnu",
                              "clang_c_sha256": "a" * 64, "clang_cxx_sha256": "b" * 64,
                              "target_config_sha256": "c" * 64, "toolchain_root": str(self.toolchain)}
        self.preflight = mock.patch.object(XP, "_preflight", return_value=self.compiler_info).start()
        mock.patch("builtins.print").start()
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(XP, "runtime_identity", return_value={"libc++.a": "d" * 64}).start()
        self.driver = types.SimpleNamespace(sync_conan_hooks=lambda env: self.assertEqual(env["CONAN_HOME"], str(self.home)))
        self.graph = {"graph": {"nodes": {
            "0": {"context": "host", "ref": "jce"},
            "1": {"context": "host", "ref": "sdl/3.4.0", "package_id": "gnu-target",
                  "settings": {"os": "Windows", "arch": "x86_64", "compiler": "clang",
                               "compiler.version": "22", "compiler.libcxx": "libc++"},
                  "options": {"shared": "False"}},
            "2": {"context": "build", "ref": "protobuf/7.35.0", "package_id": "native-host",
                  "settings": {"compiler": "msvc"}}
        }}}

    def runner(self, command, **kwargs):
        self.calls.append((command, kwargs))
        if command[:3] == ["conan", "graph", "info"]:
            return subprocess.CompletedProcess(command, 0, json.dumps(self.graph), "Conan graph diagnostic\n")
        if command[:2] == ["conan", "install"]:
            generated = (self.desktop_target / "conan" / self.cache.name /
                         "build/Release/generators/conan_toolchain.cmake")
            generated.parent.mkdir(parents=True, exist_ok=True)
            generated.write_text("# CPU generated-interface port\n", encoding="utf-8")
            return subprocess.CompletedProcess(command, 0, json.dumps(self.graph), "Conan diagnostic\n")
        if command[:2] == ["cmake", "-S"]:
            # JCE_XP_LAYOUT_PROVENANCE_V4_3: simulated CMake regeneration follows -B exactly.
            binary_dir = Path(command[command.index("-B") + 1]).resolve()
            self.assertEqual(binary_dir, self.cache.resolve(),
                             "simulated configure used a different variant cache")
            cache_file = binary_dir / "CMakeCache.txt"
            values = XP._cache_values(cache_file) if cache_file.is_file() else {}
            for arg in command:
                if arg.startswith("-D"):
                    name, value = arg[2:].split("=", 1)
                    values[name] = value
            for key, suffix in (("CMAKE_C_COMPILER", "-clang.exe"), ("CMAKE_CXX_COMPILER", "-clang++.exe")):
                values[key] = str(self.toolchain / "bin" / ("x86_64-w64-mingw32" + suffix))
            values.setdefault("JCE_ENABLE_PATENTED_CODECS", "ON")
            for generated_stock_key in ("JCE_SHADER_STOCK_POLICY_NAME",
                                        "JCE_SHADER_STOCK_POLICY_SHA256",
                                        "JCE_SHADER_OUTPUT_DIR"):
                values.pop(generated_stock_key, None)
            values.update(self.stock_metadata)
            (self.cache / "CMakeCache.txt").write_text("\n".join(key + ":STRING=" + value for key, value in values.items()) + "\n", encoding="utf-8")
            self.shader_directory.mkdir(parents=True, exist_ok=True)
        if command[:2] == ["cmake", "--build"]:
            self.assertEqual(Path(command[2]), self.cache)
            values = XP._cache_values(self.cache / "CMakeCache.txt")
            output = Path(values["JCE_EDITOR_ARTIFACT_DIR"])
            output.mkdir(parents=True, exist_ok=True)
            (output / "jce_editor.exe").write_bytes(editor_pe())
        return subprocess.CompletedProcess(command, 0, "CPU command port\n", "")

    def build(self, phase="build", **kwargs):
        options = dict(phase=phase, conan_home=self.home, runner=self.runner, driver=self.driver,
                       build_profile=self.repo / "conan/profiles/windows-x64",
                       env={"PATH": "explicit-cpu-path", "LIB": "must-not-leak"}, **self.host_tools)
        options.update(kwargs)
        # JCE_XP_RECEIPT_VARIANT_V4_3_2: follow the variant actually requested.
        self._last_variant = options.get("variant", "dist")
        return XP.build(self.repo, "x64", self.toolchain, 2, **options)

    def native_installation(self):
        installation = self.repo / "native VS"
        toolset = installation / "VC/Tools/MSVC/14.43.34808"
        for relative in ("bin/Hostx64/x64/cl.exe", "bin/Hostx64/x64/link.exe",
                         "include/yvals_core.h", "lib/x64/msvcprt.lib"):
            path = toolset / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"synthetic ordinary input; never executed\n")
        (self.repo / "conan/profiles/windows-x64").write_bytes(
            b"include(base)\n[settings]\nos=Windows\narch=x86_64\ncompiler=msvc\n"
            b"compiler.version=194\ncompiler.cppstd=20\ncompiler.runtime=dynamic\n")
        self.driver.find_vs_install = lambda arch: installation
        return installation

    def test_release_codec_preference_and_dist_cache_are_separate(self):
        # Actual producer + counted configure port; no vendor or compiler runs.
        self.build("configure")
        self.assertEqual(self.latest()["capabilities"]["patented_codecs"]["effective"], "OFF")
        dist_owner = (self.desktop_cache / "xp-editor-owner.json").read_bytes()
        self.cache = self.desktop_target / "obj/release"
        self.assertFalse((self.cache / "CMakeCache.txt").exists(),
                         "Release fixture inherited an existing CMake cache")
        self.shader_directory = self.cache / "generated" / ("engine_assets-compat-" + self.stock_digest[:32]) / "shaders"
        self.stock_metadata["JCE_SHADER_OUTPUT_DIR"] = str(self.shader_directory)
        self.build("configure", variant="release")
        command = next(cmd for cmd, _ in reversed(self.calls) if cmd[:2] == ["cmake", "-S"])
        self.assertIn("-DJCE_BUILD_VARIANT=release", command)
        self.assertFalse(any(flag.startswith("-DJCE_ENABLE_PATENTED_CODECS=") for flag in command))
        self.assertEqual(self.latest()["capabilities"]["patented_codecs"]["effective"], "ON",
                         "Release configured CMake values: " +
                         repr(XP._cache_values(self.cache / "CMakeCache.txt")))
        self.assertEqual(self.latest()["canonical_candidate"], str(self.desktop_target / "release/jce_editor.exe"))
        self.assertEqual(self.latest()["cache"], str(self.cache))
        self.assertEqual(dist_owner, (self.desktop_cache / "xp-editor-owner.json").read_bytes())
        self.build("configure", variant="release", patented_codecs="off")
        command = next(cmd for cmd, _ in reversed(self.calls) if cmd[:2] == ["cmake", "-S"])
        self.assertIn("-DJCE_ENABLE_PATENTED_CODECS=OFF", command)
        self.assertEqual(self.latest()["capabilities"]["patented_codecs"]["effective"], "OFF")
        self.build("configure", variant="release")
        self.assertEqual(self.latest()["capabilities"]["patented_codecs"]["effective"], "OFF")
        self.build("configure", variant="release", patented_codecs="on")
        self.assertEqual(self.latest()["capabilities"]["patented_codecs"]["effective"], "ON",
                         "Release configured CMake values: " +
                         repr(XP._cache_values(self.cache / "CMakeCache.txt")))
        self.assertFalse((self.desktop_target / "release/jce_editor.exe").exists())

    def test_release_and_dist_invalid_codec_modes_refuse_before_native_launch(self):
        for bad in (True, "ON", "off;on", 1):
            with self.subTest(bad=bad), self.assertRaisesRegex(ValueError, "XP patented-codecs"):
                self.build("configure", variant="release", patented_codecs=bad)
        with self.assertRaisesRegex(ValueError, "XP Editor variant"):
            self.build("configure", variant="unsupported")
        self.preflight.assert_not_called()

    def test_bx_diagnostic_request_reaches_graph_install_and_cache_identity(self):
        self.build("resolve", bgfx_bx_debug="on")
        resolve = next(c for c, _ in self.calls if c[:3] == ["conan", "graph", "info"])
        self.assertIn("user.jce:bgfx_bx_debug=on", resolve)
        self.assertIn("user.jce:bgfx_bx_debug_consumer=on", resolve)
        self.calls.clear()
        self.build("dependencies", bgfx_bx_debug="on")
        install = next(c for c, _ in self.calls if c[:2] == ["conan", "install"])
        self.assertIn("user.jce:bgfx_bx_debug=on", install)
        self.assertIn("user.jce:bgfx_bx_debug_consumer=on", install)
        self.assertEqual(self.latest()["bgfx_bx_debug_request"], "on")
        self.calls.clear()
        self.build("dependencies", bgfx_bx_debug="on")
        self.assertFalse(any(c[:2] == ["conan", "install"] for c, _ in self.calls))
        self.build("dependencies", bgfx_bx_debug="off")
        changed = next(c for c, _ in self.calls if c[:2] == ["conan", "install"])
        self.assertIn("user.jce:bgfx_bx_debug=off", changed)
        self.assertIn("user.jce:bgfx_bx_debug_consumer=off", changed)
        self.assertEqual(self.latest()["bgfx_bx_debug_request"], "off")

    def test_bx_audit_refuses_different_request_before_payload_or_publication(self):
        self.build(bgfx_bx_debug="on")
        payload = {"embedded_assets": {"entries": 4}, "embedded_notices": {"LICENSE": "a" * 64}}
        with mock.patch.object(XP, "_verify_payload", return_value=payload) as verify:
            for selected in ("off", "auto"):
                with self.subTest(selected=selected), self.assertRaisesRegex(ValueError, "built Editor identity"):
                    self.build("audit", bgfx_bx_debug=selected)
            verify.assert_not_called()
            self.assertFalse((self.repo / "dist").exists())
            self.assertIsNone(self.build("audit", bgfx_bx_debug="on"))
            verify.assert_called_once()

    def test_default_bx_diagnostic_request_leaves_historical_command_unchanged(self):
        self.build("dependencies")
        install = next(c for c, _ in self.calls if c[:2] == ["conan", "install"])
        self.assertFalse(any("bgfx_bx_debug" in item for item in install))
        self.assertEqual(self.latest()["bgfx_bx_debug_request"], "auto")

    def test_invalid_bx_diagnostic_request_refuses_before_preflight_or_cache(self):
        for selected in (None, True, 1, "ON", "Debug", "on;other"):
            with self.subTest(selected=selected), self.assertRaisesRegex(ValueError, "bgfx BX debug"):
                self.build("dependencies", bgfx_bx_debug=selected)
        self.preflight.assert_not_called()
        self.assertFalse(self.cache.exists())

    def test_consumer_cohorts_are_explicit_and_do_not_reuse_mimalloc_identity(self):
        self.build("dependencies")
        receipt = self.latest()
        command = next(command for command, _ in self.calls if command[:2] == ["conan", "install"])
        ports = receipt["consumer_ports"]
        self.assertEqual(set(ports), {"codec", "stdio", "bx_memory"})
        self.assertEqual(len(ports["codec"]["source_headers"]), 3)
        self.assertEqual(len(ports["stdio"]["source_headers"]), 1)
        self.assertEqual(set(ports["bx_memory"]["source_headers"]), {
            "jce_windows_xp_bx_memory_internal.h",
            "windows-xp-bgfx-isa.cmake", "windows-xp-bgfx-simd.hpp",
            "windows-xp-bgfx-vulkan.cpp",
        })
        self.assertNotEqual(ports["codec"]["headers_sha256"], receipt["platform_port"]["headers_sha256"])
        for kind, port in ports.items():
            self.assertIn("user.jce:xp_" + kind + "_header=" + port["header"], command)
            self.assertIn("user.jce:xp_" + kind + "_headers_sha256=" + port["headers_sha256"], command)
        self.assertIn("user.jce:xp_consumer_toolchain=" +
                      (self.repo / "cmake/toolchains/windows-x64-xp-llvm.cmake").as_posix(), command)

    def test_changed_consumer_header_cannot_audit_an_old_built_executable(self):
        paths = [self.repo / "engine/src/os/platform/compatibility" / name for name in (
            "jce_windows_xp_stdio_internal.h", "jce_windows_xp_bx_memory_internal.h")]
        paths += [self.repo / "cmake/toolchains" / name for name in (
            "windows-xp-bgfx-isa.cmake", "windows-xp-bgfx-simd.hpp",
            "windows-xp-bgfx-vulkan.cpp")]
        for header in paths:
            with self.subTest(name=header.name):
                self.build()
                header.write_bytes(b"/* changed private adapter */\n")
                with self.assertRaisesRegex(ValueError, "built Editor identity"):
                    self.build("audit")
                self.assertFalse((self.repo / "dist").exists())

    def test_default_native_profile_pins_minor_before_dependency_install(self):
        self.native_installation()
        self.build("dependencies", build_profile=None)
        selected = self.latest()["native_build_profile"]
        profile = Path(selected["profile"])
        self.assertEqual(selected["selection"]["compiler.update"], "3")
        self.assertFalse(selected["selection"]["compiler_execution_verified"])
        command = next(command for command, _ in self.calls if command[:2] == ["conan", "install"])
        self.assertEqual(command[command.index("-pr:b") + 1], str(profile))
        self.assertIn("compiler.update=3\n", profile.read_text(encoding="utf-8"))
        self.assertIn("-D_WIN32_WINNT=0x0502", (self.cache / "xp-host.profile").read_text(encoding="utf-8"))

    def test_explicit_build_profile_bypasses_installed_toolset_selection(self):
        self.driver.find_vs_install = lambda arch: self.fail("explicit profile must bypass discovery")
        self.build("dependencies")
        self.assertEqual(self.latest()["native_build_profile"]["source"], "explicit")
        self.assertFalse((self.cache / "native-build-profiles").exists())

    def test_missing_native_installation_refuses_before_dependency_compilation(self):
        self.driver.find_vs_install = lambda arch: None
        with self.assertRaisesRegex(ValueError, "native MSVC installation"):
            self.build("dependencies", build_profile=None)
        self.assertFalse(any(command[:2] == ["conan", "install"] for command, _ in self.calls))

    def test_audit_cannot_generate_a_new_default_native_profile(self):
        self.native_installation()
        self.build("dependencies")
        with self.assertRaisesRegex(ValueError, "cohort is absent"):
            self.build("audit", build_profile=None)
        self.assertFalse((self.cache / "native-build-profiles").exists())

    def latest(self):
        # JCE_XP_RECEIPT_VARIANT_V4_3_2: receipts are variant-specific.
        # The producer writes Dist -> x64 and Release -> x64-release.
        variant = getattr(self, "_last_variant", "dist")
        self.assertIn(variant, ("release", "dist"))
        lane = "x64-release" if variant == "release" else "x64"
        pointer_path = self.repo / "build/validation/xp-editor" / lane / "latest.json"
        pointer = json.loads(pointer_path.read_text(encoding="utf-8"))
        receipt = json.loads(Path(pointer["receipt"]).read_text(encoding="utf-8"))
        self.assertEqual(receipt.get("variant"), variant,
                         "XP test loaded a receipt from the wrong build variant")
        return receipt

    def test_latest_receipt_selection_is_variant_specific(self):
        # JCE_XP_RECEIPT_VARIANT_V4_3_2: this test invokes no compiler or VM.
        base = self.repo / "build/validation/xp-editor"
        for lane, variant in (("x64", "dist"), ("x64-release", "release")):
            directory = base / lane
            directory.mkdir(parents=True, exist_ok=True)
            receipt_file = directory / "receipt-fixture.json"
            receipt_file.write_text(json.dumps({"variant": variant, "marker": variant}), encoding="utf-8")
            (directory / "latest.json").write_text(
                json.dumps({"receipt": str(receipt_file)}), encoding="utf-8")
        self._last_variant = "dist"
        self.assertEqual(self.latest()["marker"], "dist")
        self._last_variant = "release"
        self.assertEqual(self.latest()["marker"], "release")
        # A stale/crossed pointer must fail, not silently certify Release.
        release = base / "x64-release/receipt-fixture.json"
        release.write_text(json.dumps({"variant": "dist", "marker": "stale"}), encoding="utf-8")
        with self.assertRaisesRegex(AssertionError, "wrong build variant"):
            self.latest()


    def overlay_factory(self, base, home, major, *, package_store):
        """Small configuration-only factory port; never copy a package store."""
        home.mkdir(parents=True, exist_ok=True)
        (home / "settings.yml").write_bytes((base / "settings.yml").read_bytes())
        (home / "settings_user.yml").write_text(json.dumps({"compiler": {"clang": {"version": [major]}}}), encoding="utf-8")
        lines = [line for line in (base / "global.conf").read_text(encoding="utf-8").splitlines()
                 if not line.startswith("core.cache:storage_path=")]
        (home / "global.conf").write_text("\n".join(lines) + "\ncore.cache:storage_path=" + package_store.as_posix() + "\n", encoding="utf-8")
        return {"home": str(home), "storage": str(package_store), "clang_major": major,
                "scope": "configuration overlay; independent selected package store"}

    @contextlib.contextmanager
    def overlay_ports(self, factory=None):
        original = XP._module
        configuration, locks = original("conan_config"), original("cache_lock")
        with mock.patch.object(configuration, "prepare_compiler_home", side_effect=factory or self.overlay_factory) as prepare, mock.patch.object(
                locks, "cache_locks", wraps=locks.cache_locks) as ownership, mock.patch.object(
                XP, "_module", side_effect=lambda name: {"conan_config": configuration, "cache_lock": locks}.get(name) or original(name)):
            yield prepare, ownership

    def test_hook_revision_selects_new_small_home_without_replacing_old_hooks(self):
        driver = types.SimpleNamespace(sync_conan_hooks=lambda env: None)
        first = XP._default_conan_home(self.repo, self.cache)
        with self.overlay_ports() as (prepare, _):
            self.build("dependencies", conan_home=None, base_conan_home=self.base, driver=driver)
            installed = first / "extensions/hooks/hook_fixture.py"
            installed.parent.mkdir(parents=True)
            installed.write_bytes(b"old installed hook remains unchanged")
            (self.repo / "conan/hooks/hook_fixture.py").write_bytes(b"# configuration hook v2")
            second = XP._default_conan_home(self.repo, self.cache)
            self.assertNotEqual(first, second)
            self.build("dependencies", conan_home=None, base_conan_home=self.base, driver=driver)
        self.assertEqual(prepare.call_count, 2)
        self.assertEqual(installed.read_bytes(), b"old installed hook remains unchanged")
        self.assertEqual(self.latest()["conan_overlay"]["metadata"]["storage"], str(self.store))
        self.assertEqual(self.latest()["conan_overlay"]["metadata"]["home"], str(second))

    def test_default_overlay_uses_observed_major_and_independent_locked_store(self):
        default_home = XP._default_conan_home(self.repo, self.cache)
        before = {path.relative_to(self.base): path.read_bytes() for path in self.base.rglob("*") if path.is_file()}
        driver = types.SimpleNamespace(sync_conan_hooks=lambda env: self.assertEqual(env["CONAN_HOME"], str(default_home)))
        with self.overlay_ports() as (prepare, ownership):
            self.build("dependencies", conan_home=None, base_conan_home=self.base, driver=driver)
        prepare.assert_called_once_with(self.base, default_home, "22", package_store=self.store)
        for resource in (self.store, self.desktop_target, self.desktop_cache, self.legacy_cache):
            self.assertIn(resource, ownership.call_args.args[0])
        self.assertEqual(before, {path.relative_to(self.base): path.read_bytes() for path in self.base.rglob("*") if path.is_file()})
        receipt = self.latest()["conan_overlay"]
        self.assertEqual(receipt["metadata"]["storage"], str(self.store))
        self.assertEqual(receipt["metadata"]["clang_major"], "22")
        self.assertTrue(receipt["refreshed"])
        self.assertEqual(len(receipt["configuration_sha256"]), 64)
        self.assertFalse((self.store / "cache.sqlite3").exists())

    def test_auto_overlay_refreshes_changed_configuration_and_dependency_stamp(self):
        driver = types.SimpleNamespace(sync_conan_hooks=lambda env: None)
        with self.overlay_ports() as (prepare, _):
            self.build("dependencies", conan_home=None, base_conan_home=self.base, driver=driver)
            first = self.latest()["conan_overlay"]["configuration_sha256"]
            self.build("dependencies", conan_home=None, base_conan_home=self.base, driver=driver)
            self.assertEqual(prepare.call_count, 1)
            self.assertTrue(self.latest()["dependencies_reused"])
            (self.base / "global.conf").write_text("tools.build:jobs=3\n", encoding="utf-8")
            self.build("dependencies", conan_home=None, base_conan_home=self.base, driver=driver)
            self.assertEqual(prepare.call_count, 2)
        self.assertNotEqual(self.latest()["conan_overlay"]["configuration_sha256"], first)
        self.assertEqual(len([command for command, _ in self.calls if command[:2] == ["conan", "install"]]), 2)

    def test_original_store_overlap_fails_before_creating_any_package_store(self):
        (self.base / "global.conf").write_text("core.cache:storage_path=" + self.store.parent.as_posix() + "\n", encoding="utf-8")
        with self.overlay_ports() as (prepare, _):
            with self.assertRaisesRegex(ValueError, "overlaps the original"):
                self.build("dependencies", conan_home=None, base_conan_home=self.base)
        prepare.assert_not_called()
        self.assertFalse(self.store.exists())
        self.assertEqual(self.calls, [])

    def test_explicit_empty_overlay_needs_explicit_source_authorization(self):
        empty = self.repo / "build/explicit-empty-conan-home"
        empty.mkdir()
        with self.overlay_ports() as (prepare, _):
            with self.assertRaisesRegex(ValueError, "requires --base-conan-home"):
                self.build("dependencies", conan_home=empty)
            prepare.assert_not_called()
            self.build("dependencies", conan_home=empty, base_conan_home=self.base,
                       driver=types.SimpleNamespace(sync_conan_hooks=lambda env: None))
        self.assertTrue((empty / ".jce-toolchain-overlay.json").is_file())

    def test_wrong_native_major_and_shared_original_storage_are_refused(self):
        self.compiler_info["clang_version"] = "clang version 21.0.0"
        with self.assertRaisesRegex(ValueError, "native XP Clang major"):
            self.build("dependencies")
        self.assertFalse(self.store.exists())
        self.compiler_info["clang_version"] = "clang version 22.1.7"
        marker = self.home / ".jce-toolchain-overlay.json"
        data = json.loads(marker.read_text(encoding="utf-8"))
        data["storage"] = str(self.base / "p")
        marker.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "independent XP store"):
            self.build("dependencies")
        self.assertEqual((self.base / "p/cache.sqlite3").read_bytes(), b"original store sentinel")

    def test_source_change_during_factory_cannot_launch_dependencies(self):
        def changing(base, home, major, *, package_store):
            result = self.overlay_factory(base, home, major, package_store=package_store)
            (base / "global.conf").write_text("tools.build:jobs=4\n", encoding="utf-8")
            return result
        with self.overlay_ports(changing):
            with self.assertRaisesRegex(ValueError, "changed while preparing"):
                self.build("dependencies", conan_home=None, base_conan_home=self.base)
        self.assertEqual(self.calls, [])
        self.assertEqual(self.latest()["state"], "FAILED")

    def test_nonempty_unowned_xp_store_is_never_adopted_or_copied(self):
        self.store.mkdir(parents=True)
        sentinel = self.store / "old-package-sentinel"
        sentinel.write_bytes(b"existing package bytes")
        with self.overlay_ports() as (prepare, _):
            with self.assertRaisesRegex(ValueError, "nonempty unowned XP Conan package store"):
                self.build("dependencies")
        prepare.assert_not_called()
        self.assertEqual(sentinel.read_bytes(), b"existing package bytes")
        self.assertEqual(self.calls, [])

    def test_prepare_is_a_distinct_unbuilt_editor_tuple(self):
        self.assertEqual(self.build("prepare"), self.cache)
        self.preflight.assert_called_once()
        self.assertEqual(self.calls, [])
        receipt = self.latest()
        self.assertEqual(receipt["state"], "PREPARED")
        self.assertEqual(receipt["tuple"]["product"], "editor")
        self.assertEqual(receipt["tuple"]["abi"]["minimum_os"], [5, 2])
        self.assertIsNone(receipt["final_artifact"])
        self.assertFalse((self.repo / "dist").exists())

    def test_explicit_gnu_profile_has_actual_compilers_and_no_msvc_runtime(self):
        self.build("prepare")
        profile = (self.cache / "xp-host.profile").read_text(encoding="utf-8")
        for text in ("compiler.version=22", "compiler.libcxx=libc++", "compiler.cppstd=20",
                     "x86_64-w64-mingw32-clang.exe", "x86_64-w64-mingw32-windres.exe",
                     "-D_WIN32_WINNT=0x0502", "-static", "-pthread"):
            self.assertIn(text, profile)
        self.assertNotIn("compiler.runtime", profile)
        self.assertNotIn("compiler.runtime_type", profile)
        x86 = XP.target_profile(self.toolchain, "x86", self.repo / "cmake/toolchains/windows-x86-xp-llvm.cmake")
        self.assertIn("-D_WIN32_WINNT=0x0501", x86)
        self.assertIn("--minor-subsystem-version,1", x86)
        self.assertIn("i686-w64-mingw32-clang++.exe", x86)
        self.assertIn("i686-w64-mingw32-windres.exe", x86)

    def test_installed_conan_parser_preserves_profile_dictionary_and_bool_types(self):
        # This is the real installed Conan parser, not a fixture implementation
        # or CLI. The older JSON-lowercase boolean must fail its typed lookup.
        try:
            from conan.internal.model.conf import ConfDefinition
            from conan.errors import ConanException
        except ModuleNotFoundError:
            self.skipTest("installed Conan 2 configuration parser unavailable")
        key = "tools.cmake.cmaketoolchain:extra_variables"
        for arch, triple in (("x86", "i686-w64-mingw32"), ("x64", "x86_64-w64-mingw32")):
            with self.subTest(arch=arch):
                profile = XP.target_profile(self.toolchain, arch,
                    self.repo / "cmake/toolchains" / ("windows-" + arch + "-xp-llvm.cmake"))
                conf_text = profile.split("[conf]", 1)[1]
                conf = ConfDefinition()
                conf.loads(conf_text)
                actual = conf.get(key, check_type=dict)
                self.assertIs(type(actual), dict)
                self.assertIs(actual["THREADS_PREFER_PTHREAD_FLAG"], True)
                self.assertEqual(actual["JCE_XP_TOOLCHAIN_ROOT"], self.toolchain.as_posix())
                executables = conf.get("tools.build:compiler_executables", check_type=dict)
                self.assertEqual(executables["rc"], (self.toolchain / "bin" / (triple + "-windres.exe")).as_posix())
                old_conf = ConfDefinition()
                old_conf.loads(conf_text.replace(key + "=" + repr(actual), key + "=" + json.dumps(actual), 1))
                with self.assertRaisesRegex(ConanException, "dict"):
                    old_conf.get(key, check_type=dict)

    def test_tuple_reuse_is_exact_and_mismatch_never_erases_cache(self):
        self.build("prepare")
        preserved = self.cache / "preserve.o"
        preserved.write_bytes(b"retained object")
        self.build("prepare")
        self.compiler_info["clang_cxx_sha256"] = "e" * 64
        with self.assertRaisesRegex(ValueError, "tuple mismatch"):
            self.build("prepare")
        self.assertEqual(preserved.read_bytes(), b"retained object")
        self.assertEqual(self.latest()["state"], "FAILED")

    def test_dependencies_scope_abi_and_jobs_to_correct_contexts(self):
        self.build("dependencies")
        command, kwargs = self.calls[-1]
        for arg in ("user.jce:windows_xp=True", "user.jce:ai_dispatch=False", "tools.build:jobs=2",
                    "bgfx/*:tools=False", "*:shared=False"):
            self.assertIn(arg, command)
        self.assertTrue(any(arg.startswith("user.jce:windows_xp_abi=") for arg in command))
        self.assertIn('tools.info.package_id:confs=["user.jce:windows_xp","user.jce:windows_xp_abi"]', command)
        self.assertIn(str(self.repo / "conan/profiles/windows-x64"), command)
        self.assertEqual(kwargs["env"]["CMAKE_BUILD_PARALLEL_LEVEL"], "2")
        self.assertNotIn("LIB", kwargs["env"])
        self.assertEqual(len(self.latest()["target_packages"]), 1)
        self.assertFalse((self.repo / "dist").exists())

    def test_graph_reuse_and_changed_recipe_regenerate_only_metadata(self):
        self.build("dependencies")
        count = len(self.calls)
        retained = self.cache / "preserve.o"
        retained.write_bytes(b"object")
        self.build("dependencies")
        self.assertEqual(len(self.calls), count)
        (self.repo / "conanfile.py").write_text("# changed recipe\n", encoding="utf-8")
        self.build("dependencies")
        self.assertEqual(len(self.calls), count + 1)
        self.assertEqual(retained.read_bytes(), b"object")

    def test_resolve_preserves_missing_binaries_without_install_or_generation(self):
        self.graph["graph"]["nodes"]["1"]["binary"] = "Missing"
        self.graph["graph"]["nodes"]["2"]["binary"] = "Missing"
        recipe = self.repo / "conan/recipes/fixture/conanfile.py"
        recipe.parent.mkdir(parents=True)
        recipe.write_text("# first-party recipe export port\n", encoding="utf-8")
        graph_path = self.build("resolve")
        receipt = self.latest()
        self.assertEqual(receipt["state"], "DEPENDENCY_GRAPH_RESOLVED_NOT_BUILT")
        self.assertEqual(receipt["graph_admission"]["missing_binaries"], ["sdl/3.4.0", "protobuf/7.35.0"])
        self.assertEqual(receipt["graph_admission"]["build_packages"][0]["context"], "build")
        self.assertEqual(receipt["target_packages"][0]["binary_status"], "Missing")
        self.assertEqual(json.loads(graph_path.read_text(encoding="utf-8")), self.graph)
        commands = [command for command, _ in self.calls]
        self.assertEqual(commands[0][:2], ["conan", "export"])
        self.assertEqual(commands[-1][:3], ["conan", "graph", "info"])
        self.assertFalse(any(command[:2] == ["conan", "install"] or command[0] == "cmake" for command in commands))
        self.assertNotIn("--build=missing", commands[-1])
        self.assertNotIn("--output-folder", commands[-1])
        self.assertFalse((self.desktop_target / "conan" / self.cache.name).exists())
        self.assertFalse((self.repo / "dist").exists())
        self.assertIsNone(receipt["final_artifact"])

    def test_resolve_and_install_share_exact_profile_options_and_configuration(self):
        self.build("resolve")
        graph = self.calls[-1][0]
        self.build("dependencies")
        install = self.calls[-1][0]
        self.assertEqual(graph[4:-1], install[3:install.index("--build=missing")])
        self.assertEqual(self.latest()["state"], "DEPENDENCIES_READY")

    def test_resolve_never_reuses_or_modifies_a_generation_stamp(self):
        self.build("dependencies")
        stamp = self.desktop_target / "conan" / self.cache.name / "xp-generation.json"
        before = stamp.read_bytes()
        self.calls.clear()
        self.graph["graph"]["nodes"]["1"]["binary"] = "Missing"
        self.build("resolve")
        self.assertEqual(stamp.read_bytes(), before)
        self.assertEqual([command[:3] for command, _ in self.calls], [["conan", "graph", "info"]])
        self.assertEqual(self.latest()["state"], "DEPENDENCY_GRAPH_RESOLVED_NOT_BUILT")
        self.assertNotIn("dependencies_reused", self.latest())

    def test_resolve_refuses_wrong_target_abi_and_missing_build_recipe_identity(self):
        self.graph["graph"]["nodes"]["1"]["settings"]["compiler"] = "msvc"
        with self.assertRaisesRegex(ValueError, "GNU ABI"):
            self.build("resolve")
        self.assertEqual(self.latest()["state"], "FAILED")
        self.assertFalse((self.desktop_target / "conan" / self.cache.name / "xp-generation.json").exists())
        self.graph["graph"]["nodes"]["1"]["settings"]["compiler"] = "clang"
        self.graph["graph"]["nodes"]["2"].pop("package_id")
        with self.assertRaisesRegex(ValueError, "build-host package identity"):
            self.build("resolve")
        self.assertFalse((self.repo / "dist").exists())

    def test_msvc_target_dependency_is_rejected_before_configure(self):
        self.graph["graph"]["nodes"]["1"]["settings"]["compiler.runtime"] = "static"
        with self.assertRaisesRegex(ValueError, "GNU ABI"):
            self.build()
        self.assertEqual(self.latest()["state"], "FAILED")
        self.assertFalse(any(command[0] == "cmake" for command, _ in self.calls))
        self.assertFalse((self.desktop_target / "conan" / self.cache.name / "xp-generation.json").exists())

    def test_default_build_is_gui_editor_with_stock_assets_and_no_sdk(self):
        exe = self.build()
        self.assertEqual(exe, self.desktop_exe)
        self.assertFalse(self.legacy_cache.exists())
        report = self.latest()
        self.assertEqual(report["cache"], str(self.desktop_cache))
        self.assertEqual(report["candidate"], str(exe))
        self.assertEqual(report["state"], "BUILT_STATIC_UNVERIFIED")
        self.assertEqual(report["capabilities"]["requested_languages"], ["lua", "c", "cpp", "js"])
        self.assertEqual(report["capabilities"]["gui"], "BUILT_NOT_EXECUTED")
        self.assertEqual(report["capabilities"]["headless"], "NOT_EXECUTED")
        self.assertEqual(report["capabilities"]["shader_profiles"], ["dx11", "glsl", "spv"])
        configure = next(command for command, _ in self.calls if command[:2] == ["cmake", "-S"])
        for arg in ("-DJCE_WINXP_LEGACY=ON", "-DJCE_BUILD_EDITOR=ON", "-DJCE_EDITOR_STANDALONE=ON",
                    "-DJCE_ENABLE_SDK_INSTALL=OFF", "-DJCE_ENABLE_AI_DISPATCH=OFF", "-DJCE_EMBED_ENGINE_SHADERS=ON",
                    "-DJCE_SHADER_PROFILES=dx11;glsl;spv"):
            self.assertIn(arg, configure)
        self.assertFalse((self.repo / "dist").exists())

    def prepare_legacy_cache(self):
        self.build("prepare")
        self.legacy_cache.parent.mkdir(parents=True, exist_ok=True)
        self.desktop_cache.rename(self.legacy_cache)
        self.cache = self.legacy_cache
        self.shader_directory = self.cache / self.shader_directory.relative_to(self.desktop_cache)
        self.stock_metadata["JCE_SHADER_OUTPUT_DIR"] = str(self.shader_directory)

    def test_historical_xp_cache_is_never_reinterpreted_as_new_variant_owner(self):
        self.prepare_legacy_cache()
        old = self.legacy_cache / "preserve.o"
        old.write_bytes(b"keep legacy XP object")
        self.build("prepare")
        self.assertEqual(self.latest()["cache"], str(self.desktop_cache))
        self.assertEqual(old.read_bytes(), b"keep legacy XP object")
        self.assertTrue((self.desktop_cache / "xp-editor-owner.json").is_file())
        self.assertTrue((self.legacy_cache / "xp-editor-owner.json").is_file())

    def test_preexisting_unproved_exe_is_archived_and_rebuilt(self):
        # JCE_XP_CANDIDATE_PROVENANCE_V4_4_1: simulates build, no real XP EXE.
        self.desktop_exe.parent.mkdir(parents=True, exist_ok=True)
        original = b"unowned previous editor bytes"
        self.desktop_exe.write_bytes(original)
        produced = self.build()
        self.assertEqual(produced, self.desktop_exe)
        self.assertNotEqual(produced.read_bytes(), original)
        evidence = self.latest()["archived_preexisting_candidate"]
        self.assertEqual(Path(evidence["backup"]).read_bytes(), original)
        self.assertEqual(evidence["sha256"], hashlib.sha256(original).hexdigest())
        self.assertFalse(evidence["restored_after_failure"])

    def test_unproved_exe_is_restored_on_failed_compilation(self):
        self.desktop_exe.parent.mkdir(parents=True, exist_ok=True)
        original = b"original old candidate"
        self.desktop_exe.write_bytes(original)
        def fail_ninja(command, **kwargs):
            if command[:2] == ["cmake", "--build"]:
                self.desktop_exe.write_bytes(b"partial failed compilation")
                return subprocess.CompletedProcess(command, 1, "", "ninja failed")
            return self.runner(command, **kwargs)
        with self.assertRaises(subprocess.CalledProcessError):
            self.build(runner=fail_ninja)
        self.assertEqual(self.desktop_exe.read_bytes(), original)
        evidence = self.latest()["archived_preexisting_candidate"]
        self.assertTrue(evidence["restored_after_failure"])
        self.assertEqual(Path(evidence["failed_build_output"]).read_bytes(),
                         b"partial failed compilation")
        self.assertEqual(self.latest()["state"], "FAILED")

    def test_configure_does_not_touch_previous_unproved_exe(self):
        self.desktop_exe.parent.mkdir(parents=True, exist_ok=True)
        original = b"previous editor remains through configure"
        self.desktop_exe.write_bytes(original)
        self.build("configure")
        self.assertEqual(self.desktop_exe.read_bytes(), original)
        self.assertNotIn("archived_preexisting_candidate", self.latest())

    def test_second_admitted_build_does_not_archive_owned_exe(self):
        self.build()
        self.build()
        self.assertNotIn("archived_preexisting_candidate", self.latest())

    def test_compatibility_shader_directory_uses_current_catalog_metadata(self):
        try:
            self.build("configure")
        except ValueError as error:
            self.fail("declared compatibility stock output must be admitted: " + str(error))
        policy = self.latest()["capabilities"]["stock_shader_policy"]
        self.assertEqual(policy["name"], "compatibility")
        self.assertEqual(policy["sha256"], self.stock_digest)
        self.assertEqual(policy["directory"], str(self.shader_directory))
        self.assertFalse((self.cache / "generated/engine_assets/shaders").exists())

    def test_stock_policy_requires_all_exported_metadata_fields(self):
        baseline = dict(self.stock_metadata)
        for key in baseline:
            with self.subTest(key=key):
                self.stock_metadata = {name: value for name, value in baseline.items() if name != key}
                with self.assertRaisesRegex(ValueError, "stock shader policy metadata is incomplete"):
                    self.build("configure")
                self.assertFalse(any(command[:2] == ["cmake", "--build"] for command, _ in self.calls))

    def test_stock_policy_requires_full_current_catalog_hash(self):
        self.stock_metadata["JCE_SHADER_STOCK_POLICY_SHA256"] = self.stock_digest[:32] + ("f" * 32)
        with self.assertRaisesRegex(ValueError, "policy hash does not match the current catalog"):
            self.build("configure")
        self.assertFalse(any(command[:2] == ["cmake", "--build"] for command, _ in self.calls))

    def test_stock_policy_refuses_escaping_and_noncanonical_output_paths(self):
        values = (str(self.repo / "foreign/shaders"), "generated/foreign/shaders",
                  str(self.shader_directory / ".." / "shaders"))
        for value in values:
            with self.subTest(directory=value):
                self.stock_metadata["JCE_SHADER_OUTPUT_DIR"] = value
                with self.assertRaises(ValueError):
                    self.build("configure")
                self.assertFalse(any(command[:2] == ["cmake", "--build"] for command, _ in self.calls))

    def test_legacy_stock_directory_cannot_substitute_for_current_policy(self):
        self.shader_directory = self.cache / "generated/engine_assets/shaders"
        self.stock_metadata["JCE_SHADER_OUTPUT_DIR"] = str(self.shader_directory)
        with self.assertRaisesRegex(ValueError, "policy output directory"):
            self.build("configure")
        self.assertTrue(self.shader_directory.is_dir())
        self.assertFalse(any(command[:2] == ["cmake", "--build"] for command, _ in self.calls))

    def test_present_legacy_stock_does_not_fill_a_missing_declared_directory(self):
        declared = self.shader_directory
        self.shader_directory = self.cache / "generated/engine_assets/shaders"
        with self.assertRaisesRegex(ValueError, "policy output directory"):
            self.build("configure")
        self.assertTrue(self.shader_directory.is_dir())
        self.assertFalse(declared.exists())

    def test_stock_catalog_schema_identity_is_checked_even_with_matching_exported_hash(self):
        catalog = json.loads(self.stock_catalog.read_text(encoding="utf-8"))
        catalog["schema"] = 2
        self.stock_catalog.write_text(json.dumps(catalog), encoding="utf-8")
        self.stock_metadata["JCE_SHADER_STOCK_POLICY_SHA256"] = hashlib.sha256(self.stock_catalog.read_bytes()).hexdigest()
        with self.assertRaisesRegex(ValueError, "stock shader catalog identity"):
            self.build("configure")

    def test_stock_output_alias_is_refused_by_existing_ordinary_path_guard(self):
        original = Path.lstat
        def alias(path):
            observed = original(path)
            if path == self.shader_directory:
                return types.SimpleNamespace(st_mode=observed.st_mode, st_file_attributes=0x400)
            return observed
        with mock.patch.object(Path, "lstat", alias):
            with self.assertRaisesRegex(ValueError, "reparse/link"):
                self.build("configure")

    def test_changed_current_stock_catalog_cannot_admit_previous_built_editor(self):
        self.build()
        self.stock_catalog.write_bytes(self.stock_catalog.read_bytes() + b"\n")
        with self.assertRaisesRegex(ValueError, "policy hash does not match the current catalog"):
            self.build("audit")
        self.assertFalse((self.repo / "dist").exists())

    def test_verified_shaderc_pins_configure_environment_over_existing_inherited_override(self):
        inherited = self.repo / "wrong-existing-shaderc.exe"
        inherited.write_bytes(b"unverified host compiler fixture")
        observed = []
        original = self.runner
        def capture(command, **kwargs):
            observed.append((command, dict(kwargs["env"])))
            return original(command, **kwargs)
        self.build("configure", runner=capture, env={"PATH": "explicit-cpu-path",
                   "JCE_SHADERC_EXECUTABLE": str(inherited)})
        dependencies = next(environment for command, environment in observed if command[:2] == ["conan", "install"])
        self.assertEqual(dependencies["JCE_SHADERC_EXECUTABLE"], str(inherited))
        configure, environment = next((command, environment) for command, environment in observed if command[:2] == ["cmake", "-S"])
        verified = str(self.host_tools["shaderc"].resolve())
        self.assertEqual(environment["JCE_SHADERC_EXECUTABLE"], verified)
        self.assertIn("-DJCE_SHADERC_EXECUTABLE=" + verified, configure)
        self.assertEqual(self.latest()["build_host_tools"]["shaderc"]["path"], verified)
        probes = [command for command, _ in observed if command[-1] == "--version"]
        self.assertEqual(probes, [[verified, "--version"]])
        self.assertFalse((self.repo / "dist").exists())

    def test_existing_host_tool_usage_error_is_retained_as_launch_only(self):
        original = self.runner
        def usage(command, **kwargs):
            if len(command) == 2 and command[1] == "--help":
                self.calls.append((command, kwargs))
                return subprocess.CompletedProcess(command, 1, "usage\n", "unknown arg: --help\n")
            return original(command, **kwargs)
        self.build("configure", runner=usage)
        receipt = self.latest()
        tools = [stage for stage in receipt["stages"] if stage["stage"].startswith("verify-host-")]
        self.assertEqual([stage["returncode"] for stage in tools], [1, 1])
        self.assertTrue(all(stage["accepted_returncodes"] == [0, 1] for stage in tools))
        self.assertEqual(receipt["build_host_tools"]["host-pak"]["scope"], "launch only")

    def test_failed_native_stage_retains_bounded_stdout_and_stderr(self):
        def fail(command, **kwargs):
            return subprocess.CompletedProcess(command, 37, "original stdout\n" * 30, "original stderr\n")
        with mock.patch.object(XP, "LOG_LIMIT", 64):
            with self.assertRaises(subprocess.CalledProcessError):
                self.build("dependencies", runner=fail)
        stage = self.latest()["stages"][-1]
        self.assertEqual(stage["returncode"], 37)
        self.assertEqual(stage["state"], "FAILED")
        self.assertTrue(stage["stdout"]["truncated"])
        self.assertEqual(Path(stage["stdout"]["path"]).stat().st_size, 64)
        self.assertEqual(Path(stage["stderr"]["path"]).read_text(), "original stderr\n")

    def test_explicit_bx_diagnostic_audit_cannot_publish_a_default_shipment(self):
        exe = self.build(bgfx_bx_debug="on")
        inventory = self.repo / "guest-exports.json"
        inventory.write_text(json.dumps(guest_inventory()), encoding="utf-8")
        payload = {"embedded_assets": {"entries": 4}, "embedded_notices": {"LICENSE": "a" * 64}}
        with mock.patch.object(XP, "_verify_payload", return_value=payload):
            with mock.patch.object(XP, "_publish", side_effect=AssertionError("diagnostic attempted publication")):
                result = self.build("audit", export_inventory=inventory, bgfx_bx_debug="on")
        self.assertEqual(result, exe)
        self.assertFalse((self.repo / "dist").exists())
        self.assertEqual(self.latest()["state"], "DIAGNOSTIC_STATIC_AUDIT_PASSED")
        self.assertIsNone(self.latest()["final_artifact"])

    def test_audit_never_publishes_missing_payload_or_unproved_imports(self):
        self.build()
        with self.assertRaisesRegex(ValueError, "asset pack"):
            self.build("audit")
        self.assertFalse((self.repo / "dist").exists())
        with mock.patch.object(XP, "_verify_payload", return_value={"embedded_assets": {"entries": 4},
                                                                  "embedded_notices": {"LICENSE": "a" * 64}}):
            self.assertIsNone(self.build("audit"))
        self.assertEqual(self.latest()["state"], "STATIC_UNVERIFIED")
        self.assertIsNone(self.latest()["final_artifact"])
        self.assertFalse((self.repo / "dist").exists())

    def test_only_successful_full_static_admission_publishes_exact_single_exe(self):
        self.build()
        inventory = self.repo / "guest-exports.json"
        inventory.write_text(json.dumps(guest_inventory()), encoding="utf-8")
        payload = {"embedded_assets": {"entries": 4}, "embedded_notices": {"LICENSE": "a" * 64}}
        with mock.patch.object(XP, "_verify_payload", return_value=payload):
            output = self.build("audit", export_inventory=inventory)
        self.assertEqual(output.name, "JCE-editor-windows-x64-xp.exe")
        self.assertEqual(list(output.parent.iterdir()), [output])
        self.assertEqual(output.read_bytes(), self.desktop_exe.read_bytes())
        self.assertEqual(self.latest()["state"], "STATIC_AUDIT_PASSED")
        self.assertEqual(self.latest()["runtime_execution"], "NOT_EXECUTED")
        with self.assertRaisesRegex(ValueError, "unqualified"):
            XP._publish(output, output.parent / "bad.exe", {"state": "STATIC_UNVERIFIED"})
        with self.assertRaisesRegex(ValueError, "assets and notices"):
            XP._publish(output, output.parent / "bad.exe", {"state": "STATIC_AUDIT_PASSED"})

    def test_unowned_home_and_cache_refuse_without_default_home_mutation(self):
        (self.home / ".jce-toolchain-overlay.json").unlink()
        with self.assertRaisesRegex(ValueError, "unowned Conan configuration"):
            self.build("dependencies")
        self.assertEqual(self.calls, [])

    def test_unowned_existing_cache_is_never_adopted(self):
        self.cache.mkdir(parents=True)
        retained = self.cache / "unknown.o"
        retained.write_bytes(b"do not erase")
        with self.assertRaisesRegex(ValueError, "nonempty unowned"):
            self.build("prepare")
        self.assertEqual(retained.read_bytes(), b"do not erase")

    def test_changed_post_build_configuration_cannot_bless_an_old_executable(self):
        self.build()
        with (self.cache / "CMakeCache.txt").open("a", encoding="utf-8") as stream:
            stream.write("unrelated:BOOL=ON\n")
        with self.assertRaisesRegex(ValueError, "built Editor identity"):
            self.build("audit")
        self.assertFalse((self.repo / "dist").exists())

    def test_actual_payload_owner_requires_stock_fonts_ui_i18n_and_shaders(self):
        self.cache.mkdir(parents=True)
        (self.cache / "reports").mkdir()
        exe = self.cache / "test.exe"
        pack = b"synthetic verified-pack boundary, not a real native Archive"
        exe.write_bytes(editor_pe() + pack)
        (self.cache / "editor_assets.pak").write_bytes(pack)
        standalone = XP._module("standalone")
        original = XP._module
        # Archive virtual keys are canonical ASCII lowercase, unlike source filenames.
        required = ["i18n/en.json", "fonts/aileron-regular.ttf", "engine_settings.rml",
                    "fonts/jce-cjk-fallback.ttf", "fonts/jce-cjk-fallback-ofl.txt",
                    "fonts/jce-cjk-fallback-provenance.json",
                    "shaders/vs_imgui_dx11.bin", "shaders/fs_imgui_dx11.bin",
                    "shaders/vs_pbr_dx11.bin", "shaders/fs_pbr_dx11.bin",
                    "shaders/vs_imgui_glsl.bin", "shaders/fs_imgui_glsl.bin",
                    "shaders/vs_pbr_glsl.bin", "shaders/fs_pbr_glsl.bin",
                    "shaders/vs_imgui_spv.bin", "shaders/fs_imgui_spv.bin",
                    "shaders/vs_pbr_spv.bin", "shaders/fs_pbr_spv.bin"]
        with mock.patch.object(standalone, "verify_notices", return_value={"LICENSE": "a" * 64}), mock.patch.object(
                XP, "_module", side_effect=lambda name: standalone if name == "standalone" else original(name)):
            for missing in (None, *required):
                with self.subTest(missing=missing):
                    paths = [path for path in required if path != missing]
                    entries = [{"path": path, "original_size": 1, "stored_size": 1, "verified": True} for path in paths]
                    inventory = {"entries": entries, "file_size": len(pack), "totals": {
                        "entries": len(entries), "verified_count": len(entries), "corrupt_count": 0,
                        "original_size": len(entries), "stored_size": len(entries)}}
                    (self.cache / "reports/editor_assets_bom.json").write_text(json.dumps(inventory), encoding="utf-8")
                    if missing is None:
                        self.assertEqual(XP._verify_payload(exe, self.cache, self.repo)["embedded_assets"]["entries"],
                                         len(required))
                    else:
                        with self.assertRaisesRegex(ValueError, "required stock Editor"):
                            XP._verify_payload(exe, self.cache, self.repo)

    def test_cli_explicit_inputs_use_one_owner(self):
        parser = argparse.ArgumentParser()
        XP.add_arguments(parser)
        args = parser.parse_args(["--arch", "x86", "--phase", "prepare", "--toolchain-root", str(self.toolchain)])
        self.assertEqual(args.arch, "x86")
        self.assertEqual(args.jobs, 1)
        resolved = parser.parse_args(["--phase", "resolve", "--toolchain-root", str(self.toolchain)])
        self.assertEqual(resolved.phase, "resolve")
        with mock.patch.object(XP, "build") as build:
            self.assertEqual(XP.main(["--phase", "prepare", "--toolchain-root", str(self.toolchain)]), 0)
        self.assertEqual(build.call_args.kwargs["phase"], "prepare")
        self.assertEqual(build.call_args.args[2], self.toolchain)
        with mock.patch.object(XP, "build", return_value=None):
            self.assertEqual(XP.main(["--phase", "audit", "--toolchain-root", str(self.toolchain)]), 2)


if __name__ == "__main__":
    unittest.main()
