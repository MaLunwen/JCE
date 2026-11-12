"""Pure-Python positive/negative controls for desktop preset layout v10.

These tests never run CMake, Conan, an editor, an SDK or an XP guest.
"""
from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("jce_layout_contract_v10", ROOT / "tools/build/layout_contract.py")
OWNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(OWNER)


def fixture(root):
    presets = []
    for target in OWNER.NATIVE_STEMS:
        for kind in (*OWNER.PRESET_KINDS, "asan") if target == "windows-x64" else OWNER.PRESET_KINDS:
            expected = OWNER.preset_cache_variables(root, target, kind)
            v = "dist" if kind in ("dist", "dist-sdk") else (
                "debug" if kind == "debug" else "asan" if kind == "asan" else "release")
            presets.append({"name": target + "-" + kind,
                            "binaryDir": "${sourceDir}/" + Path(expected["binaryDir"]).relative_to(root).as_posix(),
                            "cacheVariables": {
                                "CMAKE_TOOLCHAIN_FILE": "${sourceDir}/" + Path(expected["CMAKE_TOOLCHAIN_FILE"]).relative_to(root).as_posix(),
                                "CMAKE_BUILD_TYPE": "Debug" if kind == "debug" else "Release",
                                "JCE_BUILD_VARIANT": v}})
    return {"configurePresets": presets,
            "buildPresets": [{"name": "build-" + x["name"], "configurePreset": x["name"]}
                             for x in presets]}


class CanonicalPresetLayoutV10(unittest.TestCase):
    def test_all_presets_and_modes_have_independent_cmake_directories(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data = fixture(root)
            self.assertEqual(OWNER.check_presets(root, data), [])
            paths = [p["binaryDir"] for p in data["configurePresets"]]
            self.assertEqual(len(paths), len(set(paths)))
            for p in paths:
                self.assertIn("/obj/preset/", p)

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


if __name__ == "__main__":
    unittest.main()
