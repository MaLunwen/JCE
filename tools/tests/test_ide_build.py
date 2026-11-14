"""Actual IDE owner/JSONC controls with counted compiler ports, not Windows builds."""
from __future__ import annotations

from contextlib import contextmanager, redirect_stdout, redirect_stderr
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from tools.build import ide, ide_jsonc as local


def load_driver():
    spec = importlib.util.spec_from_file_location("jce_ide_test_driver", ROOT / "tools/build/jce.py")
    driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(driver)
    return driver


class JsoncControls(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / "build/tests/ide-build"
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()

    def test_comments_bom_crlf_strings_and_unknown_members_are_preserved(self):
        raw = (b'\xef\xbb\xbf{\r\n  // user scan policy\r\n  "watch": {"**/build/**":true},\r\n'
               b'  "url":"http://host/*x*/", /* retained */\r\n  "enabled": false,\r\n}\r\n')
        after = local.set_object(raw, {"enabled": True, "added": ["x", "y"]})
        self.assertTrue(after.startswith(b'\xef\xbb\xbf'))
        for original in (b'// user scan policy\r\n', b'"watch": {"**/build/**":true}',
                         b'"url":"http://host/*x*/", /* retained */'):
            self.assertIn(original, after)
        self.assertNotIn(b'\n', after.replace(b'\r\n', b''))
        self.assertEqual(local.Document(after).root.value,
                         {"watch": {"**/build/**": True}, "url": "http://host/*x*/",
                          "enabled": True, "added": ["x", "y"]})
        self.assertEqual(local.set_object(after, {"enabled": True, "added": ["x", "y"]}), after)

    def test_compact_and_commented_containers_append_correctly(self):
        for raw in (b'{}', b'{"x":1}', b'{"x":1,}', b'{/* keep */}', b'{"x":1 // tail\n}'):
            with self.subTest(raw=raw):
                after = local.set_object(raw, {"y": 2})
                self.assertEqual(local.Document(after).root.value["y"], 2)
        for raw in (b'{"tasks":[]}', b'{"tasks":[{"label":"old"}]}',
                    b'{"tasks":[{"label":"old"},/* keep */]}'):
            after = local.named_entry(raw, "tasks", "label", {"label": "new"})
            self.assertEqual(local.Document(after).root.value["tasks"][-1], {"label": "new"})

    def test_invalid_duplicate_ambiguous_and_excessive_inputs_refuse(self):
        for raw in (b'{"x":1,"x":2}', b'{"x":{"x":1,"x":2}}', b'{"x":NaN}',
                    b'{"x":1}garbage', b'{/*unclosed', b'[1 2]', b'{1:2}',
                    b'[' * 66 + b'0' + b']' * 66):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                local.Document(raw)
        with self.assertRaises(ValueError):
            local.Document(b' ' * (local.MAX_BYTES + 1))

    def test_generated_entry_refresh_requires_unchanged_full_content_hash(self):
        old = local.stamp_entry({"name": "jce", "cacheVariables": {"x": "old"}})
        new = local.stamp_entry({"name": "jce", "cacheVariables": {"x": "new"}})
        raw = json.dumps({"configurePresets": [old, {"name": "user", "value": 7}]}).encode()
        result = local.named_entry(raw, "configurePresets", "name", new, replace_owned=True)
        self.assertEqual(local.Document(result).root.value["configurePresets"], [new, {"name": "user", "value": 7}])
        for edit in ("extra_variable", "extra_target", "marker_only"):
            changed = json.loads(json.dumps(old))
            if edit == "extra_variable": changed["cacheVariables"]["custom"] = "keep"
            if edit == "extra_target": changed["targets"] = ["custom"]
            if edit == "marker_only": changed["vendor"]["jce/ide/1.0"].pop("sha256")
            raw = json.dumps({"configurePresets": [changed]}).encode()
            with self.subTest(edit=edit), self.assertRaisesRegex(ValueError, "user changes"):
                local.named_entry(raw, "configurePresets", "name", new, replace_owned=True)

    def test_known_legacy_entry_migrates_without_replacing_neighbour_comments(self):
        old, new = {"label": "old", "command": "previous"}, {"label": "new", "command": "native"}
        raw = (b'\xef\xbb\xbf{\r\n "tasks":[\r\n // retain old annotation\r\n '
               + json.dumps(old).encode() + b',\r\n /* custom annotation */ {"label":"custom"},\r\n ],\r\n}\r\n')
        after = local.migrate_named_entry(raw, "tasks", "label", new, [old])
        self.assertEqual(local.Document(after).root.value["tasks"], [new, {"label": "custom"}])
        for fragment in (b'\xef\xbb\xbf', b'// retain old annotation\r\n', b'/* custom annotation */ {"label":"custom"}'):
            self.assertIn(fragment, after)
        self.assertEqual(local.migrate_named_entry(after, "tasks", "label", new, [old]), after)

    def test_exact_legacy_and_current_entry_coalesce_at_either_position(self):
        old, new = {"label": "old", "command": "previous"}, {"label": "new", "command": "native"}
        for order in ([old, new], [new, old], [old, {"label": "custom"}, new]):
            for trailing in (False, True):
                with self.subTest(order=order, trailing=trailing):
                    rows = b', /* retain */\n'.join(json.dumps(row).encode() for row in order)
                    raw = b'{"tasks":[' + rows + (b', // tail\n' if trailing else b'') + b']}'
                    after = local.migrate_named_entry(raw, "tasks", "label", new, [old])
                    self.assertEqual(local.Document(after).root.value["tasks"], [row for row in order if row != old])
                    self.assertEqual(after.count(b'/* retain */'), raw.count(b'/* retain */'))
                    if trailing: self.assertIn(b'// tail\n', after)

    def test_legacy_name_does_not_authorize_modified_or_duplicate_entry(self):
        old, new = {"label": "old", "command": "previous"}, {"label": "new", "command": "native"}
        for entries in ([dict(old, args=["user"])], [old, old], [dict(new, args=["user"])], [new, new]):
            raw = json.dumps({"tasks": entries}).encode()
            with self.subTest(entries=entries), self.assertRaises(ValueError):
                local.migrate_named_entry(raw, "tasks", "label", new, [old])

    def test_migration_identity_distinguishes_json_booleans_from_numbers(self):
        old = {"label": "old", "group": {"isDefault": False}}
        new = {"label": "new", "group": {"isDefault": False}}
        for name in ("old", "new"):
            raw = json.dumps({"tasks": [{"label": name, "group": {"isDefault": 0}}]}).encode()
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "user changes"):
                local.migrate_named_entry(raw, "tasks", "label", new, [old])
        after = local.set_object(b'{"enabled":0}', {"enabled": False})
        self.assertIs(local.Document(after).root.value["enabled"], False)

    def test_transaction_saves_exact_preimages_and_noop_preserves_mtime(self):
        path = self.root / ".vscode/settings.json"
        path.parent.mkdir()
        before = b'{/* original */"x":1}\r\n'
        path.write_bytes(before)
        transaction = local.Transaction(self.root, {".vscode/settings.json": (before, b'{"x":2}\n'),
                                                  ".vscode/tasks.json": (None, b'{}\n')})
        transaction.apply(); transaction.commit()
        self.assertEqual((transaction.backup / "before/.vscode/settings.json").read_bytes(), before)
        self.assertEqual(json.loads((transaction.backup / "receipt.json").read_text())["status"],
                         "APPLIED_NOT_IDE_EXECUTED")
        mtime = path.stat().st_mtime_ns
        no_op = local.Transaction(self.root, {".vscode/settings.json": (path.read_bytes(), path.read_bytes())})
        no_op.apply(); no_op.commit()
        self.assertFalse(no_op.backup.exists())
        self.assertEqual(path.stat().st_mtime_ns, mtime)

    def test_second_file_replace_failure_rolls_back_original_bytes_and_mtime(self):
        one = self.root / "CMakeUserPresets.json"
        one.write_bytes(b'{"version":6}\r\n')
        before = one.read_bytes(); mtime = one.stat().st_mtime_ns
        transaction = local.Transaction(self.root, {"CMakeUserPresets.json": (before, b'{"version":7}\n'),
                                                  ".vscode/settings.json": (None, b'{}\n')})
        real_replace = os.replace
        def failing(source, destination):
            if Path(destination) == self.root / ".vscode/settings.json":
                raise OSError("injected second destination failure")
            return real_replace(source, destination)
        with patch.object(local.os, "replace", side_effect=failing), self.assertRaises(OSError):
            transaction.apply()
        self.assertEqual(one.read_bytes(), before)
        self.assertEqual(one.stat().st_mtime_ns, mtime)
        self.assertFalse((self.root / ".vscode/settings.json").exists())
        self.assertEqual(transaction.receipt["status"], "ROLLED_BACK")

    def test_concurrent_preimage_and_rollback_edits_are_never_overwritten(self):
        path = self.root / "CMakeUserPresets.json"
        path.write_bytes(b'{}')
        transaction = local.Transaction(self.root, {path.name: (b'{}', b'{"x":1}')})
        path.write_bytes(b'{"user":1}')
        with self.assertRaisesRegex(ValueError, "changed before"):
            transaction.apply()
        self.assertEqual(path.read_bytes(), b'{"user":1}')
        self.assertFalse(transaction.backup.exists())
        transaction = local.Transaction(self.root, {path.name: (path.read_bytes(), b'{"x":1}')})
        transaction.apply()
        path.write_bytes(b'{"new user edit":2}')
        with self.assertRaisesRegex(ValueError, "concurrent edits"):
            transaction.rollback()
        self.assertEqual(path.read_bytes(), b'{"new user edit":2}')

    def test_aliased_configuration_is_refused(self):
        outside = self.root / "outside"
        outside.mkdir(); (outside / "settings.json").write_bytes(b'{}')
        try:
            (self.root / ".vscode").symlink_to(outside, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("host cannot create the symlink control")
        with self.assertRaisesRegex(ValueError, "aliases"):
            local.read_file(self.root, ".vscode/settings.json")
        self.assertEqual((outside / "settings.json").read_bytes(), b'{}')

    def test_rollback_continues_past_an_aliased_conflict(self):
        for name in ("a.json", "b.json", "outside.json"):
            (self.root / name).write_bytes(b'{}')
        transaction = local.Transaction(self.root, {name: (b'{}', b'{"new":true}')
                                                  for name in ("a.json", "b.json")})
        transaction.apply()
        b = self.root / "b.json"; b.unlink()
        try:
            b.symlink_to(self.root / "outside.json")
        except (OSError, NotImplementedError):
            self.skipTest("host cannot create the symlink control")
        with self.assertRaisesRegex(ValueError, "concurrent edits"):
            transaction.rollback()
        self.assertEqual((self.root / "a.json").read_bytes(), b'{}')
        self.assertEqual((self.root / "outside.json").read_bytes(), b'{}')
        self.assertEqual(transaction.receipt["status"], "ROLLBACK_CONFLICT")


class ToolAdmissionControls(unittest.TestCase):
    def tools(self, arch="x64", family="14.44.00000"):
        base = ROOT / "build/tests/ide-build/observed-vs/VC/Tools/MSVC" / family / "bin/Hostx64" / arch
        return {name: str(base / (name + ".exe")) for name in ("cmake", "ninja", "conan", "cl", "link")}

    def admit(self, tools, shell_arch):
        driver = load_driver()
        env = {"PATH": "counted test path", "INCLUDE": "include", "LIB": "lib",
               "VSCMD_ARG_TGT_ARCH": shell_arch}
        target = dict(driver.TARGETS["windows-x64"], key="windows-x64", cross=False)
        with patch.object(ide.shutil, "which", side_effect=lambda name, **kwargs: tools.get(name)), \
             patch.object(driver, "_msvc_env_ready", return_value=True), \
             patch.object(ide.subprocess, "run", return_value=SimpleNamespace(returncode=0, stdout="counted tool probe")):
            return ide._tool_paths(driver, env, target)

    def test_actual_target_admission_accepts_matching_compiler_and_linker(self):
        self.assertEqual(set(self.admit(self.tools(), "x64")), {"cmake", "ninja", "conan", "cl", "link"})

    def test_wrong_ready_shell_architecture_refuses(self):
        with self.assertRaisesRegex(ValueError, "developer-shell target"):
            self.admit(self.tools(), "x86")

    def test_wrong_compiler_and_linker_target_paths_refuse(self):
        for key in ("cl", "link"):
            tools = self.tools(); tools[key] = self.tools("x86")[key]
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "does not match target"):
                self.admit(tools, "x64")

    def test_newer_ide_toolset_does_not_silently_change_conan_family(self):
        with self.assertRaisesRegex(ValueError, "Conan profile family"):
            self.admit(self.tools(family="14.51.00000"), "x64")

    def bundled_case(self, missing=False):
        scratch = ROOT / "build/tests/ide-build"; scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as temporary:
            vs = Path(temporary).resolve() / "selected VS with spaces"
            compiler = vs / "VC/Tools/MSVC/14.44.00000/bin/Hostx64/x64"
            compiler.mkdir(parents=True)
            for name in ("cl", "link", "conan"): (compiler / (name + ".exe")).write_bytes(b'file fixture, not native execution')
            cmake = vs / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
            ninja = vs / "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
            for path in ([cmake] if missing else [cmake, ninja]):
                path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'file fixture, not native execution')
            driver = load_driver()
            env = {"PATH": "without bundled tools", "INCLUDE": "include", "LIB": "lib",
                   "VSCMD_ARG_TGT_ARCH": "x64", "VSINSTALLDIR": str(vs)}
            target = dict(driver.TARGETS["windows-x64"], key="windows-x64", cross=False)
            def which(name, **kwargs): return str(compiler / (name + ".exe")) if name in ("cl", "link", "conan") else None
            with patch.object(ide.shutil, "which", side_effect=which), \
                 patch.object(driver, "_msvc_env_ready", return_value=True), \
                 patch.object(driver, "find_vs_install", side_effect=AssertionError("must reuse observed VS")), \
                 patch.object(ide.subprocess, "run", return_value=SimpleNamespace(returncode=0, stdout="counted version probe")) as run:
                if missing:
                    with self.assertRaisesRegex(ValueError, "cannot find ninja"):
                        ide._tool_paths(driver, env, target)
                    run.assert_not_called()
                else:
                    result = ide._tool_paths(driver, env, target)
                    self.assertEqual(result["cmake"], str(cmake)); self.assertEqual(result["ninja"], str(ninja))
                    self.assertIn(str(cmake.parent), env["PATH"]); self.assertIn(str(ninja.parent), env["PATH"])
                    self.assertEqual(run.call_count, 3)

    def test_selected_vs_bundled_cmake_and_ninja_fill_missing_path(self):
        self.bundled_case()

    def test_missing_bundled_tool_refuses_without_version_probes(self):
        self.bundled_case(missing=True)


class IdeBuildControls(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / "build/tests/ide-build"
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        shutil.copyfile(ROOT / "CMakePresets.json", self.root / "CMakePresets.json")
        self.driver = load_driver()
        self.driver.ROOT = self.root; self.driver.HOST = "windows"
        self.driver.host_arch = lambda: "x86_64"
        self.events, self.locks = [], []
        self.tools = {name: str(self.root / "tool path" / (name + ".exe"))
                      for name in ("cmake", "ninja", "conan", "cl")}
        self.env = {"PATH": "observed path", "INCLUDE": "observed include", "LIB": "observed lib",
                    "VCToolsVersion": "14.44.00000", "CMAKE_BUILD_PARALLEL_LEVEL": "2",
                    "UNRELATED_SECRET": "must never be saved"}
        self.driver.msvc_env = lambda target: dict(self.env)
        @contextmanager
        def scope(paths):
            self.locks.append(list(paths))
            yield
        self.driver._cache_scope = scope
        self.driver.conan_install = self.install
        self.driver.run = self.run_cmake
        self.config = "Release"
        self.toolchain = self.driver.toolchain_path(self.driver.resolve_target("x64"), self.config)
        # A pre-existing toolchain must still go through the shared fingerprint owner.
        self.toolchain.parent.mkdir(parents=True); self.toolchain.write_text("# old marker\n")
        self.output = io.StringIO()
        patcher = patch.object(ide, "_tool_paths", return_value=self.tools)
        patcher.start(); self.addCleanup(patcher.stop)

    def install(self, target, config, env):
        self.events.append(("conan", config, dict(env)))
        self.toolchain = self.driver.toolchain_path(target, config)
        if not self.driver.DRY_RUN:
            self.toolchain.parent.mkdir(parents=True, exist_ok=True)
            self.toolchain.write_text("# counted owner output, not a real Conan toolchain\n")
        return self.toolchain

    def run_cmake(self, command, *, env, cwd):
        self.events.append(("configure", list(command), dict(env)))
        self.assertEqual(cwd, self.root)
        self.assertEqual(command[1], "--preset")
        if self.driver.DRY_RUN:
            return
        preset = command[2]
        variant = preset.rsplit("-", 1)[1]
        tree = self.driver.preset_binary_dir(self.driver.resolve_target("x64"), variant)
        tree.mkdir(parents=True, exist_ok=True)
        cache = {"CMAKE_HOME_DIRECTORY": str(self.root), "CMAKE_TOOLCHAIN_FILE": str(self.toolchain),
                 "CMAKE_MAKE_PROGRAM": self.tools["ninja"], "CMAKE_C_COMPILER": self.tools["cl"],
                 "CMAKE_CXX_COMPILER": self.tools["cl"], "CMAKE_GENERATOR": "Ninja",
                 "CMAKE_BUILD_TYPE": "Debug" if variant == "debug" else "Release",
                 "JCE_BUILD_EDITOR": "ON", "JCE_BUILD_VARIANT": variant, "JCE_DELIVERY_PROFILE": "authoring"}
        (tree / "CMakeCache.txt").write_text("\n".join(k + ":STRING=" + v for k, v in cache.items()))
        (tree / "build.ninja").write_text("# counted configure port, never built\n")
        (tree / "compile_commands.json").write_text("[]\n")

    def invoke(self, *args):
        with redirect_stdout(self.output), redirect_stderr(self.output):
            return self.driver.main(list(args) or ["ide", "--arch", "x64"])

    def historical_presets(self, variants=("release",)):
        """Full previous generated shapes; only their published hash claims ownership."""
        configure, build = [], []
        for variant in variants:
            canonical = "windows-x64-" + variant
            name = "jce-ide-" + canonical
            configure.append(local.stamp_entry({
                "name": name, "displayName": "JCE IDE " + canonical,
                "description": "Prepared by scripts/jce.py ide; machine-local compiler environment",
                "inherits": canonical, "cmakeExecutable": self.tools["cmake"],
                "architecture": {"value": "unspecified", "strategy": "external"},
                "toolset": {"strategy": "external"},
                "environment": {"PATH": self.env["PATH"], "VCTOOLSVERSION": "14.44.00000"},
                "cacheVariables": {"CMAKE_MAKE_PROGRAM": self.tools["ninja"],
                    "CMAKE_C_COMPILER": self.tools["cl"], "CMAKE_CXX_COMPILER": self.tools["cl"],
                    "JCE_BUILD_EDITOR": "ON", "JCE_BUILD_VARIANT": variant, "JCE_DELIVERY_PROFILE": "authoring"}}))
            build.append(local.stamp_entry({"name": "build-" + name,
                "displayName": "Build JCE IDE " + canonical, "configurePreset": name,
                "targets": ["JCE_Editor"]}))
        return {"version": 6, "configurePresets": configure, "buildPresets": build}

    @staticmethod
    def historical_vscode(arch="x64"):
        label = "JCE: Build native Editor (release)"
        task = {"label": label, "type": "process", "command": "py",
                "args": ["-3", "${workspaceFolder}/scripts/jce.py", "editor", "--arch", arch,
                         "--variant", "release"], "options": {"cwd": "${workspaceFolder}"},
                "problemMatcher": "$msCompile", "group": {"kind": "build", "isDefault": False}}
        launch = {"name": "JCE Editor (Native Release)", "type": "cppvsdbg", "request": "launch",
                  "program": "${workspaceFolder}/build/desktop/windows-" + arch + "/release/jce_editor.exe",
                  "args": [], "cwd": "${workspaceFolder}", "console": "integratedTerminal",
                  "preLaunchTask": label}
        return task, launch

    def stale_metadata(self):
        """Counted CMake-owned file fixture, not a Windows compiler output."""
        tree = self.driver.preset_binary_dir(self.driver.resolve_target("x64"), "release")
        tree.mkdir(parents=True, exist_ok=True)
        cache = ("CMAKE_HOME_DIRECTORY:INTERNAL=" + self.root.as_posix() + "\n"
                 "CMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_CACHEFILE_DIR:INTERNAL=" + tree.as_posix() + "\n")
        (tree / "CMakeCache.txt").write_text(cache)
        descriptions = {}
        for version in ("3.27.9", "4.3.4"):
            directory = tree / "CMakeFiles" / version
            directory.mkdir(parents=True)
            values = {"CMakeSystem.cmake": 'set(CMAKE_SYSTEM_LOADED 1)\n'}
            values.update({"CMake" + language + "Compiler.cmake":
                'set(CMAKE_' + language + '_COMPILER "' + self.tools["cl"] + '")\n'
                for language in ("C", "CXX")})
            for name, text in values.items():
                path = directory / name
                path.write_text(text); descriptions[path] = path.read_bytes()
        sentinels = {tree / "CMakeFiles/JCE_Editor.dir/existing.obj": b"existing object bytes",
                     tree / "release/jce_editor.exe": b"existing executable bytes"}
        for path, raw in sentinels.items():
            path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(raw)
        return tree, cache.encode(), descriptions, sentinels

    def metadata_receipt(self):
        rows = [(path, json.loads(path.read_bytes())) for path in
                (self.root / "build/validation/ide").glob("*/receipt.json")]
        selected = [(path, row) for path, row in rows if row.get("owner") == ide.cache.OWNER]
        self.assertEqual(len(selected), 1)
        return selected[0]

    def test_prepare_retires_both_cmake_versions_before_configure_and_keeps_objects(self):
        tree, original_cache, descriptions, sentinels = self.stale_metadata()
        original_run = self.driver.run
        def configure(command, **kwargs):
            self.assertTrue(all(not path.exists() for path in descriptions))
            self.assertEqual((tree / "CMakeCache.txt").read_bytes(), original_cache)
            self.assertEqual({path: path.read_bytes() for path in sentinels}, sentinels)
            original_run(command, **kwargs)
            # Model only this invocation's generated version. The next native
            # IDE invocation must generate its own version, not reuse stale data.
            for path, raw in descriptions.items():
                if path.parent.name == "3.27.9":
                    if path.name == "CMakeSystem.cmake":
                        raw = ('include("' + self.toolchain.as_posix() + '")\n').encode() + raw
                    path.write_bytes(raw)
        self.driver.run = configure
        self.invoke("ide", "--arch", "x64")
        receipt_path, receipt = self.metadata_receipt()
        self.assertEqual(receipt["status"], "REGENERATED_NOT_BUILT")
        for path, before in descriptions.items():
            self.assertEqual((receipt_path.parent / "before" / path.relative_to(self.root)).read_bytes(), before)
        self.assertEqual((receipt_path.parent / "before" / tree.relative_to(self.root) / "CMakeCache.txt").read_bytes(), original_cache)
        self.assertEqual({path: path.read_bytes() for path in sentinels}, sentinels)
        self.assertTrue(all(not path.exists() for path in descriptions if path.parent.name == "4.3.4"))
        self.assertEqual([event[0] for event in self.events], ["conan", "configure"])
        self.assertIn("CONFIGURED_NOT_BUILT", self.output.getvalue())

    def test_foreign_or_missing_cache_source_refuses_before_conan_and_settings(self):
        tree, original_cache, descriptions, _ = self.stale_metadata()
        settings = self.root / ".vscode/settings.json"
        settings.parent.mkdir(); settings.write_bytes(b'{"custom":"preserved"}\r\n')
        for source in (None, str(self.root / "different-source")):
            with self.subTest(source=source):
                rows = original_cache.decode().splitlines()
                rows = [line for line in rows if not line.startswith("CMAKE_HOME_DIRECTORY:")]
                if source is not None: rows.append("CMAKE_HOME_DIRECTORY:INTERNAL=" + source)
                (tree / "CMakeCache.txt").write_text("\n".join(rows) + "\n")
                self.events.clear()
                with self.assertRaises(SystemExit): self.invoke("ide", "--vscode")
                self.assertFalse(self.events)
                self.assertEqual(settings.read_bytes(), b'{"custom":"preserved"}\r\n')
                self.assertEqual({path: path.read_bytes() for path in descriptions}, descriptions)
        self.assertFalse((self.root / "build/validation/ide").exists())

    def test_failed_configure_keeps_new_metadata_and_restores_json_settings(self):
        tree, original_cache, descriptions, sentinels = self.stale_metadata()
        settings = self.root / ".vscode/settings.json"
        settings.parent.mkdir(); before = b'{/* user */"custom":1}\r\n'; settings.write_bytes(before)
        generated = tree / "CMakeFiles/4.3.4/CMakeSystem.cmake"
        fresh = b"# new partial output from counted failing configure\nset(CMAKE_SYSTEM_LOADED 1)\n"
        def fail(*args, **kwargs):
            self.assertTrue(all(not path.exists() for path in descriptions))
            generated.write_bytes(fresh)
            (tree / "CMakeCache.txt").write_bytes(original_cache + b"CMAKE_BUILD_TYPE:STRING=Release\n")
            raise SystemExit(27)
        self.driver.run = fail
        with self.assertRaises(SystemExit): self.invoke("ide", "--vscode")
        self.assertEqual(settings.read_bytes(), before)
        self.assertEqual(sorted(path.name for path in settings.parent.iterdir()), ["settings.json"])
        self.assertEqual(generated.read_bytes(), fresh)
        receipt_path, receipt = self.metadata_receipt()
        self.assertEqual(receipt["status"], "ROLLBACK_CONFLICT")
        self.assertTrue(receipt["cache_changed_during_configure"])
        for path, raw in descriptions.items():
            self.assertEqual((receipt_path.parent / "before" / path.relative_to(self.root)).read_bytes(), raw)
        self.assertEqual((receipt_path.parent / "failed-configure" / generated.relative_to(self.root)).read_bytes(), fresh)
        self.assertEqual({path: path.read_bytes() for path in sentinels}, sentinels)
        self.assertIn("recovery preserved newer files", self.output.getvalue())

    def test_metadata_rollback_dispatch_owns_cache_lease_and_preserves_unknown_edits(self):
        tree, original_cache, descriptions, sentinels = self.stale_metadata()
        self.invoke("ide", "--prepare-only")
        receipt_path, receipt = self.metadata_receipt()
        self.assertEqual(receipt["status"], "RETIRED_NOT_CONFIGURED")
        self.assertTrue(all(not path.exists() for path in descriptions))
        changed = next(iter(descriptions)); changed.write_bytes(b"new IDE output or user edit")
        self.events.clear(); self.locks.clear()
        with self.assertRaises(SystemExit):
            self.invoke("ide", "--rollback-local", str(receipt_path.parent))
        self.assertEqual(changed.read_bytes(), b"new IDE output or user edit")
        self.assertTrue(all(not path.exists() for path in descriptions if path != changed))
        self.assertIn(tree, self.locks[-1]); self.assertFalse(self.events)
        changed.unlink()
        self.invoke("ide", "--rollback-local", str(receipt_path.parent))
        self.assertEqual({path: path.read_bytes() for path in descriptions}, descriptions)
        self.assertEqual((tree / "CMakeCache.txt").read_bytes(), original_cache)
        self.assertEqual({path: path.read_bytes() for path in sentinels}, sentinels)
        self.assertIn(tree, self.locks[-1]); self.assertFalse(self.events)
        self.assertIn("RESTORED_CMAKE_METADATA", self.output.getvalue())

    def test_actual_parser_shared_owner_order_environment_paths_and_no_build(self):
        self.assertEqual(self.invoke("ide", "--arch", "x64", "--variant", "release"), 0)
        self.assertEqual([event[0] for event in self.events], ["conan", "configure"])
        self.assertEqual(self.events[0][1], "Release")
        self.assertEqual(self.events[0][2]["CMAKE_BUILD_PARALLEL_LEVEL"], "2")
        self.assertFalse((self.root / "CMakeUserPresets.json").exists())
        command = self.events[1][1]
        self.assertEqual(command[:3], [self.tools["cmake"], "--preset", "windows-x64-release"])
        for key, value in {"CMAKE_MAKE_PROGRAM": self.tools["ninja"],
                           "CMAKE_C_COMPILER": self.tools["cl"], "CMAKE_CXX_COMPILER": self.tools["cl"],
                           "JCE_BUILD_EDITOR": "ON", "JCE_BUILD_VARIANT": "release", "JCE_DELIVERY_PROFILE": "authoring"}.items():
            self.assertIn("-D" + key + "=" + value, command)
        self.assertEqual(self.events[1][2]["VCToolsVersion"], "14.44.00000")
        self.assertNotIn("UNRELATED_SECRET", " ".join(command))
        self.assertFalse(any("jce-ide-" in value for value in command))
        self.assertIn("CONFIGURED_NOT_BUILT", self.output.getvalue())
        self.assertIn(self.driver.conan_dir(self.driver.resolve_target("x64")), self.locks[0])

    def test_debug_uses_its_own_existing_conan_and_preset_roots(self):
        self.invoke("ide", "--arch", "x64", "--variant", "debug", "--prepare-only")
        self.assertEqual([x[0] for x in self.events], ["conan"])
        self.assertEqual(self.events[0][1], "Debug")
        self.assertTrue(self.toolchain.as_posix().endswith("conan/preset/build/Debug/generators/conan_toolchain.cmake"))
        self.assertIn("PREPARED_NOT_CONFIGURED", self.output.getvalue())
        self.assertIn("run native IDE Configure next", self.output.getvalue())
        self.assertFalse((self.root / "CMakeUserPresets.json").exists())
        self.assertFalse(self.driver.preset_binary_dir(self.driver.resolve_target("x64"), "debug").exists())

    def test_explicit_dependency_rebuild_reaches_shared_owner_once_per_name(self):
        requests = []
        def install(target, config, env, **options):
            requests.append(options)
            return self.install(target, config, env)
        self.driver.conan_install = install
        self.invoke("ide", "--arch", "x64", "--rebuild-dependency", "rmlui",
                    "--rebuild-dependency", "freetype", "--rebuild-dependency", "rmlui")
        self.assertEqual(requests, [{"rebuild_dependencies": ("rmlui", "freetype")}])
        self.assertEqual([event[0] for event in self.events], ["conan", "configure"])
        self.invoke("ide", "--arch", "x64")
        self.assertEqual(requests[-1], {})  # ordinary preparation never requests rebuilding

    def test_dependency_rebuild_refuses_invalid_names_and_rollback_before_mutation(self):
        before = {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
        invalid = ("", "*", "rmlui/*", "rmlui/6.2", "rmlui@owner", "rmlui ",
                   "--build=*", "rmlui\n", "RmlUi", "a" * 101)
        for name in invalid:
            with self.subTest(name=name), self.assertRaises(SystemExit):
                self.invoke("ide", "--rebuild-dependency=" + name)
        with self.assertRaises(SystemExit):
            self.invoke("ide", "--rollback-local", str(self.root / "unread-backup"),
                        "--rebuild-dependency", "rmlui")
        self.assertEqual(self.events, [])
        self.assertEqual(self.locks, [])
        self.assertEqual(before, {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()})

    def test_dependency_rebuild_dry_run_keeps_request_without_writes(self):
        before = {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
        requests = []
        def install(target, config, env, **options):
            requests.append(options)
            return self.install(target, config, env)
        self.driver.conan_install = install
        with patch.object(ide, "_tool_paths", side_effect=AssertionError("must not probe")):
            self.invoke("--dry-run", "ide", "--vscode", "--rebuild-dependency", "rmlui")
        self.assertEqual(requests, [{"rebuild_dependencies": ("rmlui",)}])
        self.assertEqual(before, {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()})
        self.assertEqual(self.locks, [])

    def test_configure_failure_restores_prior_user_presets_and_stops_vscode_write(self):
        path = self.root / "CMakeUserPresets.json"
        before = b'{"version":6,"configurePresets":[{"name":"user-custom","hidden":true}]}\r\n'
        path.write_bytes(before); mtime = path.stat().st_mtime_ns
        def fail(*args, **kwargs): raise SystemExit(27)
        self.driver.run = fail
        with self.assertRaises(SystemExit): self.invoke("ide", "--vscode")
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(path.stat().st_mtime_ns, mtime)
        self.assertFalse((self.root / ".vscode").exists())
        self.assertTrue(self.toolchain.exists())

    def test_failed_canonical_configure_restores_alias_migration_and_local_files(self):
        path = self.root / "CMakeUserPresets.json"
        before = (json.dumps(self.historical_presets(), indent=2) + "\n").replace("\n", "\r\n").encode()
        path.write_bytes(before); mtime = path.stat().st_mtime_ns
        def fail(*args, **kwargs): raise SystemExit(27)
        self.driver.run = fail
        with self.assertRaises(SystemExit): self.invoke("ide", "--vscode")
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(path.stat().st_mtime_ns, mtime)
        self.assertFalse((self.root / ".vscode").exists())
        receipt = next((self.root / "build/validation/ide").glob("*/receipt.json"))
        self.assertEqual(json.loads(receipt.read_bytes())["status"], "ROLLED_BACK")
        self.assertEqual((receipt.parent / "before/CMakeUserPresets.json").read_bytes(), before)

    def test_missing_toolchain_stops_before_configuration_or_local_write(self):
        self.toolchain.unlink()
        self.driver.conan_install = lambda *args: self.toolchain
        with self.assertRaises(SystemExit): self.invoke()
        self.assertFalse((self.root / "CMakeUserPresets.json").exists())
        self.assertFalse(self.events)

    def test_cached_wrong_configuration_is_not_reported_as_prepared(self):
        real_run = self.driver.run
        def wrong(*args, **kwargs):
            real_run(*args, **kwargs)
            tree = self.driver.preset_binary_dir(self.driver.resolve_target("x64"), "release")
            cache = tree / "CMakeCache.txt"
            cache.write_text(cache.read_text().replace("JCE_DELIVERY_PROFILE:STRING=authoring", "JCE_DELIVERY_PROFILE:STRING=embedded"))
        self.driver.run = wrong
        with self.assertRaises(SystemExit): self.invoke()
        self.assertNotIn("CONFIGURED_NOT_BUILT", self.output.getvalue())
        self.assertFalse((self.root / "CMakeUserPresets.json").exists())

    def test_dry_run_calls_plan_owner_without_probes_or_output_mutation(self):
        before = {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
        with patch.object(ide, "_tool_paths", side_effect=AssertionError("must not probe")):
            self.invoke("--dry-run", "ide", "--vscode")
        self.assertEqual(before, {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()})
        self.assertFalse(self.locks)

    def test_vscode_preserves_user_options_and_appends_supported_native_entry(self):
        folder = self.root / ".vscode"; folder.mkdir()
        settings = b'{\n // user scanner\n "files.watcherExclude":{"**/build/**":true},\n "java.setting":"keep",\n "modkit.shaderc.path":"old-user-path"\n}\n'
        cpp = b'{"version":4,"configurations":[{"name":"Win32","compileCommands":["native/compile_commands.json"],"defines":["_DEBUG"]}]}\n'
        launch = b'{"version":"0.2.0","configurations":[{"name":"Java custom","type":"java"},{"name":"CK (Executable)","program":"old-ck.exe"}]}\n'
        (folder / "settings.json").write_bytes(settings)
        (folder / "c_cpp_properties.json").write_bytes(cpp)
        (folder / "launch.json").write_bytes(launch)
        self.invoke("ide", "--vscode", "--shader-build-dir", "build/absent-editor")
        after = (folder / "settings.json").read_bytes()
        for original in (b'// user scanner', b'"files.watcherExclude":{"**/build/**":true}',
                         b'"java.setting":"keep"', b'"modkit.shaderc.path":"old-user-path"'):
            self.assertIn(original, after)
        props = local.Document((folder / "c_cpp_properties.json").read_bytes()).root.value
        self.assertEqual(props["configurations"][0]["compileCommands"], ["native/compile_commands.json"])
        self.assertEqual(props["configurations"][0]["configurationProvider"], "ms-vscode.cmake-tools")
        starts = local.Document((folder / "launch.json").read_bytes()).root.value["configurations"]
        self.assertEqual(starts[:2], json.loads(launch)["configurations"])
        self.assertEqual(starts[-1]["program"], "${command:cmake.launchTargetPath}")
        self.assertEqual(starts[-1]["cwd"], "${command:cmake.getLaunchTargetDirectory}")
        self.assertNotIn("preLaunchTask", starts[-1])
        tasks = local.Document((folder / "tasks.json").read_bytes()).root.value["tasks"]
        self.assertEqual(tasks[-1]["type"], "cmake")
        self.assertEqual(tasks[-1]["command"], "build")
        self.assertEqual(tasks[-1]["preset"], "${command:cmake.activeBuildPresetName}")
        self.assertNotIn("args", tasks[-1])
        values = local.Document(after).root.value
        self.assertIs(values["cmake.buildBeforeRun"], True)
        self.assertEqual(values["cmake.useVsDeveloperEnvironment"], "always")
        self.assertIn("Modkit=UNAVAILABLE", self.output.getvalue())
        self.assertIn("CONFIGURED_NOT_BUILT", self.output.getvalue())

    def test_optional_shader_requires_version_probe_before_setting_publication(self):
        settings = {"modkit.shaderc.path": str(self.root / "shaderc.exe"),
                    "modkit.shaderc.includeDirs": [str(self.root / "headers")],
                    "modkit.shaderc.useBuildDir": True, "modkit.shaderc.buildDir": str(self.root / "build/native")}
        from tools.build import ide_modkit
        with patch.object(ide_modkit, "resolve_settings", return_value=settings), \
             patch.object(self.driver, "_shaderc_runs", return_value=False) as probe:
            self.invoke("ide", "--vscode", "--shader-build-dir", "build/native")
            probe.assert_called_once_with(Path(settings["modkit.shaderc.path"]))
        self.assertNotIn("modkit.shaderc.path", local.Document((self.root / ".vscode/settings.json").read_bytes()).root.value)
        with patch.object(ide_modkit, "resolve_settings", return_value=settings), \
             patch.object(self.driver, "_shaderc_runs", return_value=True):
            self.invoke("ide", "--vscode", "--shader-build-dir", "build/native")
        actual = local.Document((self.root / ".vscode/settings.json").read_bytes()).root.value
        for key, value in settings.items(): self.assertEqual(actual[key], value)

    def test_historical_owned_presets_hide_once_without_altering_their_paths_or_environment(self):
        path = self.root / "CMakeUserPresets.json"
        historical = self.historical_presets(("release", "debug", "dist"))
        historical["configurePresets"].append({"name": "user-custom", "hidden": True, "cacheVariables": {"MY_OPTION": "keep"}})
        path.write_text(json.dumps(historical, indent=2) + "\n")
        self.invoke()
        result = json.loads(path.read_bytes())
        for field in ("configurePresets", "buildPresets"):
            for before, after in zip(historical[field], result[field]):
                if before["name"] == "user-custom":
                    self.assertEqual(before, after)
                else:
                    self.assertIs(after.get("hidden"), True)
                    self.assertTrue(local.owned_entry(after))
                    self.assertEqual(after, local.stamp_entry(dict(before, hidden=True)))
        published, mtime = path.read_bytes(), path.stat().st_mtime_ns
        receipt_count = len(list((self.root / "build/validation/ide").glob("*/receipt.json")))
        self.invoke()
        self.assertEqual(path.read_bytes(), published)
        self.assertEqual(path.stat().st_mtime_ns, mtime)
        self.assertEqual(len(list((self.root / "build/validation/ide").glob("*/receipt.json"))), receipt_count)

    def test_user_changed_or_foreign_historical_presets_are_preserved_with_diagnostic(self):
        path = self.root / "CMakeUserPresets.json"
        data = self.historical_presets()
        data["configurePresets"][0]["cacheVariables"]["MY_OPTION"] = "keep"
        data["configurePresets"].append({"name": "jce-ide-foreign", "inherits": "user-custom"})
        path.write_text(json.dumps(data)); before = path.read_bytes()
        self.invoke()
        self.assertEqual(path.read_bytes(), before)
        self.assertIn("preserved custom or modified historical preset: jce-ide-windows-x64-release", self.output.getvalue())
        self.assertIn("preserved custom or modified historical preset: build-jce-ide-windows-x64-release", self.output.getvalue())
        self.assertIn("preserved custom or modified historical preset: jce-ide-foreign", self.output.getvalue())

    def test_duplicate_or_malformed_user_presets_refuse_before_dependency_work(self):
        path = self.root / "CMakeUserPresets.json"
        for data in ({"version": 6, "configurePresets": [{"name": "windows-x64-release"}]},
                     {"version": 6, "configurePresets": [{"name": "custom"}, {"name": "custom"}]},
                     {"version": 6, "buildPresets": [{"name": "build-windows-x64-release"}]},
                     {"version": 6, "configurePresets": {}},
                     {"version": 6, "configurePresets": [7]}):
            with self.subTest(data=data):
                before = json.dumps(data).encode()
                path.write_bytes(before)
                with self.assertRaises(SystemExit): self.invoke("ide", "--vscode")
                self.assertEqual(path.read_bytes(), before)
                self.assertFalse(self.events)
                self.assertFalse((self.root / ".vscode").exists())

    def test_exact_old_vscode_rows_migrate_and_native_rows_follow_variant_without_duplication(self):
        folder = self.root / ".vscode"; folder.mkdir()
        old_task, old_launch = self.historical_vscode("arm64")
        for filename, field, entry, extra in (
                ("tasks.json", "tasks", old_task, {"label": "custom", "type": "process", "command": "keep"}),
                ("launch.json", "configurations", old_launch, {"name": "custom", "type": "java"})):
            raw = b'{\r\n // keep migration comment\r\n ' + json.dumps(field).encode() + b':['
            raw += json.dumps(entry).encode() + b', /* custom */ ' + json.dumps(extra).encode() + b'],\r\n}\r\n'
            (folder / filename).write_bytes(raw)
        self.invoke("ide", "--arch", "x64", "--variant", "debug", "--vscode")
        current = {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in folder.glob("*.json")}
        tasks = local.Document((folder / "tasks.json").read_bytes()).root.value["tasks"]
        launches = local.Document((folder / "launch.json").read_bytes()).root.value["configurations"]
        self.assertEqual(len(tasks), 2)
        self.assertEqual(tasks[0]["type"], "cmake")
        self.assertEqual(tasks[0]["preset"], "${command:cmake.activeBuildPresetName}")
        self.assertEqual(launches[0]["program"], "${command:cmake.launchTargetPath}")
        self.assertEqual(launches[0]["cwd"], "${command:cmake.getLaunchTargetDirectory}")
        self.assertNotIn("preLaunchTask", launches[0])
        for filename in ("tasks.json", "launch.json"):
            self.assertIn(b'// keep migration comment\r\n', (folder / filename).read_bytes())
            self.assertIn(b'/* custom */', (folder / filename).read_bytes())
        for variant in ("release", "dist", "debug"):
            self.invoke("ide", "--arch", "x64", "--variant", variant, "--vscode")
            for path, before in current.items():
                self.assertEqual((path.read_bytes(), path.stat().st_mtime_ns), before)
            self.assertEqual(self.events[-1][1][2], "windows-x64-" + variant)
            self.assertIn("-DJCE_BUILD_VARIANT=" + variant, self.events[-1][1])
            self.assertIn("-DJCE_DELIVERY_PROFILE=authoring", self.events[-1][1])

    def test_edited_old_task_or_launch_refuses_before_conan_or_any_local_write(self):
        folder = self.root / ".vscode"; folder.mkdir()
        for edited in ("task", "launch"):
            old_task, old_launch = self.historical_vscode()
            if edited == "task": old_task["args"].append("--user-choice")
            else: old_launch["args"] = ["--user-choice"]
            (folder / "tasks.json").write_text(json.dumps({"version": "2.0.0", "tasks": [old_task]}))
            (folder / "launch.json").write_text(json.dumps({"version": "0.2.0", "configurations": [old_launch]}))
            before = {p: p.read_bytes() for p in folder.glob("*.json")}
            with self.subTest(edited=edited), self.assertRaises(SystemExit):
                self.invoke("ide", "--vscode")
            self.assertEqual({p: p.read_bytes() for p in folder.glob("*.json")}, before)
            self.assertFalse(self.events)
            self.assertIn("legacy IDE entry has user changes", self.output.getvalue())

    def test_duplicate_old_and_current_generated_rows_coalesce_without_second_launcher(self):
        self.invoke("ide", "--vscode")
        folder = self.root / ".vscode"
        old_task, old_launch = self.historical_vscode()
        for filename, field, old in (("tasks.json", "tasks", old_task),
                                     ("launch.json", "configurations", old_launch)):
            path = folder / filename
            data = local.Document(path.read_bytes()).root.value
            data[field].append(old)
            path.write_text(json.dumps(data))
        self.invoke("ide", "--vscode")
        for filename, field in (("tasks.json", "tasks"), ("launch.json", "configurations")):
            values = local.Document((folder / filename).read_bytes()).root.value[field]
            self.assertEqual(len(values), 1)

    def test_changed_native_launch_is_not_overwritten_during_variant_switch(self):
        self.invoke("ide", "--vscode")
        folder = self.root / ".vscode"
        path = folder / "launch.json"
        data = local.Document(path.read_bytes()).root.value
        data["configurations"][0]["args"] = ["user-argument"]
        path.write_text(json.dumps(data))
        before = {p: p.read_bytes() for p in folder.glob("*.json")}
        self.events.clear()
        with self.assertRaises(SystemExit):
            self.invoke("ide", "--vscode", "--variant", "debug")
        self.assertFalse(self.events)
        self.assertEqual({p: p.read_bytes() for p in folder.glob("*.json")}, before)
        self.assertIn("existing IDE entry has user changes", self.output.getvalue())

    def test_all_jsonc_preflight_happens_before_conan_and_any_publication(self):
        folder = self.root / ".vscode"; folder.mkdir()
        bad = b'{ // unfinished user edit\n'
        (folder / "settings.json").write_bytes(bad)
        with self.assertRaises(SystemExit): self.invoke("ide", "--vscode")
        self.assertEqual((folder / "settings.json").read_bytes(), bad)
        self.assertFalse((self.root / "CMakeUserPresets.json").exists())
        self.assertFalse(self.events)

    def test_successful_local_configuration_has_exact_restore_and_repeat_status(self):
        folder = self.root / ".vscode"; folder.mkdir()
        original = b'{/* keep scanner */"files.watcherExclude":{"**/build/**":true}}\r\n'
        (folder / "settings.json").write_bytes(original)
        self.invoke("ide", "--vscode")
        receipts = list((self.root / "build/validation/ide").glob("*/receipt.json"))
        self.assertEqual(len(receipts), 1)
        backup = receipts[0].parent
        self.events.clear()
        self.invoke("ide", "--rollback-local", str(backup))
        self.assertEqual((folder / "settings.json").read_bytes(), original)
        for relative in ("CMakeUserPresets.json", ".vscode/c_cpp_properties.json", ".vscode/tasks.json", ".vscode/launch.json"):
            self.assertFalse((self.root / relative).exists())
        self.assertFalse(self.events)
        self.invoke("ide", "--rollback-local", str(backup))
        self.assertIn("ALREADY_RESTORED", self.output.getvalue())

    def test_local_restore_refuses_user_edits_and_corrupt_backup_before_any_write(self):
        folder = self.root / ".vscode"; folder.mkdir()
        (folder / "settings.json").write_bytes(b'{"custom":1}\n')
        self.invoke("ide", "--vscode")
        backup = next((self.root / "build/validation/ide").glob("*/receipt.json")).parent
        path = folder / "settings.json"
        published = path.read_bytes()
        path.write_bytes(published + b'// user change\n')
        files = list(folder.glob("*.json"))
        before = {p: p.read_bytes() for p in files}
        with self.assertRaises(SystemExit): self.invoke("ide", "--rollback-local", str(backup))
        self.assertEqual({p: p.read_bytes() for p in files}, before)
        path.write_bytes(published)
        (backup / "before/.vscode/settings.json").write_bytes(b'{"corrupt":true}')
        before = {p: p.read_bytes() for p in files}
        with self.assertRaises(SystemExit): self.invoke("ide", "--rollback-local", str(backup))
        self.assertEqual({p: p.read_bytes() for p in files}, before)

    def test_actual_standalone_configure_keeps_user_presets(self):
        module = self.driver._standalone_module()
        before = b'{"version":6,"configurePresets":[]}\n'
        (self.root / "CMakeUserPresets.json").write_bytes(before)
        build = self.root / "build/desktop/windows-x64/obj/release"
        conan = self.root / "build/desktop/windows-x64/conan/release"
        shader = self.root / "shaderc.exe"; shader.write_bytes(b'counted port, not executable')
        args = SimpleNamespace(variant="release", clean=False, artifact_dir=None, patented_codecs=None)
        target = self.driver.resolve_target("x64")
        def run(command, **kwargs):
            if command[0] == "cmake": (build / "generated/engine_assets/shaders").mkdir(parents=True)
        with patch.object(module, "native_cache_plan", return_value={"build": build, "conan": conan}), \
             patch.object(module, "sdk_outputs"), \
             patch.object(module, "editor_artifact_dir", return_value=self.root / "release"), \
             patch.object(self.driver, "sync_conan_hooks"), patch.object(self.driver, "export_conan_recipes"), \
             patch.object(self.driver, "find_vs_install", return_value=None), \
             patch.object(self.driver, "_shaderc_runs", return_value=True), \
             patch.object(self.driver, "run", side_effect=run):
            module._configure(self.driver, args, target, {"JCE_SHADERC_EXECUTABLE": str(shader)})
        self.assertTrue((self.root / "CMakeUserPresets.json").is_file())
        self.assertEqual((self.root / "CMakeUserPresets.json").read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
