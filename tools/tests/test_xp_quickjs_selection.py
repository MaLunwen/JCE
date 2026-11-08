"""Graph/export controls for the explicit legacy provider; no native tool runs."""
from pathlib import Path
from types import SimpleNamespace
import importlib.util
import tempfile
import unittest
from unittest.mock import patch

try:
    from conan import ConanFile
    from conan.tools.cmake import CMakeToolchain
except ImportError:
    CONAN_AVAILABLE = False
else:
    CONAN_AVAILABLE = True

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


class XpQuickJsSelectionTests(unittest.TestCase):
    @unittest.skipUnless(CONAN_AVAILABLE, "Installed Conan recipe subject unavailable")
    def test_real_root_requirements_select_one_truthful_provider(self):
        recipe = load("xp_js_root_recipe", "conanfile.py")
        for xp in (False, True):
            with self.subTest(xp=xp):
                required = []
                owner = SimpleNamespace(settings=SimpleNamespace(os="Windows"),
                    _windows_xp_profile=lambda: xp,
                    requires=lambda ref, **kw: required.append(ref))
                recipe.JCEConan.requirements(owner)
                versions = [ref for ref in required if ref.startswith("quickjs-ng/")]
                self.assertEqual(versions, ["quickjs-ng/0.4.1" if xp else "quickjs-ng/0.16.2"])
                self.assertIn("lua/5.4.8", required)
                self.assertIn("mimalloc/3.3.2", required)
                self.assertIn("dav1d/1.5.3", required)
                self.assertEqual("libcurl/8.21.0" in required, not xp)
                self.assertEqual([ref for ref in required if ref.startswith("abseil/")],
                                 ["abseil/20250814.2"] if xp else [])

    @unittest.skipUnless(CONAN_AVAILABLE, "Installed Conan recipe subject unavailable")
    def test_xp_disables_unused_profiler_without_changing_modern_options(self):
        recipe = load("xp_js_config_recipe", "conanfile.py")
        for xp in (False, True):
            with self.subTest(xp=xp):
                options = {key: SimpleNamespace() for key in ("bgfx/*", "tracy/*", "libcurl/*", "zstd/*")}
                owner = SimpleNamespace(settings=SimpleNamespace(os="Windows"),
                    _windows_xp_profile=lambda: xp, options=options)
                with patch.object(recipe, "cross_building", return_value=False):
                    recipe.JCEConan.configure(owner)
                if xp:
                    self.assertIs(options["tracy/*"].enable, False)
                    self.assertIs(options["bgfx/*"].tools, False)
                    self.assertEqual(vars(options["zstd/*"]), {"build_programs": False, "threading": False})
                else:
                    self.assertFalse(vars(options["tracy/*"]))
                    self.assertFalse(vars(options["bgfx/*"]))
                    self.assertFalse(vars(options["zstd/*"]))
                    self.assertEqual(options["libcurl/*"].with_ssl, "schannel")

    def test_xp_exports_selected_pin_and_missing_pin_refuses(self):
        driver = load("xp_js_export_driver", "tools/build/xp_editor.py")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            folder = root / "conan/recipes/quickjs-ng"
            folder.mkdir(parents=True)
            (folder / "conanfile.py").write_text("# recipe\n", encoding="utf-8")
            data = folder / "conandata.yml"
            data.write_text('sources:\n  "0.16.2":\n    url: modern\n  "0.4.1":\n    url: legacy\n', encoding="utf-8")
            self.assertEqual(list(driver._recipe_commands(root)),
                [["conan", "export", str(folder), "--version", "0.4.1"]])
            data.write_text('sources:\n  "0.16.2":\n    url: modern\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "source pin is missing"):
                list(driver._recipe_commands(root))


if __name__ == "__main__":
    unittest.main()
