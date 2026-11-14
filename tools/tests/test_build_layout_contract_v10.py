"""Preset layout controls, with real CMake differential checks when available.

CMake fixtures use LANGUAGES NONE and temporary build trees. They do not compile
JCE, execute a Windows compiler, launch an IDE, or certify a target runtime.
"""
from __future__ import annotations

import copy
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("jce_layout_contract_v10", ROOT / "tools/build/layout_contract.py")
OWNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(OWNER)


def fixture(root):
    # Use the shipped recipe instead of regenerating the gate's expectations.
    return json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))


class CanonicalPresetLayoutV10(unittest.TestCase):
    def test_all_presets_and_modes_have_independent_cmake_directories(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = fixture(root)
            self.assertEqual(OWNER.check_presets(root, data), [])
            paths = [p["binaryDir"] for p in data["configurePresets"] if not p.get("hidden")]
            self.assertEqual(len(paths), len(set(paths)))
            for p in paths:
                if "/desktop/" not in p:
                    continue
                self.assertIn("/obj/preset/", p)
            self.assertEqual((len(data["configurePresets"]), len(paths), len(data["buildPresets"])), (50, 42, 42))

    def test_dist_cannot_inherit_release_binary_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = fixture(root)
            dist = next(x for x in data["configurePresets"] if x["name"] == "linux-x64-dist")
            dist["binaryDir"] = "${sourceDir}/build/desktop/linux-x64/obj/preset/release"
            errors = OWNER.check_presets(root, data)
            self.assertTrue(any("linux-x64-dist: binaryDir" in e for e in errors), errors)

    def test_conan_cannot_come_from_another_target_or_embedded_editor(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = fixture(root)
            debug = next(x for x in data["configurePresets"] if x["name"] == "windows-x64-debug")
            debug["cacheVariables"]["CMAKE_TOOLCHAIN_FILE"] = (
                "${sourceDir}/build/desktop/windows-x64/conan/release/build/Debug/generators/conan_toolchain.cmake")
            errors = OWNER.check_presets(root, data)
            self.assertTrue(any("windows-x64-debug: CMAKE_TOOLCHAIN_FILE" in e for e in errors), errors)

    def test_inheritance_and_missing_binary_dir_are_not_implicitly_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = fixture(root)
            dist = next(x for x in data["configurePresets"] if x["name"] == "macos-arm64-dist")
            del dist["binaryDir"]
            dist["inherits"] = "macos-arm64-release"
            errors = OWNER.check_presets(root, data)
            self.assertTrue(any("macos-arm64-dist: binaryDir" in e for e in errors), errors)

    def test_duplicate_and_invalid_build_preset_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = fixture(root)
            data["configurePresets"].append(copy.deepcopy(data["configurePresets"][0]))
            data["buildPresets"].append({"name": "broken", "configurePreset": "does-not-exist"})
            errors = OWNER.check_presets(root, data)
            self.assertTrue(any("duplicate configurePreset" in e for e in errors), errors)
            self.assertTrue(any("nonexistent" in e for e in errors), errors)

    def test_same_cache_alias_and_missing_native_metadata_are_rejected(self):
        cases = (
            ("_windows-x64", "condition", None, "host condition"),
            ("_windows-x64", "architecture", {"value": "x64", "strategy": "set"}, "architecture"),
            ("_windows-x64", "toolset", {"value": "v143", "strategy": "external"}, "toolset"),
            ("windows-x64-release", "displayName", "", "displayName"),
        )
        for name, key, value, message in cases:
            with self.subTest(name=name, key=key):
                data = fixture(ROOT)
                next(x for x in data["configurePresets"] if x["name"] == name)[key] = value
                self.assertTrue(any(message in e for e in OWNER.check_presets(ROOT, data)))
        data = fixture(ROOT)
        data["configurePresets"].append({"name": "local-copy", "displayName": "Another label",
                                        "inherits": "windows-x64-release"})
        self.assertTrue(any("shared binaryDir" in e for e in OWNER.check_presets(ROOT, data)))

    def test_purpose_build_visibility_and_schema_minimum_are_not_optional(self):
        for name, variable, value in (
            ("windows-x64-release", "JCE_DELIVERY_PROFILE", "embedded"),
            ("windows-x64-release-sdk", "JCE_BUILD_EDITOR", "ON"),
        ):
            with self.subTest(name=name, variable=variable):
                data = fixture(ROOT)
                next(x for x in data["configurePresets"] if x["name"] == name)["cacheVariables"][variable] = value
                self.assertTrue(any("purpose" in e for e in OWNER.check_presets(ROOT, data)))
        data = fixture(ROOT)
        data["cmakeMinimumRequired"]["minor"] = 20
        data["buildPresets"][0].pop("condition")
        errors = OWNER.check_presets(ROOT, data)
        self.assertTrue(any("3.25" in e for e in errors))
        self.assertTrue(any("buildPreset" in e and "host condition" in e for e in errors))

    def test_vendor_namespaces_and_partial_architecture_do_not_mutate_inputs(self):
        source = {
            "first": {"name": "first", "hidden": True, "displayName": "First", "description": "Base",
                "architecture": {"value": "x64", "strategy": "external"},
                "vendor": {"example.org/a/1": {"first": True}}},
            "second": {"name": "second", "vendor": {"example.org/a/1": {"second": True},
                                                       "example.org/b/1": {"other": True}}},
            "child": {"name": "child", "inherits": ["first", "second"],
                      "architecture": {"strategy": "set"}},
        }
        before = copy.deepcopy(source)
        resolved = OWNER._resolve(source, "child")
        self.assertEqual(resolved["vendor"], {"example.org/a/1": {"first": True}, "example.org/b/1": {"other": True}})
        self.assertEqual(resolved["architecture"], {"value": "x64", "strategy": "set"})
        self.assertFalse({"hidden", "displayName", "description", "inherits"} & resolved.keys())
        self.assertEqual(source, before)

    def test_inventory_preserves_legacy_and_xp_trees(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = root / "build/desktop"
            for path in ("windows-x64", "windows-x64-conan", "windows-x64-debug",
                         "windows-x86-xp", "windows-arm64"):
                (base / path).mkdir(parents=True)
            (base / "windows-x64/CMakeCache.txt").write_text("legacy marker")
            before = sorted(x.as_posix() for x in base.iterdir())
            rows = {Path(x["entry"]).name: x["status"] for x in OWNER.inventory(root)}
            after = sorted(x.as_posix() for x in base.iterdir())
            self.assertEqual(before, after)
            self.assertEqual(rows["windows-x64"], "TARGET_ROOT_WITH_LEGACY_CACHE")
            self.assertEqual(rows["windows-x64-conan"], "LEGACY_FLAT_CACHE_KEEP_IN_PLACE")
            self.assertEqual(rows["windows-x64-debug"], "LEGACY_FLAT_CACHE_KEEP_IN_PLACE")
            self.assertEqual(rows["windows-x86-xp"], "TARGET_ROOT")

    def test_real_repo_owner_runtime_and_json_match(self):
        # This is a source/policy check, not a build or a historical-cache repair.
        self.assertEqual(OWNER.audit(ROOT), [])


@unittest.skipUnless(shutil.which("cmake"), "CMake is unavailable; native preset controls were not executed")
class NativeCMakePresetControls(unittest.TestCase):
    def run_cmake(self, root, *args, success=True):
        env = dict(os.environ)
        for key in tuple(env):
            if key.startswith("JCE_PRESET_TEST_"):
                del env[key]
        completed = subprocess.run([shutil.which("cmake"), *args], cwd=root, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", timeout=60)
        if success:
            self.assertEqual(completed.returncode, 0, completed.stdout)
        else:
            self.assertNotEqual(completed.returncode, 0, completed.stdout)
        return completed.stdout

    def write_fixture(self, root, presets, cmake):
        (root / "CMakePresets.json").write_text(json.dumps(presets), encoding="utf-8")
        (root / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.25)\nproject(PresetControl NONE)\n" + cmake,
                                           encoding="utf-8")

    def test_inheritance_matches_actual_cmake_cache_environment_and_visibility(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            entries = {
                "first": {"name": "first", "hidden": True, "displayName": "Do not inherit this label",
                    "description": "Do not inherit this description", "generator": "Ninja" if shutil.which("ninja") else "Unix Makefiles",
                    "binaryDir": "${sourceDir}/out", "cacheVariables": {"VALUE": "first", "LEFT": "left", "ERASE": "remove"},
                    "environment": {"JCE_PRESET_TEST_VALUE": "first", "JCE_PRESET_TEST_LEFT": "left", "JCE_PRESET_TEST_ERASE": "remove"}},
                "second": {"name": "second", "hidden": True, "cacheVariables": {"VALUE": "second", "RIGHT": "right"},
                    "environment": {"JCE_PRESET_TEST_VALUE": "second", "JCE_PRESET_TEST_RIGHT": "right"}},
                "child": {"name": "child", "inherits": ["first", "second"], "cacheVariables": {"ERASE": None},
                          "environment": {"JCE_PRESET_TEST_ERASE": None}},
            }
            self.write_fixture(root, {"version": 6, "configurePresets": list(entries.values())},
                'file(WRITE "${CMAKE_BINARY_DIR}/observed.txt" "${VALUE}|${LEFT}|${RIGHT}|${ERASE}|'
                '$ENV{JCE_PRESET_TEST_VALUE}|$ENV{JCE_PRESET_TEST_LEFT}|$ENV{JCE_PRESET_TEST_RIGHT}|$ENV{JCE_PRESET_TEST_ERASE}")\n')
            listing = self.run_cmake(root, "--list-presets")
            self.assertIn('"child"', listing)
            self.assertNotIn("Do not inherit", listing)
            self.run_cmake(root, "--preset", "child")
            observed = (root / "out/observed.txt").read_text()
            self.assertEqual(observed, "first|left|right||first|left|right|")
            resolved = OWNER._resolve(entries, "child")
            self.assertEqual(resolved["cacheVariables"]["VALUE"], observed.split("|")[0])
            self.assertEqual(resolved["environment"]["JCE_PRESET_TEST_VALUE"], observed.split("|")[4])
            self.assertIsNone(resolved["cacheVariables"]["ERASE"])
            self.assertIsNone(resolved["environment"]["JCE_PRESET_TEST_ERASE"])
            self.assertFalse({"hidden", "displayName", "description"} & resolved.keys())

    def test_project_configure_and_build_lists_follow_the_actual_host(self):
        host = platform.system()
        prefixes = {"Windows": ("windows-x86", "windows-x64", "windows-arm64"),
                    "Linux": ("linux-x64", "linux-arm64"), "Darwin": ("macos-x64", "macos-arm64")}
        if host not in prefixes:
            self.skipTest("This host is outside the project's existing preset catalog")
        expected = {"host-release", "jni-release", "android-arm64-release", "wasm-release", "wasm-dist"}
        expected.update(target + "-" + kind for target in prefixes[host]
                        for kind in ("release", "debug", "dist", "release-sdk", "dist-sdk"))
        if host == "Windows":
            expected.add("windows-x64-asan")
        elif host == "Darwin":
            expected.add("ios-arm64-release")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.write_fixture(root, fixture(ROOT), "")
            for kind, names in (("configure", expected), ("build", {"build-" + name for name in expected})):
                listing = self.run_cmake(root, "--list-presets=" + kind)
                observed = set(re.findall(r'^\s+"([^"]+)"', listing, re.MULTILINE))
                self.assertEqual(observed, names, listing)
            # A build preset does not inherit its configure preset's condition.
            data = fixture(ROOT)
            for entry in data["buildPresets"]:
                entry.pop("condition", None)
            self.write_fixture(root, data, "")
            listing = self.run_cmake(root, "--list-presets=build")
            observed = set(re.findall(r'^\s+"([^"]+)"', listing, re.MULTILINE))
            self.assertGreater(len(observed), len(expected), listing)

    @unittest.skipUnless(shutil.which("ninja"), "Ninja is unavailable; external metadata controls were not executed")
    def test_windows_ninja_metadata_is_external_with_ordinary_refusal_controls(self):
        # Windows conditions are enabled only inside this LANGUAGES NONE fixture.
        # This tests real CMake/Ninja metadata handling, not a Windows compiler.
        for changed in (None, "architecture", "toolset"):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                data = fixture(ROOT)
                by_name = {entry["name"]: entry for entry in data["configurePresets"]}
                base = by_name["_windows-x64"]
                base["condition"] = True
                if changed:
                    base[changed]["strategy"] = "set"
                toolchain = root / "build/desktop/windows-x64/conan/preset/build/Release/generators/conan_toolchain.cmake"
                toolchain.parent.mkdir(parents=True)
                toolchain.write_text("# Empty fixture toolchain; no compiler or Conan package.\n")
                self.write_fixture(root, data, "")
                output = self.run_cmake(root, "--preset", "windows-x64-release", success=changed is None)
                if changed is not None:
                    self.assertIn("does not support", output)


if __name__ == "__main__":
    unittest.main()
