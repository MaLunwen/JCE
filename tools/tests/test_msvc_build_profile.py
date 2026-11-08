"""Pure filesystem MSVC profile controls; synthetic files, no compiler executes."""
import contextlib
import json
from pathlib import Path
import shutil
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.build import msvc_build_profile as subject

ROOT = Path(__file__).resolve().parents[2]
BASE = ("include(base)\n\n[settings]\nos=Windows\narch=x86_64\ncompiler=msvc\n"
        "compiler.version=194\ncompiler.cppstd=20\ncompiler.runtime=dynamic\n"
        "compiler.runtime_type=Release\n")


class MsvcBuildProfileTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "build/validation/msvc-build-profile-tests"
        parent.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=parent)
        self.root = Path(temporary.name).resolve()
        self.assertTrue(self.root.is_relative_to(parent.resolve()))
        self.addCleanup(temporary.cleanup)
        self.base = self.root / "repo with spaces/windows.profile"
        self.base.parent.mkdir()
        self.base.write_text(BASE, encoding="ascii", newline="\n")
        self.vs = self.root / "VS install"
        self.output = self.root / "profiles"
        self.process_guard = contextlib.ExitStack()
        self.addCleanup(self.process_guard.close)
        self.process_guard.enter_context(patch("subprocess.run", side_effect=AssertionError("no process")))
        self.process_guard.enter_context(patch("os.system", side_effect=AssertionError("no process")))

    def toolset(self, version, *, missing=()):
        root = self.vs / "VC/Tools/MSVC" / version
        for relative in ("bin/Hostx64/x64/cl.exe", "bin/Hostx64/x64/link.exe",
                         "include/yvals_core.h", "lib/x64/msvcprt.lib", "lib/x64/libcpmt.lib"):
            if relative in missing:
                continue
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(("synthetic " + version + " " + relative).encode())
        return root

    def prepare(self, **kwargs):
        return subject.prepare(self.base, self.vs, self.output, **kwargs)

    def test_windows_fd_path_ctime_difference_is_not_a_replacement(self):
        self.toolset("14.43.34808")
        import os
        actual_fstat = os.fstat
        class FdStat:
            def __init__(self, found):
                self.found = found
                self.st_ctime_ns = found.st_ctime_ns + 1000000
            def __getattr__(self, name):
                return getattr(self.found, name)
        with patch.object(subject.os, "fstat", side_effect=lambda fd: FdStat(actual_fstat(fd))):
            result = self.prepare()
        self.assertEqual(result["selection"]["compiler.update"], "3")

    def test_update3_then_highest_update4_and_numeric_patch_selection(self):
        self.toolset("14.43.34808")
        first = self.prepare()
        self.assertEqual(first["selection"]["compiler.version"], "194")
        self.assertEqual(first["selection"]["compiler.update"], "3")
        self.toolset("14.44.9")
        self.toolset("14.44.10")
        second = self.prepare()
        self.assertEqual(second["selection"]["selected_toolset_version"], "14.44.10")
        self.assertEqual(second["selection"]["compiler.update"], "4")
        self.assertNotEqual(first["identity"], second["identity"])
        self.assertFalse(second["selection"]["compiler_execution_verified"])
        self.assertFalse(second["selection"]["rt_version_verified"])
        self.assertTrue(Path(first["profile"]).is_file())

    def test_base_family_controls_selection_and_static_runtime_library(self):
        self.toolset("14.43.34808")
        self.toolset("14.50.10000")
        self.assertEqual(self.prepare()["selection"]["selected_toolset_version"], "14.43.34808")
        self.base.write_text(BASE.replace("194", "193").replace("dynamic", "static"))
        self.toolset("14.39.10000")
        result = self.prepare()
        self.assertEqual(result["selection"]["compiler.version"], "193")
        self.assertEqual(result["selection"]["compiler.update"], "9")
        owner = json.loads(Path(result["owner"]).read_text())
        self.assertTrue(owner["critical_files"]["stl_library"]["path"].endswith("/libcpmt.lib"))

    def test_incomplete_newer_toolset_is_not_selected_and_missing_all_refuses(self):
        self.toolset("14.43.34808")
        self.toolset("14.44.10000", missing=("bin/Hostx64/x64/link.exe",))
        self.assertEqual(self.prepare()["selection"]["compiler.update"], "3")
        (self.vs / "VC/Tools/MSVC/14.43.34808/bin/Hostx64/x64/link.exe").unlink()
        with self.assertRaisesRegex(ValueError, "no complete"):
            self.prepare()

    def test_wrong_family_arch_runtime_and_custom_base_refuse(self):
        self.toolset("14.50.10000")
        with self.assertRaisesRegex(ValueError, "no complete"):
            self.prepare()
        self.toolset("14.43.34808")
        for before, after in (("arch=x86_64", "arch=x86"), ("compiler=msvc", "compiler=clang"),
                              ("compiler.runtime=dynamic", "compiler.runtime=unknown")):
            self.base.write_text(BASE.replace(before, after))
            with self.subTest(after=after), self.assertRaises(ValueError):
                self.prepare()
        for text in (BASE + "\n[conf]\nother=value\n", BASE + "\ncompiler=msvc\n",
                     BASE.replace("include(base)", "{% set target = 194 %}"),
                     BASE + "\ncompiler.update=3\n"):
            self.base.write_text(text)
            with self.subTest(text=text[-50:]), self.assertRaises(ValueError):
                self.prepare()
        self.base.write_text(BASE)
        for kwargs in ({"arch": "x64"}, {"allow_create": 1}, {"allow_create": None}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                self.prepare(**kwargs)

    def test_two_file_cohort_reuses_exact_bytes_and_timestamps(self):
        self.toolset("14.43.34808")
        first = self.prepare()
        cohort = Path(first["profile"]).parent
        before = {path.name: (path.read_bytes(), path.stat().st_mtime_ns) for path in cohort.iterdir()}
        self.assertEqual(set(before), {"build.profile", "owner.json"})
        self.assertEqual(self.prepare(allow_create=False), first)
        self.assertEqual(self.prepare(), first)
        self.assertEqual(before, {path.name: (path.read_bytes(), path.stat().st_mtime_ns)
                                  for path in cohort.iterdir()})
        profile = Path(first["profile"]).read_bytes()
        self.assertNotIn(b"\r", profile)
        self.assertTrue(profile.endswith(b"\n"))
        self.assertIn(("include(" + self.base.as_posix() + ")\n").encode(), profile)
        self.assertIn(("installation_path=" + self.vs.as_posix()).encode(), profile)

    def test_missing_or_changed_cohort_audit_never_writes(self):
        toolset = self.toolset("14.43.34808")
        with self.assertRaisesRegex(ValueError, "read-only audit"):
            self.prepare(allow_create=False)
        self.assertFalse(self.output.exists())
        first = self.prepare()
        before = {path.name for path in self.output.iterdir()}
        (toolset / "bin/Hostx64/x64/cl.exe").write_bytes(b"changed compiler bytes")
        with self.assertRaisesRegex(ValueError, "read-only audit"):
            self.prepare(allow_create=False)
        self.assertEqual(before, {path.name for path in self.output.iterdir()})
        second = self.prepare()
        self.assertNotEqual(first["identity"], second["identity"])
        owner = json.loads(Path(second["owner"]).read_text())
        self.assertNotEqual(owner["critical_files"]["cl"]["sha256"],
                            json.loads(Path(first["owner"]).read_text())["critical_files"]["cl"]["sha256"])

    def test_existing_mutated_or_unowned_cohort_is_never_repaired(self):
        self.toolset("14.43.34808")
        result = self.prepare()
        profile, owner = Path(result["profile"]), Path(result["owner"])
        original = profile.read_bytes()
        profile.write_bytes(b"corrupted profile\n")
        for create in (True, False):
            with self.subTest(create=create), self.assertRaisesRegex(ValueError, "refusing repair"):
                self.prepare(allow_create=create)
        self.assertEqual(profile.read_bytes(), b"corrupted profile\n")
        profile.write_bytes(original)
        intruder = owner.parent / "foreign.txt"
        intruder.write_bytes(b"unowned")
        with self.assertRaisesRegex(ValueError, "unowned"):
            self.prepare()
        self.assertEqual(intruder.read_bytes(), b"unowned")
        intruder.unlink()
        owner.write_bytes(b"{}\n")
        with self.assertRaisesRegex(ValueError, "refusing repair"):
            self.prepare()

    def test_empty_preexisting_cohort_refuses_repair(self):
        self.toolset("14.43.34808")
        result = self.prepare()
        Path(result["profile"]).unlink()
        Path(result["owner"]).unlink()
        with self.assertRaisesRegex(ValueError, "unknown/missing"):
            self.prepare()
        self.assertEqual(list(Path(result["profile"]).parent.iterdir()), [])

    def test_raced_unowned_cohort_is_not_populated_or_repaired(self):
        self.toolset("14.43.34808")
        result = self.prepare()
        cohort = Path(result["profile"]).parent
        self.assertTrue(cohort.resolve().is_relative_to(self.root))
        Path(result["profile"]).unlink()
        Path(result["owner"]).unlink()
        cohort.rmdir()
        actual_mkdir = Path.mkdir
        def race(path, *args, **kwargs):
            if path == cohort:
                actual_mkdir(path, *args, **kwargs)
                raise FileExistsError("another owner reserved this directory")
            return actual_mkdir(path, *args, **kwargs)
        with patch.object(Path, "mkdir", race), self.assertRaisesRegex(ValueError, "unknown/missing"):
            self.prepare()
        self.assertEqual(list(cohort.iterdir()), [])

    def test_profile_path_injection_is_refused(self):
        self.vs = self.root / "VS {{untrusted}}"
        self.toolset("14.43.34808")
        with self.assertRaisesRegex(ValueError, "plain profile"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_relocation_changes_exact_pins_and_identity(self):
        self.toolset("14.43.34808")
        first = self.prepare()
        moved = self.root / "relocated repo (copy)"
        moved.mkdir()
        new_base = moved / "windows.profile"
        new_base.write_bytes(self.base.read_bytes())
        new_vs = moved / "VS"
        shutil.copytree(self.vs, new_vs)
        second = subject.prepare(new_base, new_vs, moved / "profiles")
        self.assertNotEqual(first["identity"], second["identity"])
        payload = Path(second["profile"]).read_text()
        self.assertIn(new_base.as_posix(), payload)
        self.assertIn(new_vs.as_posix(), payload)
        self.assertNotIn(self.vs.as_posix(), payload)
        # Conan's installed parser strips only the final include delimiter.
        # Keep the filesystem assertions above even when Conan is unavailable.
        try:
            from conan.internal.api.profile.profile_loader import _ProfileParser
        except ImportError:
            _ProfileParser = None
        if _ProfileParser is not None:
            self.assertEqual(_ProfileParser(payload).includes, [new_base.as_posix()])
        explicit_include = BASE.replace("include(base)", "include(" +
                                        (moved / "base.profile").as_posix() + ")")
        self.assertEqual(subject._base(explicit_include, "x86_64"), ("194", "dynamic"))

    def test_jinja_and_control_character_paths_still_refuse(self):
        self.toolset("14.43.34808")
        injected = self.root / "repo {{untrusted}}" / "windows.profile"
        injected.parent.mkdir()
        injected.write_text(BASE)
        with self.assertRaisesRegex(ValueError, "represented in include"):
            subject.prepare(injected, self.vs, self.output)
        for character in ("\r", "\n", "\t", "\x01", "\x7f"):
            with self.subTest(character=repr(character)), \
                    self.assertRaisesRegex(ValueError, "control characters"):
                subject.prepare(Path(str(self.base) + character), self.vs, self.output)
        self.assertFalse(self.output.exists())

    def test_aliases_and_traversal_refuse_without_outputs(self):
        toolset = self.toolset("14.43.34808")
        actual_lstat = Path.lstat
        for target in (self.vs, toolset, toolset / "bin/Hostx64/x64/cl.exe"):
            def redirected(path, *, selected=target):
                found = actual_lstat(path)
                if path == selected:
                    return SimpleNamespace(st_mode=found.st_mode, st_file_attributes=0x400)
                return found
            with self.subTest(target=target), patch.object(Path, "lstat", redirected), \
                    self.assertRaisesRegex(ValueError, "aliased"):
                self.prepare()
        self.assertFalse(self.output.exists())
        with self.assertRaisesRegex(ValueError, "traversal"):
            subject.prepare(self.base, self.vs, self.root / "profiles/../other")
        with self.assertRaisesRegex(ValueError, "outside the VS"):
            subject.prepare(self.base, self.vs, self.vs / "generated")

    def test_entry_per_file_profile_and_total_budgets(self):
        self.toolset("14.43.34808")
        self.toolset("14.44.10000")
        for name, value, expected in (("MAX_ENTRIES", 1, "entry budget"),
                                      ("MAX_FILE_BYTES", 1, "byte budget"),
                                      ("MAX_PROFILE_BYTES", 1, "byte budget"),
                                      ("MAX_TOTAL_BYTES", 1, "total input byte")):
            with self.subTest(name=name), patch.object(subject, name, value), \
                    self.assertRaisesRegex(ValueError, expected):
                self.prepare()
        self.assertFalse(self.output.exists())

    def test_newer_complete_toolset_appearing_during_admission_refuses(self):
        toolset = self.toolset("14.43.34808")
        target = toolset / "bin/Hostx64/x64/cl.exe"
        actual = subject._observe
        added = False
        def observe(path, limit, contents=False):
            nonlocal added
            result = actual(path, limit, contents)
            if Path(path) == target and not added:
                added = True
                self.toolset("14.44.10000")
            return result
        with patch.object(subject, "_observe", side_effect=observe), \
                self.assertRaisesRegex(ValueError, "selection changed"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_selected_input_mutation_before_admission_refuses(self):
        toolset = self.toolset("14.43.34808")
        target = toolset / "bin/Hostx64/x64/cl.exe"
        actual = subject._observe
        changed = False
        def observe(path, limit, contents=False):
            nonlocal changed
            result = actual(path, limit, contents)
            if Path(path) == target and not changed:
                changed = True
                target.write_bytes(b"changed during admission")
            return result
        with patch.object(subject, "_observe", side_effect=observe), \
                self.assertRaisesRegex(ValueError, "changed before"):
            self.prepare()
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
