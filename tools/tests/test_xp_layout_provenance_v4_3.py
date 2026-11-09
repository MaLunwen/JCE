"""Pure-Python XP v4.3 provenance tests; does not build or run an XP executable."""
from pathlib import Path
import hashlib
import importlib.util
import json
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("xp_identity_v43", ROOT / "tools/build/xp_editor.py")
XP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(XP)


class XPVariantConanIdentity(unittest.TestCase):
    def fake_files(self, home, variant):
        root = home / "build/desktop/windows-x64-xp"
        cache = root / "obj" / variant
        conan = root / "conan" / variant
        cache.mkdir(parents=True, exist_ok=True)
        conan.mkdir(parents=True, exist_ok=True)
        files = {
            "exe": cache / "sample.exe",
            "cmake": cache / "CMakeCache.txt",
            "profile": cache / "xp-host.profile",
            "build_profile": cache / "native-build.profile",
            "graph": conan / "xp-graph.json",
            "generation": conan / "xp-generation.json",
        }
        for key, path in files.items():
            path.write_bytes((variant + ":" + key).encode("ascii"))
        return cache, conan, files

    def identity(self, cache, files):
        return XP._built_identity(files["exe"], cache, "a" * 64,
                                  files["profile"], files["build_profile"])

    def test_both_variants_hash_own_conan_graph_and_generation(self):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp)
            observations = {}
            for variant in ("release", "dist"):
                cache, conan, files = self.fake_files(home, variant)
                result = self.identity(cache, files)
                self.assertEqual(result["graph_sha256"], hashlib.sha256(files["graph"].read_bytes()).hexdigest())
                self.assertEqual(result["generation_sha256"], hashlib.sha256(files["generation"].read_bytes()).hexdigest())
                self.assertEqual(XP._xp_conan_identity_root(cache), conan)
                observations[variant] = result
            self.assertNotEqual(observations["release"]["graph_sha256"],
                                observations["dist"]["graph_sha256"])
            self.assertNotEqual(observations["release"]["generation_sha256"],
                                observations["dist"]["generation_sha256"])

    def test_old_nested_conan_cannot_supply_a_new_variant_receipt(self):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp)
            cache, conan, files = self.fake_files(home, "dist")
            old = cache / "conan"
            old.mkdir()
            (old / "xp-graph.json").write_bytes(b"old fixture data")
            files["graph"].unlink()
            with self.assertRaises(FileNotFoundError):
                self.identity(cache, files)

    def test_nonvariant_cache_identity_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for bad in (root / "obj", root / "obj-debug", root / "obj/release/nested"):
                with self.subTest(bad=bad), self.assertRaisesRegex(ValueError, "obj/<variant>"):
                    XP._xp_conan_identity_root(bad)

    def test_test_fixture_recomputes_stock_metadata_without_resetting_codec_option(self):
        source = (ROOT / "tools/tests/test_xp_editor.py").read_text(encoding="utf-8")
        self.assertIn("values.setdefault(\"JCE_ENABLE_PATENTED_CODECS\", \"ON\")", source)
        self.assertIn("for generated_stock_key in (", source)
        self.assertIn("values.pop(generated_stock_key, None)", source)
        self.assertIn("self.assertEqual(binary_dir, self.cache.resolve()", source)
        begin = source.index("    def test_release_codec_preference_and_dist_cache_are_separate(self):")
        end = source.index("    def test_release_and_dist_invalid_codec_modes_refuse_before_native_launch(self):", begin)
        block = source[begin:end]
        self.assertNotIn('self.assertEqual(self.latest()["capabilities"]["patented_codecs"]["effective"], "ON")\n', block)
        self.assertGreaterEqual(block.count('"Release configured CMake values: "'), 1)


if __name__ == "__main__":
    unittest.main()
