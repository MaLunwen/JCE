"""Static and mocked runtime controls for JCE canonical variant outputs.

These do NOT claim any actual MSVC, LLVM-MinGW-XP, Conan or XP guest execution.
"""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(relative):
    path = ROOT / relative
    spec = importlib.util.spec_from_file_location("jce_layout_v4_" + path.stem, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class NativeEditorVariantLayoutV4(unittest.TestCase):
    def test_modern_layout_is_variant_isolated_even_with_old_caches_present(self):
        impl = load("tools/build/standalone.py")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            target = {"key": "windows-x64", "host": "windows", "arch": "x86_64"}
            old = root / "build/desktop/windows-x64/obj"
            old.mkdir(parents=True)
            (old / "CMakeCache.txt").write_text("old owner not admissible")
            for variant in ("release", "dist"):
                args = SimpleNamespace(variant=variant)
                plan = impl.native_cache_layout(SimpleNamespace(ROOT=root), args, target)
                self.assertEqual(plan["build"], old / variant)
                self.assertEqual(plan["conan"], root / "build/desktop/windows-x64/conan" / variant)
                self.assertEqual(plan["toolchain"], plan["conan"] / "build/Release/generators/conan_toolchain.cmake")
                self.assertFalse(plan["build"].exists())
                self.assertEqual((old / "CMakeCache.txt").read_text(), "old owner not admissible")
                args._native_cache_plan = {**plan, "build": old}
                with self.assertRaisesRegex(ValueError, "identity changed"):
                    impl.native_cache_layout(SimpleNamespace(ROOT=root), args, target)
            with self.assertRaisesRegex(ValueError, "release or dist"):
                impl.native_cache_layout(SimpleNamespace(ROOT=root), SimpleNamespace(variant="debug"), target)

    def test_xp_producer_paths_are_variant_isolated_and_codec_preserved(self):
        text = (ROOT / "tools/build/xp_editor.py").read_text(encoding="utf-8")
        self.assertIn('desktop_target / "obj" / variant', text)
        self.assertIn('desktop_target / "conan" / variant', text)
        self.assertNotIn('"obj-release" if variant', text)
        self.assertIn('candidate = _owned_child(desktop_target / variant / "jce_editor.exe"', text)
        self.assertIn('if variant == "release" and patented_codecs is not None', text)
        self.assertIn('f"-DJCE_BUILD_VARIANT={variant}"', text)
        self.assertIn('"-DJCE_BUILD_SCRIPT_VM_PYTHON=OFF"', text)
        self.assertIn('"-DJCE_BUILD_SCRIPT_CSHARP=OFF"', text)
        self.assertIn('"-DJCE_BUILD_SCRIPT_JAVA=OFF"', text)

    def test_xp_and_modern_physical_layout_match(self):
        impl = load("tools/build/standalone.py")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            for key in ("windows-x64", "windows-x86", "windows-x64-xp", "windows-x86-xp"):
                target = {"key": key, "host": "windows", "arch": "x86_64"}
                for variant in ("release", "dist"):
                    plan = impl.native_cache_layout(SimpleNamespace(ROOT=root), SimpleNamespace(variant=variant), target)
                    self.assertEqual(plan["build"].relative_to(root).as_posix(),
                                     f"build/desktop/{key}/obj/{variant}")
                    self.assertEqual(plan["conan"].relative_to(root).as_posix(),
                                     f"build/desktop/{key}/conan/{variant}")


if __name__ == "__main__":
    unittest.main()
