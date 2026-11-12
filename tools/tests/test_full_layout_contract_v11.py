"""Pure Python path, ownership, and negative controls; NOT native build tests."""
from __future__ import annotations
import copy
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("jce_full_layout_v11_test", ROOT / "tools/build/full_layout_contract.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class FullBuildLayoutV11Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        cls.roots = json.loads((ROOT / "contracts/build-output-roots.json").read_text(encoding="utf-8"))

    def test_current_platform_presets_have_disjoint_binary_trees(self):
        self.assertEqual(GATE.check_presets(ROOT, self.presets), [])
        resolved = {p["name"]: p for p in self.presets["configurePresets"]}
        actual = [GATE._res(resolved, name)["binaryDir"] for name in GATE.PLATFORM_PRESETS]
        self.assertEqual(len(actual), len(set(actual)))
        self.assertTrue(all(name.startswith("${sourceDir}/build/") for name in actual))

    def test_full_source_parity_and_registry(self):
        self.assertEqual(GATE.check_sources(ROOT), [])
        self.assertEqual(GATE.check_roots(ROOT, self.roots), [])
        self.assertEqual(GATE.audit(ROOT), [])

    def test_web_release_dist_overlap_is_rejected(self):
        bad = copy.deepcopy(self.presets)
        p = {x["name"]: x for x in bad["configurePresets"]}
        p["wasm-dist"]["binaryDir"] = p["wasm-release"]["binaryDir"]
        failures = GATE.check_presets(ROOT, bad)
        self.assertTrue(any("wasm-dist" in x and ("binaryDir" in x or "shares" in x) for x in failures), failures)

    def test_wrong_implicit_conan_is_rejected(self):
        bad = copy.deepcopy(self.presets)
        p = {x["name"]: x for x in bad["configurePresets"]}
        p["ios-arm64-release"]["cacheVariables"]["CMAKE_TOOLCHAIN_FILE"] = "${sourceDir}/build/mobile/ios-arm64-conan/build/Release/generators/conan_toolchain.cmake"
        self.assertTrue(any("ios-arm64-release: Conan" in x for x in GATE.check_presets(ROOT,bad)))

    def test_missing_registered_build_family_rejected(self):
        bad = copy.deepcopy(self.roots)
        bad["roots"].pop("consumers")
        self.assertIn("missing registered build root: consumers", GATE.check_roots(ROOT,bad))

    def test_missing_web_dist_build_preset_rejected(self):
        bad = copy.deepcopy(self.presets)
        bad["buildPresets"] = [x for x in bad["buildPresets"] if x["name"] != "build-wasm-dist"]
        self.assertIn("missing or invalid build preset build-wasm-dist", GATE.check_presets(ROOT,bad))

    def test_mutated_platform_batch_detected_without_touching_repository(self):
        with tempfile.TemporaryDirectory(prefix="jce-v11-mutation-") as temp:
            root = Path(temp)
            for rel in ("tools/build/build-android.bat", "tools/build/build-web.bat",
                        "tools/build/build-ios-arm64.sh", "tools/build/jce.py",
                        "tools/build/android/build.gradle", "tools/build/android/app/build.gradle"):
                src = ROOT / rel
                dst = root / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(src,dst)
            web = root / "tools/build/build-web.bat"
            text = web.read_text(encoding="utf-8")
            self.assertIn("obj\\platform\\%VARIANT%", text)
            web.write_text(text.replace("obj\\platform\\%VARIANT%", "obj\\platform\\release", 1), encoding="utf-8")
            self.assertTrue(any("build-web.bat" in x for x in GATE.check_sources(root)))

    def test_legacy_windows_caches_are_not_moved_or_reclassified_as_current(self):
        with tempfile.TemporaryDirectory(prefix="jce-v11-inventory-") as temp:
            root = Path(temp)
            policy = root / "contracts/build-output-roots.json"
            policy.parent.mkdir(parents=True)
            policy.write_text(json.dumps(self.roots),encoding="utf-8")
            existing = root / "build/desktop/windows-x64-debug"
            existing.mkdir(parents=True)
            (existing / "CMakeCache.txt").write_text("old cache remains", encoding="utf-8")
            (root / "build/mobile/android-arm64").mkdir(parents=True)
            paths = {p["path"]: p["status"] for p in GATE.inventory(root)}
            self.assertEqual(paths["build/desktop/windows-x64-debug"], "LEGACY_FLAT_CACHE_KEEP_IN_PLACE")
            self.assertEqual(paths["build/mobile/android-arm64"], "TARGET_OWNER")
            self.assertEqual((existing / "CMakeCache.txt").read_text(), "old cache remains")

    def test_unregistered_or_aliased_roots_are_visible(self):
        with tempfile.TemporaryDirectory(prefix="jce-v11-root-") as temp:
            root = Path(temp)
            policy = root / "contracts/build-output-roots.json"
            policy.parent.mkdir(parents=True)
            policy.write_text(json.dumps(self.roots),encoding="utf-8")
            bad = root / "build/random-build"
            bad.mkdir(parents=True)
            self.assertIn({"path":"build/random-build","status":"UNREGISTERED_REVIEW"},GATE.inventory(root))


if __name__ == "__main__":
    unittest.main()
