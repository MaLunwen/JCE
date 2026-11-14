"""Real file controls for selective CMake metadata recovery; no Windows build."""
from __future__ import annotations

import json
import os
from pathlib import Path, PureWindowsPath
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from tools.build import ide_cache as cache, ide_jsonc as local


class MetadataControls(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / "build/tests/ide-cache"
        scratch.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.tree = self.root / "build/desktop/windows-x64/obj/preset/release"
        self.tree.mkdir(parents=True)
        self.toolchain = self.root / "build/desktop/windows-x64/conan/preset/build/Release/generators/conan_toolchain.cmake"
        self.compiler = (self.root / "VS/VC/Tools/MSVC/14.43/bin/Hostx64/x64/cl.exe").as_posix()
        self.cache_file = self.tree / "CMakeCache.txt"
        self.cache_file.write_text("CMAKE_HOME_DIRECTORY:INTERNAL=" + self.root.as_posix() +
            "\nCMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_CACHEFILE_DIR:INTERNAL=" + self.tree.as_posix() +
            "\nCMAKE_BUILD_TYPE:STRING=Release\n")

    def version(self, name="4.3.4", *, bound=False, compiler=None):
        directory = self.tree / "CMakeFiles" / name
        directory.mkdir(parents=True)
        system = 'set(CMAKE_SYSTEM_NAME "Windows")\n'
        if bound:
            system += 'include("' + self.toolchain.as_posix() + '")\n'
        system += "set(CMAKE_SYSTEM_LOADED 1)\n"
        (directory / "CMakeSystem.cmake").write_text(system)
        for language in ("C", "CXX"):
            (directory / ("CMake" + language + "Compiler.cmake")).write_text(
                'set(CMAKE_' + language + '_COMPILER "' + (compiler or self.compiler) + '")\n')
        return directory

    def recovery(self):
        return cache.Recovery(self.root, self.tree, self.toolchain, self.compiler)

    def regenerate(self, recovery):
        for key, (before, _after) in recovery.changes.items():
            (self.root / key).write_bytes(before + b"# newly generated\n")
        self.cache_file.write_bytes(self.cache_file.read_bytes() + b"JCE_BUILD_EDITOR:BOOL=ON\n")

    def test_two_versions_retire_only_descriptions_and_keep_objects(self):
        first = self.version("3.27.9", bound=True)
        second = self.version()
        for name in ("CMakeRCCompiler.cmake", "CMakeASM_MASMCompiler.cmake"):
            (second / name).write_text("# counted language metadata\n")
        objects = self.tree / "CMakeFiles/JCE_Editor.dir/main.obj"
        objects.parent.mkdir(); objects.write_bytes(b"object sentinel")
        executable = self.tree / "release/jce_editor.exe"
        executable.parent.mkdir(); executable.write_bytes(b"executable sentinel")
        unrelated = first / "user.txt"; unrelated.write_bytes(b"keep")
        observed = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in (objects, executable, unrelated, self.cache_file)}
        recovery = self.recovery()
        self.assertEqual(len(recovery.changes), 8)
        recovery.apply()
        for key, (before, _after) in recovery.changes.items():
            self.assertFalse((self.root / key).exists())
            self.assertEqual((recovery.backup / "before" / key).read_bytes(), before)
        self.assertEqual((recovery.backup / "before" / recovery.cache).read_bytes(), observed[self.cache_file][0])
        for path, original in observed.items():
            self.assertEqual((path.read_bytes(), path.stat().st_mtime_ns), original)

    def test_healthy_metadata_is_a_noop(self):
        directory = self.version(bound=True)
        before = {p: p.stat().st_mtime_ns for p in directory.iterdir()}
        recovery = self.recovery(); recovery.apply(); recovery.commit()
        self.assertFalse(recovery.changes)
        self.assertFalse(recovery.backup.exists())
        self.assertEqual(before, {p: p.stat().st_mtime_ns for p in directory.iterdir()})

    def test_windows_path_key_semantics_keep_healthy_metadata_untouched(self):
        """Windows key parsing is modeled; files still use the actual host OS."""
        directories = [self.version("3.27.9", bound=True), self.version(bound=True)]
        before = {p: (p.read_bytes(), p.stat().st_mtime_ns)
                  for directory in directories for p in directory.iterdir()}
        # Recovery receives real host Paths. Only its imported Path constructor
        # for inventory key parsing uses Windows semantics; no Windows run is claimed.
        with patch.object(cache, "Path", PureWindowsPath):
            recovery = self.recovery()
        self.assertEqual(recovery.reasons, [])
        self.assertFalse(recovery.changes)
        recovery.apply(); recovery.commit()
        self.assertFalse(recovery.backup.exists())
        self.assertEqual(before, {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in before})

    def test_windows_path_key_semantics_read_and_reject_malformed_system(self):
        """Model Windows key parsing, not Windows filesystem or CMake execution."""
        directory = self.version(bound=True)
        system = directory / "CMakeSystem.cmake"
        raw = b"# not a recognized generated CMake system description\n"
        system.write_bytes(raw)
        with patch.object(cache, "Path", PureWindowsPath):
            with self.assertRaisesRegex(ValueError, "unrecognized generated CMake system"):
                self.recovery()
        self.assertEqual(system.read_bytes(), raw)

    def test_different_or_incomplete_compiler_descriptions_trigger_recovery(self):
        directory = self.version(bound=True, compiler=self.compiler.replace("14.43", "14.44"))
        self.assertTrue(self.recovery().changes)
        (directory / "CMakeCXXCompiler.cmake").unlink()
        self.assertTrue(any("missing CXX" in row for row in self.recovery().reasons))

    def test_absent_metadata_needs_no_backup(self):
        self.cache_file.unlink()
        recovery = self.recovery(); recovery.apply(); recovery.commit()
        self.assertFalse(recovery.changes)
        self.assertFalse(recovery.backup.exists())

    def test_owner_fields_required_even_when_process_cwd_is_source_root(self):
        self.version()
        original = self.cache_file.read_bytes()
        previous = Path.cwd()
        try:
            os.chdir(self.root)
            for text in ("CMAKE_GENERATOR:INTERNAL=Ninja\n", "CMAKE_HOME_DIRECTORY:INTERNAL=\nCMAKE_GENERATOR:INTERNAL=Ninja\n",
                         original.decode().replace(str(self.root), str(self.root / "foreign")),
                         original.decode().replace("Ninja", "Visual Studio 18")):
                with self.subTest(text=text):
                    self.cache_file.write_text(text)
                    with self.assertRaises(ValueError): self.recovery()
        finally:
            os.chdir(previous)

    def test_invalid_duplicate_and_unrecognized_metadata_refuse(self):
        directory = self.version()
        original = self.cache_file.read_bytes()
        self.cache_file.write_bytes(original + b"CMAKE_GENERATOR:STRING=Ninja\n")
        with self.assertRaisesRegex(ValueError, "duplicate"): self.recovery()
        self.cache_file.write_bytes(original)
        (directory / "CMakeSystem.cmake").write_text("# user file\n")
        with self.assertRaisesRegex(ValueError, "unrecognized"): self.recovery()

    def test_unowned_tree_and_aliased_metadata_refuse(self):
        self.version()
        with self.assertRaises(ValueError):
            cache.Recovery(self.root, self.root / "build/other", self.toolchain, self.compiler)
        alias = self.tree / "CMakeFiles/3.27.9"
        try: alias.symlink_to(self.tree / "CMakeFiles/4.3.4", target_is_directory=True)
        except (OSError, NotImplementedError): self.skipTest("host does not allow symbolic links")
        with self.assertRaisesRegex(ValueError, "aliases"): self.recovery()

    def test_bounded_versions_refuse_without_removing_files(self):
        for index in range(cache.MAX_VERSIONS + 1): self.version("4.3." + str(index))
        with self.assertRaisesRegex(ValueError, "version budget"): self.recovery()
        self.assertEqual(len(list((self.tree / "CMakeFiles").glob("*/CMakeSystem.cmake"))), cache.MAX_VERSIONS + 1)

    def test_changed_cache_or_description_before_apply_is_preserved(self):
        directory = self.version()
        recovery = self.recovery()
        original = self.cache_file.read_bytes()
        self.cache_file.write_bytes(original + b"USER:STRING=keep\n")
        with self.assertRaisesRegex(ValueError, "cache changed"): recovery.apply()
        self.assertTrue((directory / "CMakeSystem.cmake").is_file())
        self.cache_file.write_bytes(original)
        recovery = self.recovery()
        changed = directory / "CMakeCCompiler.cmake"; changed.write_bytes(b"later user data")
        with self.assertRaisesRegex(ValueError, "input changed"): recovery.apply()
        self.assertEqual(changed.read_bytes(), b"later user data")
        self.assertTrue((directory / "CMakeCXXCompiler.cmake").is_file())

    def test_removal_failure_restores_already_removed_metadata(self):
        self.version()
        recovery = self.recovery()
        target = self.root / sorted(recovery.changes)[1]
        real = Path.unlink
        fired = False
        def fail(path, *args, **kwargs):
            nonlocal fired
            if path == target and not fired:
                fired = True
                raise OSError("injected metadata removal failure")
            return real(path, *args, **kwargs)
        with patch.object(Path, "unlink", fail), self.assertRaisesRegex(OSError, "injected"):
            recovery.apply()
        for key, (before, _after) in recovery.changes.items():
            self.assertEqual((self.root / key).read_bytes(), before)

    def test_before_configure_rollback_restores_original_bytes_and_times(self):
        directory = self.version()
        times = {p: p.stat().st_mtime_ns for p in directory.iterdir()}
        recovery = self.recovery(); recovery.apply(); recovery.rollback()
        for key, (before, _after) in recovery.changes.items(): self.assertEqual((self.root / key).read_bytes(), before)
        for path, stamp in times.items(): self.assertEqual(path.stat().st_mtime_ns, stamp)

    def test_failed_configure_new_metadata_is_preserved_with_both_snapshots(self):
        self.version(); recovery = self.recovery(); recovery.apply()
        key = next(iter(recovery.changes)); (self.root / key).write_bytes(b"new metadata or concurrent edit")
        with self.assertRaisesRegex(ValueError, "rollback preserved"): recovery.rollback()
        self.assertEqual((self.root / key).read_bytes(), b"new metadata or concurrent edit")
        self.assertEqual((recovery.backup / "failed-configure" / key).read_bytes(), b"new metadata or concurrent edit")
        self.assertEqual((recovery.backup / "before" / key).read_bytes(), recovery.changes[key][0])

    def test_failed_configure_changed_cache_is_never_overwritten(self):
        self.version(); recovery = self.recovery(); recovery.apply()
        self.cache_file.write_bytes(b"new partial cache")
        with self.assertRaisesRegex(ValueError, "changed its cache"): recovery.rollback()
        self.assertEqual(self.cache_file.read_bytes(), b"new partial cache")
        self.assertEqual((recovery.backup / "before" / recovery.cache).read_bytes(), recovery.cache_before)

    def test_successful_restore_is_guarded_and_idempotent(self):
        self.version(); recovery = self.recovery(); recovery.apply()
        self.regenerate(recovery); recovery.commit()
        self.assertEqual(cache.restore_resources(self.root, recovery.backup), [self.tree])
        self.assertEqual(cache.restore(self.root, recovery.backup, dry_run=True)["status"], "DRY_RUN")
        self.assertEqual(cache.restore(self.root, recovery.backup)["status"], "RESTORED_CMAKE_METADATA")
        self.assertEqual(self.cache_file.read_bytes(), recovery.cache_before)
        for key, (before, _after) in recovery.changes.items(): self.assertEqual((self.root / key).read_bytes(), before)
        self.assertEqual(cache.restore(self.root, recovery.backup)["status"], "ALREADY_RESTORED")

    def test_prepare_only_restore_accepts_still_absent_metadata(self):
        self.version(); recovery = self.recovery(); recovery.apply(); recovery.commit(configured=False)
        self.assertEqual(cache.restore(self.root, recovery.backup)["status"], "RESTORED_CMAKE_METADATA")

    def test_later_changes_or_backup_tamper_refuse_all_restoration(self):
        self.version(); recovery = self.recovery(); recovery.apply(); self.regenerate(recovery); recovery.commit()
        key = next(iter(recovery.changes)); current = (self.root / key).read_bytes()
        self.cache_file.write_bytes(b"later IDE cache")
        with self.assertRaisesRegex(ValueError, "later changes"): cache.restore(self.root, recovery.backup)
        self.assertEqual((self.root / key).read_bytes(), current)
        saved = recovery.backup / "before" / key; saved.write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "backup identity"): cache.restore(self.root, recovery.backup)

    def test_forged_restore_roster_cannot_target_source(self):
        self.version(); recovery = self.recovery(); recovery.apply(); self.regenerate(recovery); recovery.commit()
        path = recovery.backup / "receipt.json"; data = json.loads(path.read_text())
        data["files"][0]["path"] = "CMakePresets.json"; path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "unowned"): cache.restore(self.root, recovery.backup)

    def test_json_transaction_default_still_rejects_non_json(self):
        with self.assertRaises(ValueError): local.Transaction(self.root, {"local.json": (None, b"set(X 1)\n")}).apply()
        self.assertFalse((self.root / "local.json").exists())
        with self.assertRaises(ValueError): local.Transaction(self.root, {}, validate_json=0)


if __name__ == "__main__":
    unittest.main()
