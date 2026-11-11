"""Negative controls for native single-file delivery; runs without MSVC."""
import importlib.util
from contextlib import contextmanager, redirect_stderr
import io
import json
import os
import re
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("standalone_test", ROOT / "tools/build/standalone.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def pe_fixture(imports, delayed=(), *, functions=None, delayed_functions=None,
               machine=0x8664, legacy_delay=False, subsystem=2):
    """Synthetic mapped PE with real descriptor, thunk and hint/name layouts."""
    is_64 = machine != 0x14c
    pointer = 8 if is_64 else 4
    optional_size = 240 if is_64 else 224
    directory_offset = 112 if is_64 else 96
    image_base = 0x140000000 if is_64 else 0x400000
    blob = bytearray(4096)
    blob[:2] = b"MZ"
    struct.pack_into("<I", blob, 0x3c, 0x80)
    blob[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", blob, 0x84, machine, 1)
    struct.pack_into("<H", blob, 0x94, optional_size)
    struct.pack_into("<H", blob, 0x98, 0x20b if is_64 else 0x10b)
    struct.pack_into("<H", blob, 0x98 + 68, subsystem)
    struct.pack_into("<Q" if is_64 else "<I", blob, 0x98 + (24 if is_64 else 28), image_base)
    struct.pack_into("<I", blob, 0x98 + directory_offset - 4, 16)
    section = 0x98 + optional_size
    struct.pack_into("<IIII", blob, section + 8, 3584, 0x1000, 3584, 512)
    cursor, thunk_cursor = 2048, 1280
    defaults = {"kernel32.dll": "GetProcAddress", "user32.dll": "MessageBoxW",
                "d3d11.dll": "D3D11CreateDevice"}
    for directory, offset, width, names, selected in (
            (1, 512, 20, imports, functions or {}),
            (13, 1024, 32, delayed, delayed_functions or {})):
        if not names:
            continue
        struct.pack_into("<II", blob, 0x98 + directory_offset + directory * 8,
                         offset - 512 + 0x1000, (len(names) + 1) * width)
        for i, name in enumerate(names):
            encoded = name.encode("ascii") + b"\0"
            blob[cursor:cursor + len(encoded)] = encoded
            descriptor = offset + i * width
            name_rva = cursor - 512 + 0x1000
            cursor += len(encoded)
            thunk_rva = thunk_cursor - 512 + 0x1000
            values = selected.get(name, [defaults.get(name.lower(), "FixtureEntry")])
            for symbol in values:
                if isinstance(symbol, int):
                    value = (1 << (pointer * 8 - 1)) | symbol
                else:
                    value = cursor - 512 + 0x1000
                    encoded = b"\0\0" + symbol.encode("ascii") + b"\0"
                    blob[cursor:cursor + len(encoded)] = encoded
                    cursor += len(encoded)
                    if directory == 13 and legacy_delay:
                        value += image_base
                struct.pack_into("<Q" if is_64 else "<I", blob, thunk_cursor, value)
                thunk_cursor += pointer
            struct.pack_into("<Q" if is_64 else "<I", blob, thunk_cursor, 0)
            thunk_cursor += pointer
            if directory == 1:
                struct.pack_into("<IIIII", blob, descriptor, thunk_rva, 0, 0, name_rva, thunk_rva)
            else:
                bias = image_base if legacy_delay else 0
                struct.pack_into("<IIIII", blob, descriptor, 0 if legacy_delay else 1,
                                 name_rva + bias, 0, thunk_rva + bias, thunk_rva + bias)
    return blob


def native_cache_fixture(root, target, variant="dist", *, legacy=False, **changes):
    target_root = root / "build/desktop" / target["key"]
    # JCE_EDITOR_VARIANT_LAYOUT_V4: fixture supports historical caches only when explicitly requested.
    if legacy:
        build = target_root.with_name(target_root.name + "-" + legacy)
        conan = build.with_name(build.name + "-conan")
    else:
        build, conan = target_root / "obj" / variant, target_root / "conan" / variant
    toolchain = conan / "build/Release/generators/conan_toolchain.cmake"
    toolchain.parent.mkdir(parents=True, exist_ok=True)
    toolchain.write_text('set(CMAKE_MSVC_RUNTIME_LIBRARY "$<$<CONFIG:Release>:MultiThreaded>")\n', encoding="utf-8")
    compiler = build / "CMakeFiles/3.27.9/CMakeCCompiler.cmake"
    compiler.parent.mkdir(parents=True, exist_ok=True)
    compiler.write_text('set(CMAKE_C_COMPILER_ID "MSVC")\nset(CMAKE_C_COMPILER_ARCHITECTURE_ID x64)\n', encoding="utf-8")
    values = dict(CMAKE_HOME_DIRECTORY=str(root), CMAKE_CACHEFILE_DIR=str(build),
        CMAKE_TOOLCHAIN_FILE=str(toolchain), CMAKE_BUILD_TYPE="Release", JCE_BUILD_VARIANT=variant,
        JCE_DELIVERY_PROFILE="embedded", JCE_EDITOR_STANDALONE="ON", JCE_BUILD_EDITOR="ON",
        CMAKE_CACHE_MAJOR_VERSION="3", CMAKE_CACHE_MINOR_VERSION="27", CMAKE_CACHE_PATCH_VERSION="9",
        JCE_ENABLE_PROFILING="OFF", JCE_BUILD_SCRIPT_VM_PYTHON="OFF", JCE_BUILD_SCRIPT_JAVA="OFF",
        JCE_BUILD_SCRIPT_CSHARP="OFF", JCE_EMBED_ENGINE_SHADERS="ON")
    values.update(changes)
    (build / "CMakeCache.txt").write_text("\n".join(key + ":STRING=" + value
        for key, value in values.items()) + "\n", encoding="utf-8")
    return build, toolchain


class CacheCleanSourceProtectionTests(unittest.TestCase):
    target = {"key": "fixture", "host": "windows", "arch": "x86_64", "cross": False}

    def driver(self, root):
        return SimpleNamespace(ROOT=root, HOST="windows", DRY_RUN=False, host_arch=lambda: "x86_64",
            die=Mock(side_effect=ValueError),
            bgfx_graphics_conan_args=lambda env: [], run=Mock(), export_conan_recipes=Mock(),
            sync_conan_hooks=Mock(side_effect=ValueError("stop after clean")))

    def project(self, parent):
        source = parent / "release/arbitrary project name"
        (source / "src").mkdir(parents=True)
        (source / "jce_project.json").write_bytes(b"{malformed but owned project")
        (source / "CMakeLists.txt").write_bytes(b"# KEEP_PROJECT_BUILD_SOURCE\n")
        (source / "src/owned.c").write_bytes(b"KEEP_USER_SOURCE")
        return source

    def assert_native_preserves(self, owner, root, source):
        driver = self.driver(root)
        real_remove = shutil.rmtree
        with patch.object(owner.shutil, "rmtree", wraps=real_remove) as remove:
            with self.assertRaises(ValueError):
                owner._configure(driver, SimpleNamespace(variant="dist", clean=True), self.target, {})
            self.assertTrue((source / "src/owned.c").is_file(), "cache cleanup deleted nested project source")
            self.assertEqual((source / "src/owned.c").read_bytes(), b"KEEP_USER_SOURCE")
            self.assertEqual((source / "jce_project.json").read_bytes(), b"{malformed but owned project")
            self.assertEqual((source / "CMakeLists.txt").read_bytes(), b"# KEEP_PROJECT_BUILD_SOURCE\n")
            remove.assert_not_called()
            driver.sync_conan_hooks.assert_not_called()
            driver.export_conan_recipes.assert_not_called()
            driver.run.assert_not_called()

    def test_canonical_nested_project_refuses_before_either_cache_deletion(self):
        for second_root in (False, True):
            with self.subTest(second_root=second_root), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                build, toolchain = native_cache_fixture(root, self.target)
                source = self.project(build.parent.parent / "conan" / build.name if second_root else build)
                self.assert_native_preserves(MODULE, root, source)
                self.assertTrue((build / "CMakeCache.txt").is_file())
                self.assertTrue(toolchain.is_file())

    def test_omitted_guard_fails_the_same_executed_source_preservation_assertion(self):
        path = ROOT / "tools/build/standalone.py"
        source = path.read_text(encoding="utf-8")
        wire = '        guard_cache_clean((build, conan), driver.ROOT / "build/desktop")'
        self.assertEqual(source.count(wire), 1)
        spec = importlib.util.spec_from_file_location("cache_clean_missing_source_guard", path)
        old = importlib.util.module_from_spec(spec)
        exec(compile(source.replace(wire, "        pass # old unchecked recursive clean"), str(path), "exec"), old.__dict__)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            build, _ = native_cache_fixture(root, self.target)
            project = self.project(build)
            self.assert_native_preserves(MODULE, root, project)
            with self.assertRaisesRegex(AssertionError, "deleted nested project source"):
                self.assert_native_preserves(old, root, project)

    def test_generated_cmake_scratch_remains_cleanable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            build, toolchain = native_cache_fixture(root, self.target)
            scratch = build / "CMakeFiles/scratch/CMakeLists.txt"
            scratch.parent.mkdir()
            scratch.write_text("# generated compiler scratch\n")
            driver = self.driver(root)
            real_remove = shutil.rmtree
            with patch.object(MODULE.shutil, "rmtree", wraps=real_remove) as remove:
                with self.assertRaisesRegex(ValueError, "stop after clean"):
                    MODULE._configure(driver, SimpleNamespace(variant="dist", clean=True), self.target, {})
                self.assertEqual([call.args[0] for call in remove.call_args_list],
                                 [build, build.parent.parent / "conan" / build.name])
            self.assertFalse(build.exists())
            self.assertFalse(toolchain.exists())
            driver.sync_conan_hooks.assert_called_once_with({})
            driver.run.assert_not_called()

    def test_unreadable_directory_and_scan_budgets_preserve_both_roots(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            build, _ = native_cache_fixture(root, self.target)
            source = self.project(build)
            unreadable = build / "unreadable"
            unreadable.mkdir()
            (source / "jce_project.json").unlink()
            real_scan = MODULE.os.scandir
            denied = Mock()
            def scan(path):
                if Path(path) == unreadable:
                    denied()
                    raise PermissionError("counted unreadable directory")
                return real_scan(path)
            driver = self.driver(root)
            with patch.object(MODULE.os, "scandir", side_effect=scan), \
                 patch.object(MODULE.shutil, "rmtree") as remove:
                with self.assertRaisesRegex(ValueError, "cannot inspect source ownership"):
                    MODULE._configure(driver, SimpleNamespace(variant="dist", clean=True), self.target, {})
                denied.assert_called_once_with()
                remove.assert_not_called()
                driver.sync_conan_hooks.assert_not_called()
                driver.run.assert_not_called()
            self.assertEqual((source / "src/owned.c").read_bytes(), b"KEEP_USER_SOURCE")
            # Without a marker, exhausted scans still refuse every deletion.
            conan = build.parent.parent / "conan" / build.name
            for budgets in ({"entry_budget": 1}, {"depth_budget": 0}):
                with self.subTest(budgets=budgets), self.assertRaisesRegex(ValueError, "budget exhausted"):
                    MODULE.guard_cache_clean((build, conan), root / "build/desktop", **budgets)
                self.assertEqual((source / "src/owned.c").read_bytes(), b"KEEP_USER_SOURCE")
                self.assertTrue(conan.is_dir())

    def test_file_and_directory_link_targets_are_never_traversed_or_deleted(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            cache = root / "build/desktop/generated"
            cache.mkdir(parents=True)
            outside = root / "keep-target"
            outside.mkdir()
            target = outside / "library.so.1"
            target.write_bytes(b"KEEP_LINK_TARGET")
            (outside / "jce_project.json").write_bytes(b"KEEP_EXTERNAL_PROJECT")
            try:
                (cache / "libjce.so").symlink_to(target)
            except OSError as error:
                self.skipTest("host cannot create generated file symlinks: " + str(error))
            MODULE.guard_cache_clean((cache,), root / "build/desktop")
            directory_link = cache / "external-directory"
            directory_link.symlink_to(outside, target_is_directory=True)
            if os.name == "nt":
                with self.assertRaisesRegex(ValueError, "reparse"):
                    MODULE.guard_cache_clean((cache,), root / "build/desktop")
            else:
                MODULE.guard_cache_clean((cache,), root / "build/desktop")
            shutil.rmtree(cache)
            self.assertEqual(target.read_bytes(), b"KEEP_LINK_TARGET")
            self.assertEqual((outside / "jce_project.json").read_bytes(), b"KEEP_EXTERNAL_PROJECT")

    def test_cleanup_roots_and_original_alias_spelling_are_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            allowed = root / "build/desktop"
            cache = allowed / "generated"
            cache.mkdir(parents=True)
            for path in (allowed, root, allowed / "../desktop/generated"):
                with self.subTest(path=path), self.assertRaisesRegex(ValueError, "strict child"):
                    MODULE.guard_cache_clean((path,), allowed)
            alias = allowed / "alias"
            try:
                alias.symlink_to(cache, target_is_directory=True)
            except OSError as error:
                self.skipTest("host cannot create a root alias: " + str(error))
            with self.assertRaisesRegex(ValueError, "aliased/non-directory"):
                MODULE.guard_cache_clean((alias,), allowed)
            root_alias = root / "allowed-root-alias"
            root_alias.symlink_to(allowed, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "aliased/non-directory"):
                MODULE.guard_cache_clean((cache,), root_alias)
            self.assertTrue(cache.is_dir())

    @unittest.skipUnless(hasattr(os, "mkfifo"), "POSIX FIFO creation unavailable")
    def test_fifo_is_not_opened_or_traversed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            cache = root / "build/desktop/generated"
            cache.mkdir(parents=True)
            os.mkfifo(cache / "generated.pipe")
            MODULE.guard_cache_clean((cache,), root / "build/desktop")
            shutil.rmtree(cache)
            self.assertFalse(cache.exists())

    def test_authoring_editor_and_sdk_clean_use_the_same_source_guard(self):
        spec = importlib.util.spec_from_file_location("cache_clean_authoring_dispatch", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            build = root / "build/desktop/fixture/authoring/obj/release"
            source = self.project(build)
            target = dict(self.target, preset_stem="fixture")
            args = SimpleNamespace(arch=None, variant="release", clean=True, delivery_profile="authoring")
            with patch.object(driver, "ROOT", root), patch.object(driver, "HOST", "windows"), \
                 patch.object(driver, "DRY_RUN", False), \
                 patch.object(driver, "resolve_target", return_value=target), \
                 patch.object(driver, "preset_binary_dir", return_value=build), \
                 patch.object(driver, "conan_dir", return_value=build.with_name(build.name + "-conan")), \
                 patch.object(driver, "_standalone_module", return_value=MODULE), \
                 patch.object(driver, "msvc_env", return_value={}), \
                 patch.object(driver, "conan_install") as install, \
                 patch.object(driver, "run") as run, \
                 patch.object(driver.shutil, "rmtree") as remove:
                with self.assertRaisesRegex(ValueError, "nested project source"):
                    driver.cmd_editor.__wrapped__(args)
                with self.assertRaisesRegex(ValueError, "nested project source"):
                    driver._sdk_one.__wrapped__(target, "release", "Release", True)
                install.assert_not_called()
                run.assert_not_called()
                remove.assert_not_called()
            self.assertEqual((source / "src/owned.c").read_bytes(), b"KEEP_USER_SOURCE")


class NativeCacheSelectionTests(unittest.TestCase):
    target = {"key": "fixture", "host": "windows", "arch": "x86_64", "cross": False}

    def test_canonical_layout_never_adopts_legacy_objects_or_lease_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            driver = SimpleNamespace(ROOT=root)
            args = SimpleNamespace(variant="dist")
            target_root = root / "build/desktop/fixture"
            expected = MODULE.native_cache_plan(driver, args, self.target)
            self.assertEqual(expected["build"], target_root / "obj/dist")
            self.assertEqual(expected["conan"], target_root / "conan/dist")
            old_standalone, _ = native_cache_fixture(root, self.target,
                variant="dist", legacy="standalone")
            old_release, _ = native_cache_fixture(root, self.target,
                variant="release", legacy="release")
            self.assertEqual(MODULE.native_cache_plan(driver, args, self.target), expected)
            self.assertFalse(expected["build"].exists())
            with self.assertRaisesRegex(ValueError, "cache is missing"):
                MODULE.native_cache_plan(driver, args, self.target, exact_variant=True)
            args._native_cache_plan = {**expected, "build": old_standalone}
            with self.assertRaisesRegex(ValueError, "identity changed"):
                MODULE.native_cache_plan(driver, args, self.target)
            del args._native_cache_plan
            canonical, _ = native_cache_fixture(root, self.target, variant="release")
            self.assertNotEqual(canonical, expected["build"])
            self.assertEqual(MODULE.native_cache_plan(driver, args, self.target), expected)
            args.variant = "release"
            self.assertEqual(MODULE.native_cache_plan(driver, args, self.target,
                exact_variant=True)["build"], canonical)
            self.assertTrue((old_standalone / "CMakeCache.txt").is_file())
            self.assertTrue((old_release / "CMakeCache.txt").is_file())

    def test_foreign_static_runtime_architecture_and_shipping_flags_refuse_reuse(self):
        for field, value in (("JCE_DELIVERY_PROFILE", "authoring"), ("CMAKE_BUILD_TYPE", "Debug"),
                ("JCE_ENABLE_PROFILING", "ON"), ("JCE_BUILD_SCRIPT_VM_PYTHON", "ON"),
                ("JCE_EMBED_ENGINE_SHADERS", "OFF"), ("CMAKE_HOME_DIRECTORY", "foreign"),
                ("CMAKE_TOOLCHAIN_FILE", "foreign")):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                native_cache_fixture(root, self.target, **{field: value})
                with self.assertRaisesRegex(ValueError, "cache identity mismatch"):
                    MODULE.native_cache_plan(SimpleNamespace(ROOT=root), SimpleNamespace(variant="dist"), self.target)
        for mutation in ("dynamic_crt", "wrong_arch", "missing_compiler", "canonical_collision"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                build, toolchain = native_cache_fixture(root, self.target)
                if mutation == "dynamic_crt":
                    toolchain.write_text('set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")\n')
                elif mutation == "wrong_arch":
                    compiler = build / "CMakeFiles/3.27.9/CMakeCCompiler.cmake"
                    compiler.write_text(compiler.read_text().replace("ARCHITECTURE_ID x64", "ARCHITECTURE_ID ARM64"))
                elif mutation == "missing_compiler":
                    (build / "CMakeFiles/3.27.9/CMakeCCompiler.cmake").unlink()
                else:
                    native_cache_fixture(root, self.target, legacy=False, JCE_DELIVERY_PROFILE="authoring")
                with self.assertRaises(ValueError):
                    MODULE.native_cache_plan(SimpleNamespace(ROOT=root), SimpleNamespace(variant="dist"), self.target)

    def test_retired_experiment_cache_refuses_build_and_package_reuse(self):
        for value in (None, "OFF", "ON"):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                settings = {} if value is None else {"JCE_BUILD_CSM_PCF_DIAGNOSTIC": value}
                build, _ = native_cache_fixture(root, self.target, **settings)
                driver, args = SimpleNamespace(ROOT=root), SimpleNamespace(variant="dist")
                for exact_variant in (False, True):
                    if value == "ON":
                        with self.assertRaisesRegex(ValueError, "cache contains a retired experiment"):
                            MODULE.native_cache_plan(driver, args, self.target, exact_variant=exact_variant)
                    else:
                        plan = MODULE.native_cache_plan(driver, args, self.target, exact_variant=exact_variant)
                        self.assertEqual(plan["build"], build)

    def test_remembered_lease_paths_stay_canonical_and_revalidate_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            canonical, _ = native_cache_fixture(root, self.target)
            driver, args = SimpleNamespace(ROOT=root), SimpleNamespace(variant="dist")
            args._native_cache_plan = MODULE.native_cache_plan(driver, args, self.target)
            native_cache_fixture(root, self.target, legacy="standalone",
                                 JCE_ENABLE_PROFILING="ON")
            self.assertEqual(MODULE.native_cache_plan(driver, args, self.target)["build"], canonical)
            native_cache_fixture(root, self.target, JCE_ENABLE_PROFILING="ON")
            with self.assertRaisesRegex(ValueError, "JCE_ENABLE_PROFILING"):
                MODULE.native_cache_plan(driver, args, self.target)

    def test_active_cmake_version_admits_retained_older_compiler_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            build, _ = native_cache_fixture(root, self.target)
            old = build / "CMakeFiles/3.25.0/CMakeCCompiler.cmake"
            old.parent.mkdir(parents=True)
            old.write_text('set(CMAKE_C_COMPILER_ID "MSVC")\nset(CMAKE_C_COMPILER_ARCHITECTURE_ID ARM64)\n')
            args, driver = SimpleNamespace(variant="dist"), SimpleNamespace(ROOT=root)
            self.assertEqual(MODULE.native_cache_plan(driver, args, self.target)["build"], build)
            native_cache_fixture(root, self.target, CMAKE_CACHE_MINOR_VERSION="25", CMAKE_CACHE_PATCH_VERSION="0")
            with self.assertRaisesRegex(ValueError, "compiler/architecture"):
                MODULE.native_cache_plan(driver, args, self.target)

    def test_omitted_cache_admission_wire_fails_the_same_runtime_refusal(self):
        path = ROOT / "tools/build/standalone.py"
        source = path.read_text(encoding="utf-8")
        wire = "    _admit_cache(plan, target, exact_variant=exact_variant)"
        self.assertEqual(source.count(wire), 1)
        spec = importlib.util.spec_from_file_location("native_cache_admission_omission", path)
        old = importlib.util.module_from_spec(spec)
        exec(compile(source.replace(wire, "    pass # old presence-only cache reuse"), str(path), "exec"), old.__dict__)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            _, toolchain = native_cache_fixture(root, self.target)
            toolchain.write_text('set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")\n')
            def require_refusal(owner):
                with self.assertRaisesRegex(ValueError, "static MSVC runtime"):
                    owner.native_cache_plan(SimpleNamespace(ROOT=root), SimpleNamespace(variant="dist"), self.target)
            require_refusal(MODULE)
            with self.assertRaises(AssertionError):
                require_refusal(old)

    def test_editor_gui_policy_does_not_reject_console_sdk_tools(self):
        with tempfile.TemporaryDirectory() as temporary:
            exe = Path(temporary) / "editor.exe"
            data = pe_fixture(["KERNEL32.dll"])
            struct.pack_into("<H", data, 0x98 + 68, 3)
            exe.write_bytes(data)
            self.assertEqual(MODULE.verify(exe)["pe_subsystem"], 3)
            with self.assertRaisesRegex(ValueError, "GUI executable"):
                MODULE.verify(exe, require_gui=True)


class NativeEditorDispatchTests(unittest.TestCase):
    def driver(self):
        spec = importlib.util.spec_from_file_location("single_default_dispatch", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        return driver

    def test_default_editor_package_and_legacy_alias_select_same_embedded_owner(self):
        driver = self.driver()
        parser = driver.build_parser()
        owner = Mock()
        target = NativeCacheSelectionTests.target
        with patch.dict(driver.sys.modules, {driver.__name__: driver}), \
             patch.object(driver, "DRY_RUN", True), \
             patch.object(driver, "resolve_target", return_value=target), \
             patch.object(driver, "msvc_env", return_value={}), \
             patch.object(driver, "HOST", target["host"]), \
             patch.object(driver, "_standalone_module", return_value=owner):
            for options in ([], ["--standalone"], ["--delivery-profile", "authoring", "--standalone"]):
                args = parser.parse_args(["editor", *options])
                self.assertEqual(driver._delivery_module().profile(args), "embedded")
                args.func(args)
                owner.build_editor.assert_called_with(driver, args, target, {})
                package = parser.parse_args(["package", "editor", *options])
                package.func(package)
                owner.package_editor.assert_called_with(driver, package)
            authoring = parser.parse_args(["editor", "--delivery-profile", "authoring", "--variant", "asan"])
            owner.build_editor.reset_mock()
            with patch.object(driver, "_editor_asan") as developer:
                authoring.func(authoring)
                developer.assert_called_once_with(target, {})
            owner.build_editor.assert_not_called()

    def test_actual_default_package_revalidates_cache_after_lease_before_publication(self):
        driver = self.driver()
        parser = driver.build_parser()
        target = NativeCacheSelectionTests.target
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            build, _ = native_cache_fixture(root, target)
            args = parser.parse_args(["package", "editor", "--variant", "dist", "--skip-build"])
            events = []

            @contextmanager
            def lease(paths):
                self.assertIn(build, paths)
                self.assertIn(build.parent.parent / "conan" / build.name, paths)
                events.append("acquired")
                native_cache_fixture(root, target, JCE_ENABLE_PROFILING="ON")
                try:
                    yield
                finally:
                    events.append("released")

            with patch.dict(driver.sys.modules, {driver.__name__: driver}), \
                 patch.object(driver, "ROOT", root), patch.object(driver, "HOST", "windows"), \
                 patch.object(driver, "DRY_RUN", False), \
                 patch.object(driver, "resolve_target", return_value=target), \
                 patch.object(driver, "_cache_scope", side_effect=lease), \
                 patch.object(driver, "msvc_env") as environment, \
                 patch.object(driver, "run") as run:
                with self.assertRaisesRegex(ValueError, "JCE_ENABLE_PROFILING"):
                    args.func(args)
                environment.assert_not_called()
                run.assert_not_called()
            self.assertEqual(events, ["acquired", "released"])
            self.assertFalse((root / "dist/editor/JCE-editor-fixture.exe").exists())

    def test_package_leases_cached_override_and_final_output_and_refuses_changed_override(self):
        driver = self.driver()
        parser = driver.build_parser()
        target = NativeCacheSelectionTests.target
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            output = root / "artifact override"
            native_cache_fixture(root, target, JCE_EDITOR_ARTIFACT_DIR=str(output))
            args = parser.parse_args(["package", "editor", "--variant", "dist", "--skip-build"])
            final = root / "dist/editor/JCE-editor-fixture.exe"

            @contextmanager
            def lease(paths):
                self.assertIn(output, paths)
                self.assertIn(final.parent, paths)
                self.assertIn(final, paths)
                native_cache_fixture(root, target, JCE_EDITOR_ARTIFACT_DIR=str(root / "changed output"))
                yield

            with patch.dict(driver.sys.modules, {driver.__name__: driver}), \
                 patch.object(driver, "ROOT", root), patch.object(driver, "HOST", "windows"), \
                 patch.object(driver, "DRY_RUN", False), \
                 patch.object(driver, "resolve_target", return_value=target), \
                 patch.object(driver, "_cache_scope", side_effect=lease), \
                 patch.object(MODULE, "verify") as verify:
                with self.assertRaisesRegex(ValueError, "output changed"):
                    args.func(args)
                verify.assert_not_called()
            self.assertFalse(final.exists())


class NativeShadercSelectionTests(unittest.TestCase):
    @contextmanager
    def fixture(self, *, owner=MODULE):
        base = ROOT / "build/tests/standalone-shaderc"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            root = Path(temporary).resolve()
            target = dict(NativeCacheSelectionTests.target, profile="windows-x64")
            cached, automatic = root / "cached/shaderc.exe", root / "automatic/shaderc.exe"
            for path in (cached, automatic):
                path.parent.mkdir(parents=True)
                path.write_bytes(b"counted host-tool port, not an executable")
            build, _ = native_cache_fixture(root, target, legacy=False,
                                            JCE_SHADERC_EXECUTABLE=str(cached))
            (build / "generated/engine_assets/shaders").mkdir(parents=True)
            spec = importlib.util.spec_from_file_location("shaderc_selection_driver", ROOT / "tools/build/jce.py")
            driver = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(driver)
            commands, probes, resolutions = [], [], []
            rejected = set()

            def run(command, **kwargs):
                commands.append((command, kwargs))

            def probe(command, **kwargs):
                self.assertEqual(command[1:], ["--version"])
                path = Path(command[0])
                probes.append(path)
                return SimpleNamespace(returncode=1 if path in rejected else 0)

            def resolve():
                resolutions.append("automatic")
                return automatic

            args = SimpleNamespace(variant="dist", clean=False)
            with patch.object(driver, "ROOT", root), patch.object(driver, "HOST", "windows"), \
                 patch.object(driver, "DRY_RUN", False), \
                 patch.object(driver, "sync_conan_hooks"), patch.object(driver, "export_conan_recipes"), \
                 patch.object(driver, "find_vs_install", return_value=None), \
                 patch.object(driver, "host_build_profile", return_value="windows-x64"), \
                 patch.object(driver, "run", side_effect=run), patch.object(driver, "log"), \
                 patch.object(driver, "find_host_shaderc", side_effect=resolve), \
                 patch.object(driver.subprocess, "run", side_effect=probe):
                yield SimpleNamespace(root=root, target=target, build=build, cached=cached,
                    automatic=automatic, driver=driver, args=args, commands=commands,
                    probes=probes, resolutions=resolutions, rejected=rejected,
                    configure=lambda env: owner._configure(driver, args, target, env))

    def require_cached_compiler(self, fixture, env=None):
        self.assertEqual(fixture.configure(env or {}), fixture.build)
        self.assertEqual(len(fixture.commands), 2)
        self.assertEqual(fixture.commands[0][0][:2], ["conan", "install"])
        self.assertEqual(fixture.commands[1][0][:2], ["cmake", "-S"])
        self.assertIn("-DJCE_SHADERC_EXECUTABLE=" + str(fixture.cached), fixture.commands[1][0])
        self.assertEqual(fixture.probes, [fixture.cached])
        self.assertEqual(fixture.resolutions, [])

    def test_configure_preserves_runnable_selected_cache_compiler(self):
        with self.fixture() as fixture:
            self.require_cached_compiler(fixture)

    def test_explicit_overrides_precede_cache_and_empty_primary_keeps_alias(self):
        for selectors in ({"JCE_SHADERC_EXECUTABLE": "primary", "BGFX_SHADERC": "alias"},
                          {"JCE_SHADERC_EXECUTABLE": "", "BGFX_SHADERC": "alias"}):
            with self.subTest(selectors=selectors), self.fixture() as fixture:
                primary, alias = fixture.root / "primary.exe", fixture.root / "alias.exe"
                primary.write_bytes(b"primary port"); alias.write_bytes(b"alias port")
                env = {key: str(primary if value == "primary" else alias) if value else ""
                       for key, value in selectors.items()}
                fixture.configure(env)
                expected = primary if selectors["JCE_SHADERC_EXECUTABLE"] else alias
                self.assertIn("-DJCE_SHADERC_EXECUTABLE=" + str(expected), fixture.commands[-1][0])
                self.assertEqual(fixture.probes, [expected])
                self.assertEqual(fixture.resolutions, [])
                self.assertEqual(len(fixture.commands), 2)
        with self.fixture() as fixture:
            self.require_cached_compiler(fixture, {"JCE_SHADERC_EXECUTABLE": "", "BGFX_SHADERC": ""})

    def test_invalid_explicit_tool_fails_without_cache_or_automatic_fallback(self):
        for selector in ("JCE_SHADERC_EXECUTABLE", "BGFX_SHADERC"):
            for missing in (False, True):
                with self.subTest(selector=selector, missing=missing), self.fixture() as fixture:
                    explicit = fixture.root / "explicit.exe"
                    if not missing:
                        explicit.write_bytes(b"foreign host tool")
                        fixture.rejected.add(explicit)
                    errors = io.StringIO()
                    with redirect_stderr(errors), self.assertRaises(SystemExit):
                        fixture.configure({selector: str(explicit)})
                    self.assertIn("explicit host shaderc cannot execute", errors.getvalue())
                    self.assertEqual(fixture.probes, [] if missing else [explicit])
                    self.assertEqual(fixture.resolutions, [])
                    self.assertEqual(len(fixture.commands), 1)

    def test_unavailable_cached_tool_uses_existing_automatic_fallback(self):
        for missing in (False, True):
            with self.subTest(missing=missing), self.fixture() as fixture:
                if missing:
                    fixture.cached.unlink()
                else:
                    fixture.rejected.add(fixture.cached)
                fixture.configure({})
                self.assertIn("-DJCE_SHADERC_EXECUTABLE=" + str(fixture.automatic), fixture.commands[-1][0])
                self.assertEqual(fixture.probes, [] if missing else [fixture.cached])
                self.assertEqual(fixture.resolutions, ["automatic"])
                self.assertEqual(len(fixture.commands), 2)

    def test_inactive_cache_compiler_is_never_consulted(self):
        with self.fixture() as fixture:
            inactive = fixture.root / "inactive.exe"
            inactive.write_bytes(b"inactive host tool")
            native_cache_fixture(fixture.root, fixture.target, legacy="standalone",
                                 JCE_SHADERC_EXECUTABLE=str(inactive))
            self.require_cached_compiler(fixture)

    def test_empty_or_relative_cache_tool_does_not_select_from_process_cwd(self):
        for configured in ("", "cached/shaderc.exe"):
            with self.subTest(configured=configured), self.fixture() as fixture:
                native_cache_fixture(fixture.root, fixture.target, legacy=False,
                                     JCE_SHADERC_EXECUTABLE=configured)
                fixture.configure({})
                self.assertIn("-DJCE_SHADERC_EXECUTABLE=" + str(fixture.automatic), fixture.commands[-1][0])
                self.assertEqual(fixture.probes, [])
                self.assertEqual(fixture.resolutions, ["automatic"])

    def test_dry_run_never_probes_or_searches_a_configured_tool(self):
        with self.fixture() as fixture:
            fixture.driver.DRY_RUN = True
            fixture.configure({})
            self.assertEqual(fixture.probes, [])
            self.assertEqual(fixture.resolutions, [])
            self.assertFalse(any(value.startswith("-DJCE_SHADERC_EXECUTABLE=")
                                 for value in fixture.commands[-1][0]))

    def test_omitted_preservation_wire_fails_the_same_configure_expectation(self):
        path = ROOT / "tools/build/standalone.py"
        source = path.read_text(encoding="utf-8")
        wire = "                        shaderc = candidate"
        self.assertEqual(source.count(wire), 1)
        spec = importlib.util.spec_from_file_location("shaderc_preservation_omission", path)
        old = importlib.util.module_from_spec(spec)
        exec(compile(source.replace(wire, "                        shaderc = None"), str(path), "exec"), old.__dict__)
        with self.fixture() as fixture:
            self.require_cached_compiler(fixture)
        with self.fixture(owner=old) as fixture, self.assertRaises(AssertionError):
            self.require_cached_compiler(fixture)

    def test_foreign_native_cache_refuses_before_tool_selection_or_commands(self):
        with self.fixture() as fixture:
            native_cache_fixture(fixture.root, fixture.target, legacy=False,
                CMAKE_HOME_DIRECTORY=str(fixture.root / "foreign"), JCE_SHADERC_EXECUTABLE=str(fixture.cached))
            with self.assertRaisesRegex(ValueError, "CMAKE_HOME_DIRECTORY"):
                fixture.configure({})
            self.assertEqual(fixture.probes, [])
            self.assertEqual(fixture.resolutions, [])
            self.assertEqual(fixture.commands, [])


class NativeStandaloneTests(unittest.TestCase):
    def test_windows_owned_msvcrt_is_admitted_without_allowing_compiler_runtimes(self):
        with tempfile.TemporaryDirectory() as temporary:
            exe = Path(temporary) / "editor.exe"
            for machine in (0x14c, 0x8664):
                for delayed in (False, True):
                    exe.write_bytes(pe_fixture(["kernel32.dll"] + ([] if delayed else ["msvcrt.dll"]),
                        ["msvcrt.dll"] if delayed else [], machine=machine,
                        functions={"msvcrt.dll":["malloc", "free"]},
                        delayed_functions={"msvcrt.dll":["malloc", "free"]}))
                    with self.subTest(machine=machine, delayed=delayed):
                        self.assertIn("msvcrt.dll", MODULE.verify(exe)["imports"])
            for name in ("msvcrt-custom.dll", "msvcr120.dll", "vcruntime140.dll",
                         "libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll"):
                for delayed in (False, True):
                    exe.write_bytes(pe_fixture(["kernel32.dll"] + ([] if delayed else [name]),
                        [name] if delayed else []))
                    with self.subTest(name=name, delayed=delayed), self.assertRaisesRegex(ValueError, "external DLLs"):
                        MODULE.verify(exe)

    def test_bgfx_cpu_profiler_uses_shared_selection_and_explicit_hook_environment(self):
        spec = importlib.util.spec_from_file_location("jce_standalone_bgfx_args", ROOT / "tools/build/jce.py")
        shared = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(shared)
        # JCE_LAYOUT_V4_2_STANDALONE_FIX: isolate mocked configure from real build/desktop caches.
        scratch = tempfile.TemporaryDirectory(prefix="jce-layout-v4-2-")
        self.addCleanup(scratch.cleanup)
        root = Path(scratch.name).resolve()
        driver = SimpleNamespace(ROOT=root, HOST="windows", DRY_RUN=True,
            host_arch=lambda: "x86_64",
            sync_conan_hooks=Mock(), export_conan_recipes=Mock(),
            find_vs_install=lambda arch: None, host_build_profile=lambda: "windows-x64",
            bgfx_graphics_conan_args=shared.bgfx_graphics_conan_args,
            codec_overrides=lambda args: [], run=Mock(), build_jobs=lambda env: "2")
        target = {"host": "windows", "arch": "x86_64", "profile": "windows-x64", "key": "windows-x64"}
        args = SimpleNamespace(variant="dist", clean=False)
        env = {"CONAN_HOME": "isolated fixture cache", "JCE_BGFX_CPU_PROFILER": "on",
               "JCE_BGFX_DEBUG_ANNOTATION": "on"}
        MODULE._configure(driver, args, target, env)
        command = driver.run.call_args_list[0].args[0]
        self.assertIn("user.jce:bgfx_cpu_profiler=on", command)
        self.assertIn("user.jce:bgfx_debug_annotation=on", command)
        driver.sync_conan_hooks.assert_called_once_with(env)
        driver.export_conan_recipes.assert_called_once_with(env)
        self.assertIs(driver.run.call_args_list[0].kwargs["env"], env)
        for value in ("", "1", "invalid"):
            driver.run.reset_mock(); driver.sync_conan_hooks.reset_mock()
            driver.export_conan_recipes.reset_mock()
            with self.subTest(value=value), self.assertRaises(SystemExit):
                MODULE._configure(driver, args, target, {"JCE_BGFX_CPU_PROFILER": value})
            driver.run.assert_not_called(); driver.sync_conan_hooks.assert_not_called()
            driver.export_conan_recipes.assert_not_called()

    def test_invalid_annotation_refuses_before_standalone_dispatch(self):
        spec = importlib.util.spec_from_file_location("jce_standalone_annotation_args", ROOT / "tools/build/jce.py")
        shared = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(shared)
        driver = SimpleNamespace(ROOT=ROOT, HOST="windows", DRY_RUN=True,
            host_arch=lambda: "x86_64",
            bgfx_graphics_conan_args=shared.bgfx_graphics_conan_args,
            run=Mock())
        args = SimpleNamespace(variant="dist", clean=False)
        target = {"host": "windows", "arch": "x86_64", "key": "windows-x64"}
        with self.assertRaises(SystemExit):
            MODULE._configure(driver, args, target, {"JCE_BGFX_DEBUG_ANNOTATION": "invalid"})
        driver.run.assert_not_called()

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

    def test_known_post_windows7_symbols_rejected_in_both_import_tables(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "editor.exe"
            for name in ("GetThreadDescription", "SetThreadDescription", "CreateFile2", "CopyFile2"):
                for delayed in (False, True):
                    kwargs = {"delayed_functions" if delayed else "functions": {"KERNEL32.dll": [name]}}
                    exe.write_bytes(pe_fixture([] if delayed else ["KERNEL32.dll"],
                                               ["KERNEL32.dll"] if delayed else [], **kwargs))
                    with self.subTest(name=name, delayed=delayed), self.assertRaisesRegex(
                            ValueError, "Windows 7.*" + name):
                        MODULE.verify(exe)

    def test_import_names_not_dynamic_lookup_strings_select_the_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "editor.exe"
            blob = pe_fixture(["KERNEL32.dll"])
            blob[3500:3521] = b"GetThreadDescription\0"
            exe.write_bytes(blob)
            result = MODULE.verify(exe)
            self.assertEqual(result["windows_api_admission"]["named_import_count"], 1)
            self.assertEqual(result["windows_api_admission"]["coverage"], "known post-Windows-7 names only")

    def test_import_symbols_cover_pe32_pe32plus_arm64_and_legacy_delay(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "editor.exe"
            for machine in (0x14c, 0x8664, 0xaa64):
                for delayed in (False, True):
                    kwargs = {"delayed_functions" if delayed else "functions": {"KERNEL32.dll": ["CreateFile2"]}}
                    exe.write_bytes(pe_fixture([] if delayed else ["KERNEL32.dll"],
                                               ["KERNEL32.dll"] if delayed else [], machine=machine, **kwargs))
                    with self.subTest(machine=machine, delayed=delayed), self.assertRaisesRegex(
                            ValueError, "Windows 7.*CreateFile2"):
                        MODULE.verify(exe)
                    safe = {"delayed_functions" if delayed else "functions": {"KERNEL32.dll": ["GetProcAddress", 9]}}
                    exe.write_bytes(pe_fixture([] if delayed else ["KERNEL32.dll"],
                                               ["KERNEL32.dll"] if delayed else [], machine=machine, **safe))
                    result = MODULE.verify(exe)["windows_api_admission"]
                    self.assertEqual(result["named_import_count"], 1)
                    self.assertEqual(result["ordinal_imports"][0]["ordinal"], 9)
            exe.write_bytes(pe_fixture([], ["KERNEL32.dll"], machine=0x14c, legacy_delay=True,
                                       delayed_functions={"KERNEL32.dll": ["GetThreadDescription"]}))
            with self.assertRaisesRegex(ValueError, "Windows 7.*GetThreadDescription"):
                MODULE.verify(exe)

    def test_ordinal_imports_are_retained_with_unknown_api_floor(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "editor.exe"
            exe.write_bytes(pe_fixture(["KERNEL32.dll"], ["USER32.dll"],
                functions={"KERNEL32.dll": ["GetProcAddress", 7]},
                delayed_functions={"USER32.dll": [8]}))
            result = MODULE.verify(exe)["windows_api_admission"]
            self.assertEqual(result["named_import_count"], 1)
            self.assertEqual(result["ordinal_api_floor"], "unknown")
            self.assertEqual(result["ordinal_imports"], [
                {"dll": "kernel32.dll", "kind": "ordinary", "ordinal": 7},
                {"dll": "user32.dll", "kind": "delayed", "ordinal": 8}])

    def test_normal_imports_without_original_thunk_use_first_thunk(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "editor.exe"
            blob = pe_fixture(["KERNEL32.dll"], functions={"KERNEL32.dll": ["CreateFile2"]})
            struct.pack_into("<I", blob, 512, 0)
            exe.write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "Windows 7.*CreateFile2"):
                MODULE.verify(exe)

    def test_malformed_function_tables_cannot_hide_import_symbols(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "editor.exe"
            for broken in ("missing_thunks", "unmapped_thunk", "unmapped_name", "unterminated_name",
                           "unterminated_thunks", "reserved_delay_attributes", "short_optional_header"):
                blob = pe_fixture(["KERNEL32.dll"], ["USER32.dll"])
                if broken == "missing_thunks":
                    struct.pack_into("<I", blob, 512, 0)
                    struct.pack_into("<I", blob, 528, 0)
                elif broken == "unmapped_thunk":
                    struct.pack_into("<I", blob, 512, 0xffffff00)
                elif broken == "unmapped_name":
                    struct.pack_into("<Q", blob, 1280, 0xffffff00)
                elif broken == "unterminated_name":
                    struct.pack_into("<Q", blob, 1280, 0x1ffe)
                    blob[4094:4096] = b"xx"
                elif broken == "unterminated_thunks":
                    struct.pack_into("<I", blob, 512, 0x1ff8)
                    struct.pack_into("<Q", blob, 4088, (1 << 63) | 9)
                elif broken == "reserved_delay_attributes":
                    struct.pack_into("<I", blob, 1024, 3)
                else:
                    struct.pack_into("<H", blob, 0x94, 112)
                exe.write_bytes(blob)
                with self.subTest(broken=broken), self.assertRaises(ValueError):
                    MODULE.verify(exe)

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
        spec = importlib.util.spec_from_file_location("jce_standalone_recipe_args", ROOT / "tools/build/jce.py")
        shared = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(shared)
        # JCE_LAYOUT_V4_2_STANDALONE_FIX: isolate mocked configure from real build/desktop caches.
        scratch = tempfile.TemporaryDirectory(prefix="jce-layout-v4-2-")
        self.addCleanup(scratch.cleanup)
        root = Path(scratch.name).resolve()
        driver = SimpleNamespace(ROOT=root, HOST="windows", DRY_RUN=True,
            host_arch=lambda: "x86_64",
                                 sync_conan_hooks=Mock(), export_conan_recipes=Mock(),
                                 find_vs_install=lambda arch: None,
                                 host_build_profile=lambda: "windows-x64",
                                 bgfx_graphics_conan_args=lambda env: [], codec_overrides=lambda args: [],
                                 run=Mock(), build_jobs=Mock(side_effect=lambda env=None: (env or {}).get("CMAKE_BUILD_PARALLEL_LEVEL", "6")), log=Mock())
        args = SimpleNamespace(variant="dist", clean=False)
        target = {"host": "windows", "arch": "x86_64", "profile": "windows-x64", "key": "windows-x64"}
        env = {"CMAKE_BUILD_PARALLEL_LEVEL": "2"}
        exe = MODULE.build_editor(driver, args, target, env)
        commands = [call.args[0] for call in driver.run.call_args_list]
        self.assertIn("compiler.runtime=static", commands[0])
        self.assertEqual(commands[0][commands[0].index("-c:a") + 1], "tools.build:jobs=2")
        self.assertEqual(commands[2][-2:], ["-j", "2"])
        self.assertEqual(driver.build_jobs.call_args_list, [unittest.mock.call(env), unittest.mock.call(env)])
        self.assertIs(driver.run.call_args_list[0].kwargs["env"], env)
        self.assertIn("*:shared=False", commands[0])
        self.assertIn("-DJCE_EDITOR_STANDALONE=ON", commands[1])
        self.assertIn("-DJCE_BUILD_VARIANT=dist", commands[1])
        for language in ("VM_PYTHON", "CSHARP", "JAVA"):
            self.assertIn("-DJCE_BUILD_SCRIPT_" + language + "=OFF", commands[1])
        self.assertIn("-DJCE_ENABLE_SDK_INSTALL=OFF", commands[1])
        self.assertIn("-DJCE_BUILD_TESTS=OFF", commands[1])
        output = root / "build/desktop/windows-x64/dist"
        self.assertIn("-DJCE_EDITOR_ARTIFACT_DIR=" + str(output), commands[1])
        self.assertEqual(exe, output / "jce_editor.exe")
        cmake = (ROOT / "editor/CMakeLists.txt").read_text()
        self.assertIn("LANGUAGES c cpp js REQUIRED NO_STAGE", cmake)


class NativePackageLayoutTests(unittest.TestCase):
    def test_default_package_build_uses_canonical_cache_and_artifact(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            target = dict(NativeCacheSelectionTests.target, profile="windows-x64")
            build, toolchain = native_cache_fixture(root, target, legacy=False)
            (build / "generated/engine_assets/shaders").mkdir(parents=True)
            target_root = root / "build/desktop/fixture"
            target_root.mkdir(exist_ok=True)
            foreign = target_root / "CMakeCache.txt"
            foreign.write_bytes(b"JCE_DELIVERY_PROFILE:STRING=authoring\n")
            output = target_root / "dist"
            exe = output / "jce_editor.exe"
            expected = pe_fixture(["KERNEL32.dll"])

            def run(command, **unused):
                if command[:2] == ["cmake", "-S"]:
                    self.assertEqual(Path(command[command.index("-B") + 1]), build)
                    self.assertIn("-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain), command)
                    self.assertIn("-DJCE_EDITOR_ARTIFACT_DIR=" + str(output), command)
                elif command[:2] == ["cmake", "--build"]:
                    self.assertEqual(Path(command[2]), build)
                    output.mkdir()
                    exe.write_bytes(expected)

            driver = SimpleNamespace(ROOT=root, HOST="windows", DRY_RUN=False, host_arch=lambda: "x86_64",
                resolve_target=lambda arch: target, msvc_env=lambda selected: {},
                bgfx_graphics_conan_args=Mock(return_value=[]), codec_overrides=Mock(return_value=[]),
                sync_conan_hooks=Mock(), export_conan_recipes=Mock(), find_vs_install=Mock(return_value=None),
                host_build_profile=Mock(return_value="windows-x64"), build_jobs=Mock(return_value="2"),
                find_host_shaderc=Mock(return_value=root / "shaderc.exe"), run=Mock(side_effect=run),
                _assert_no_profiler_listener=Mock(), log=Mock())
            args = SimpleNamespace(arch=None, variant="dist", skip_build=False, out=None)
            self.assertIn(output, MODULE.editor_package_cache_paths(driver, args, target))
            with (patch.object(MODULE, "verify_assets", return_value={}),
                  patch.object(MODULE, "verify_notices", return_value={}),
                  patch.object(MODULE, "source_identity", return_value={})):
                MODULE.package_editor(driver, args)
            self.assertEqual([call.args[0][:2] for call in driver.run.call_args_list],
                             [["conan", "install"], ["cmake", "-S"], ["cmake", "--build"]])
            self.assertEqual(exe.read_bytes(), expected)
            self.assertEqual((root / "dist/editor/JCE-editor-fixture.exe").read_bytes(), expected)
            self.assertEqual(foreign.read_bytes(), b"JCE_DELIVERY_PROFILE:STRING=authoring\n")
            self.assertTrue((target_root / "obj" / "dist" / "CMakeCache.txt").is_file())

    def test_windows_subsystem_policy_follows_variant_in_build_and_package(self):
        base = ROOT / "build/tests"
        base.mkdir(parents=True, exist_ok=True)
        target = NativeCacheSelectionTests.target
        for action in ("build", "package"):
            for variant, subsystem in (("release", 3), ("dist", 2), ("dist", 3)):
                with (self.subTest(action=action, variant=variant, subsystem=subsystem),
                      tempfile.TemporaryDirectory(prefix="editor-subsystem-", dir=base) as temporary):
                    root = Path(temporary).resolve()
                    build, _ = native_cache_fixture(root, target, variant=variant)
                    output = root / "build/desktop/fixture" if action == "build" else build
                    exe = output / variant / "jce_editor.exe"
                    exe.parent.mkdir(parents=True)
                    exe.write_bytes(pe_fixture(["KERNEL32.dll"], subsystem=subsystem))
                    out = root / "delivery/editor.exe"
                    driver = SimpleNamespace(
                        ROOT=root, HOST="windows", DRY_RUN=False, resolve_target=lambda arch: target,
                        run=Mock(), build_jobs=Mock(return_value=1), _assert_no_profiler_listener=Mock(),
                        log=Mock())
                    args = SimpleNamespace(arch=None, variant=variant, skip_build=True, out=str(out))
                    receipt = (build / "standalone.json" if action == "build" else
                               root / "build/reports/delivery/fixture" / variant / "editor/manifest.json")
                    with (patch.object(MODULE, "_configure", return_value=build),
                          patch.object(MODULE, "verify_assets", return_value={"embedded": True}),
                          patch.object(MODULE, "verify_notices", return_value={"embedded": True}),
                          patch.object(MODULE, "source_identity", return_value={})):
                        if action == "build":
                            produce = lambda: MODULE.build_editor(driver, args, target, {})
                        else:
                            produce = lambda: MODULE.package_editor(driver, args)
                        if variant == "dist" and subsystem == 3:
                            with self.assertRaisesRegex(ValueError, "GUI executable"):
                                produce()
                            self.assertFalse(receipt.exists())
                            self.assertFalse(out.exists())
                        else:
                            produce()
                            self.assertEqual(json.loads(receipt.read_bytes())["pe_subsystem"], subsystem)
                            if action == "package":
                                self.assertEqual(out.read_bytes(), exe.read_bytes())

    def test_cached_override_is_authoritative_and_asset_receipt_uses_build_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build/desktop/fixture/obj/dist"
            old = build / "dist/jce_editor.exe"
            old.parent.mkdir(parents=True)
            old.write_bytes(pe_fixture(["KERNEL32.dll"]))
            output = root / "new output \u6d4b\u8bd5"
            output.mkdir()
            fresh = output / "jce_editor.exe"
            fresh.write_bytes(pe_fixture(["KERNEL32.dll", "USER32.dll"]))
            target = {"host": "windows", "key": "fixture", "arch": "x86_64"}
            native_cache_fixture(root, target, legacy=False, JCE_EDITOR_ARTIFACT_DIR=str(output))
            cache = build / "CMakeCache.txt"
            driver = SimpleNamespace(ROOT=root, HOST="windows", DRY_RUN=False,
                resolve_target=lambda arch: target, msvc_env=lambda selected: {},
                _assert_no_profiler_listener=Mock(), log=Mock())
            args = SimpleNamespace(arch=None, variant="dist", skip_build=True,
                                   out=str(root / "delivery/editor.exe"))
            with patch.object(MODULE, "verify_assets", return_value={}) as assets, \
                 patch.object(MODULE, "verify_notices", return_value={}), \
                 patch.object(MODULE, "source_identity", return_value={}):
                MODULE.package_editor(driver, args)
                self.assertEqual(Path(args.out).read_bytes(), fresh.read_bytes())
                self.assertNotEqual(Path(args.out).read_bytes(), old.read_bytes())
                assets.assert_called_once_with(fresh, build)
                assets.reset_mock()
                args.skip_build = False
                args.out = str(root / "built/editor.exe")
                with patch.object(MODULE, "build_editor", return_value=fresh) as builder:
                    MODULE.package_editor(driver, args)
                    builder.assert_called_once()
                assets.assert_called_once_with(fresh, build)
                self.assertEqual(Path(args.out).read_bytes(), fresh.read_bytes())
                args.skip_build = True
                fresh.unlink()
                args.out = str(root / "missing/editor.exe")
                with self.assertRaises(FileNotFoundError):
                    MODULE.package_editor(driver, args)
                self.assertFalse(Path(args.out).exists())
            cache.write_text("JCE_EDITOR_ARTIFACT_DIR:PATH=\n", encoding="utf-8")
            self.assertEqual(MODULE.cached_editor_artifact(build, "dist", "windows"), old)
            cache.unlink()
            self.assertEqual(MODULE.cached_editor_artifact(build, "dist", "windows"), old)
            cache.write_text("JCE_EDITOR_ARTIFACT_DIR:PATH=relative output\nJCE_BUILD_VARIANT:STRING=dist\n", encoding="utf-8")
            self.assertEqual(MODULE.cached_editor_artifact(build, "dist", "windows"),
                             build / "relative output/jce_editor.exe")
            cache.write_text("JCE_EDITOR_ARTIFACT_DIR:PATH=" + str(output) + "\nJCE_BUILD_VARIANT:STRING=release\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "variant"):
                MODULE.cached_editor_artifact(build, "dist", "windows")
            fresh.write_bytes(pe_fixture(["KERNEL32.dll", "USER32.dll"]))
            args.skip_build = False
            args.out = str(root / "new variant/editor.exe")
            native_cache_fixture(root, target, variant="release", legacy=False, JCE_EDITOR_ARTIFACT_DIR=str(output))
            # JCE_LAYOUT_V4_2_STANDALONE_FIX: release and dist have independent CMake caches.
            native_cache_fixture(root, target, variant="dist", legacy=False, JCE_EDITOR_ARTIFACT_DIR=str(output))
            self.assertTrue((root / "build/desktop/fixture/obj/release/CMakeCache.txt").is_file())
            self.assertTrue((root / "build/desktop/fixture/obj/dist/CMakeCache.txt").is_file())
            with patch.object(MODULE, "build_editor", return_value=fresh), \
                 patch.object(MODULE, "verify_assets", return_value={}), \
                 patch.object(MODULE, "verify_notices", return_value={}), \
                 patch.object(MODULE, "source_identity", return_value={}):
                MODULE.package_editor(driver, args)
            self.assertEqual(Path(args.out).read_bytes(), fresh.read_bytes())

    def test_only_executable_is_staged_and_failure_does_not_publish(self):
        base = ROOT / 'build/tests/standalone-package-layout'
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            root = Path(temporary).resolve()
            source = root / 'build/desktop/fixture/obj/dist/dist/jce_editor.exe'
            source.parent.mkdir(parents=True)
            source.write_bytes(pe_fixture(['KERNEL32.dll']))
            out = root / 'delivery/editor.exe'
            target = {'host': 'windows', 'key': 'fixture', 'arch': 'x86_64'}
            native_cache_fixture(root, target)
            driver = SimpleNamespace(ROOT=root, HOST='windows', DRY_RUN=False,
                resolve_target=lambda arch: target, msvc_env=lambda selected: {},
                _assert_no_profiler_listener=Mock(), log=Mock())
            args = SimpleNamespace(arch=None, variant='dist', skip_build=True, out=str(out))
            with patch.object(MODULE, 'verify_assets', return_value={'scope': 'fixture'}), \
                 patch.object(MODULE, 'verify_notices', return_value={'scope': 'fixture'}), \
                 patch.object(MODULE, 'source_identity', return_value={'jce_version': 'fixture'}):
                MODULE.package_editor(driver, args)
                self.assertEqual(list(out.parent.iterdir()), [out])
                self.assertEqual(out.read_bytes(), source.read_bytes())
                receipt = root / 'build/reports/delivery/fixture/dist/editor/SHA256SUMS'
                self.assertEqual(receipt.read_bytes(),
                    (MODULE.verify(source)['sha256'] + '  editor.exe\n').encode())
                args.out = None
                MODULE.package_editor(driver, args)
                default = root / "dist/editor/JCE-editor-fixture.exe"
                self.assertEqual(default.read_bytes(), source.read_bytes())
                self.assertEqual(list(default.parent.iterdir()), [default])
                args.out = str(root / 'refused/editor.exe')
                source.write_bytes(pe_fixture(['KERNEL32.dll', 'python312.dll']))
                with self.assertRaisesRegex(ValueError, 'external DLLs'):
                    MODULE.package_editor(driver, args)
                self.assertFalse(Path(args.out).exists())


@unittest.skipUnless(shutil.which("cmake"), "CMake subject unavailable")
class EditorRawNoticeStagingTests(unittest.TestCase):
    def run_branch(self, *, omit_stage=False, missing_source=False):
        text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        raw = re.findall(r"else\(\)\n(\t# No cooking.*?)(?=\nendif\(\)\n\n# .*Object format detection)",
                         text, re.DOTALL)
        self.assertEqual(len(raw), 1, "one actual no-cook resource owner required")
        branch = raw[0]
        if omit_stage:
            wire = '--resource-dir "${EDITOR_NOTICE_STAGE_DIR}"'
            self.assertEqual(branch.count(wire), 1)
            branch = branch.replace(wire, "", 1)
        command = re.findall(r'add_custom_command\(\n\tOUTPUT     "\$\{EDITOR_STAMP_FILE\}".*?\n\tVERBATIM\)',
                             text, re.DOTALL)
        self.assertEqual(len(command), 1, "one actual Editor PAK command required")
        declaration = re.findall(r'^set\(TPL_SRC_FILE .*?\)$', text, re.MULTILINE)
        self.assertEqual(len(declaration), 1)
        scratch = ROOT / "build/tests/editor-raw-notice-staging"
        scratch.mkdir(parents=True, exist_ok=True)
        holder = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(holder.cleanup)
        root = Path(holder.name)
        source, output = root / "source with spaces", root / "output with spaces"
        source.mkdir(); output.mkdir()
        contents = {"LICENSE": b"JCE rights fixture\r\nAll bytes retained.\n",
                    "THIRD_PARTY_LICENSES.md": b"Third-party rights fixture\nNo substitute notice.\n"}
        for name, payload in contents.items():
            if not (missing_source and name == "THIRD_PARTY_LICENSES.md"):
                (source / name).write_bytes(payload)
        script = ('cmake_minimum_required(VERSION 3.20)\n'
                  f'set(CMAKE_CURRENT_SOURCE_DIR [==[{source.as_posix()}]==])\n'
                  f'set(CMAKE_CURRENT_BINARY_DIR [==[{output.as_posix()}]==])\n'
                  'set(ENGINE_RESOURCE_DIR "engine-root")\n'
                  'set(ENGINE_UI_DIR "engine-ui")\n'
                  'set(EDITOR_RESOURCE_DIR "editor-assets")\n'
                  'set(GAME_ICON_STAGE_DIR "unused-game-icon")\n' + declaration[0] + '\n' + branch + '\n'
                  # Counted command destination; no archive or compiler is run.
                  'function(add_custom_command)\n'
                  '  string(REPLACE ";" "\\n" packet "${ARGV}")\n'
                  '  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pack-args.txt" "${packet}\\n")\n'
                  'endfunction()\n' + command[0] + '\n')
        driver = root / "stage.cmake"
        driver.write_text(script, encoding="utf-8", newline="\n")
        result = subprocess.run([shutil.which("cmake"), "-P", str(driver)],
                                capture_output=True, text=True, timeout=30)
        args = (output / "pack-args.txt").read_text(encoding="utf-8").splitlines() if result.returncode == 0 else []
        return result, source, output, contents, args

    def check_branch(self, *, omit_stage=False):
        result, source, output, contents, args = self.run_branch(omit_stage=omit_stage)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        roots = [args[i + 1] for i, item in enumerate(args[:-1]) if item == "--resource-dir"]
        stage = output / "_raw_editor_notices"
        self.assertIn(stage.as_posix(), roots, "root notices must enter raw Editor resource list")
        self.assertEqual(roots[:3], ["engine-root", "engine-ui", "editor-assets"])
        self.assertEqual({f.name for f in stage.iterdir()}, set(contents))
        depends = args[args.index("DEPENDS") + 1:]
        for name, payload in contents.items():
            self.assertTrue((stage / name).is_file(), "missing staged root notice: " + name)
            self.assertEqual((stage / name).read_bytes(), payload)
            self.assertIn((source / name).as_posix(), depends,
                          "notice edits must invalidate the actual Editor pack")

    def test_actual_raw_editor_branch_stages_exact_root_notices_and_pack_dependencies(self):
        self.check_branch()

    def test_missing_root_notice_refuses_before_pack_command(self):
        result, _, output, _, args = self.run_branch(missing_source=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("THIRD_PARTY_LICENSES.md", result.stderr)
        self.assertFalse((output / "pack-args.txt").exists())
        self.assertEqual(args, [])

    def test_omitted_actual_notice_resource_wire_fails_ordinary_assertion(self):
        with self.assertRaisesRegex(AssertionError, "root notices must enter raw Editor resource list"):
            self.check_branch(omit_stage=True)


if __name__ == "__main__":
    unittest.main()
