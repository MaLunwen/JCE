"""Pure failure/positive controls: not a network or Conan execution claim."""
from __future__ import annotations

import importlib.util
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("jce_ci_seed_lua_source", ROOT / "tools/build/ci_seed_lua_source.py")
seed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seed)


class ConanSourceSeedTests(unittest.TestCase):
    def test_requires_ci_and_owned_workspace_home(self):
        with tempfile.TemporaryDirectory() as dirname:
            workspace = Path(dirname)
            good = {"CI": "true", "GITHUB_ACTIONS": "true",
                    "GITHUB_WORKSPACE": str(workspace),
                    "CONAN_HOME": str(workspace / ".conan2")}
            self.assertEqual(seed.ci_paths(good)[0], workspace / ".conan2")
            for override in ({"CI": "false"}, {"CONAN_HOME": str(workspace / "elsewhere")},
                             {"GITHUB_WORKSPACE": ""}):
                with self.assertRaises(ValueError):
                    seed.ci_paths({**good, **override})

    def test_config_is_idempotent_and_preserves_other_keys(self):
        with tempfile.TemporaryDirectory() as dirname:
            home = Path(dirname)
            (home / "global.conf").write_text("core:non_interactive=True\n", encoding="utf-8")
            seed.set_source_cache(home, home / "verified-sources")
            once = (home / "global.conf").read_bytes()
            seed.set_source_cache(home, home / "verified-sources")
            self.assertEqual(once, (home / "global.conf").read_bytes())
            self.assertIn(b"core:non_interactive=True", once)
            self.assertIn(b"core.sources:download_cache=", once)

    def test_existing_conflict_is_rejected(self):
        with tempfile.TemporaryDirectory() as dirname:
            home = Path(dirname)
            (home / "global.conf").write_text("core.sources:download_cache=/elsewhere\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                seed.set_source_cache(home, home / "verified-sources")

    def test_valid_blob_requires_conan_generated_metadata(self):
        with tempfile.TemporaryDirectory() as dirname:
            cache = Path(dirname)
            payload = b"fixture bytes only"
            sha = hashlib.sha256(payload).hexdigest()
            (cache / sha).write_bytes(payload)
            with self.assertRaisesRegex(ValueError, "metadata"):
                seed.verified_cache_blob(cache, sha)
            (cache / (sha + ".json")).write_text("{}", encoding="ascii")
            self.assertTrue(seed.verified_cache_blob(cache, sha))
            (cache / sha).write_bytes(b"corrupted")
            with self.assertRaisesRegex(ValueError, "SHA256"):
                seed.verified_cache_blob(cache, sha)

    def test_seed_exports_a_recipe_before_expectation_of_cache(self):
        # Offline command-port control: does not claim a real Conan cache hit.
        with tempfile.TemporaryDirectory() as dirname:
            workspace = Path(dirname)
            env = {"CI": "true", "GITHUB_ACTIONS": "true",
                   "GITHUB_WORKSPACE": str(workspace),
                   "CONAN_HOME": str(workspace / ".conan2")}
            with patch.object(seed.shutil, "which", return_value="conan"), \
                 patch.object(seed.subprocess, "run") as runner, \
                 patch.object(seed, "verified_cache_blob", side_effect=[False, True]):
                seed.seed(env)
            args = runner.call_args.args[0]
            self.assertEqual(args, ["conan", "create", ".", "--test-folder="])
            self.assertTrue(runner.call_args.kwargs["check"])
            self.assertEqual(runner.call_args.kwargs["env"], env)

    def test_recipe_is_offline_generated_and_hash_pinned(self):
        recipe = seed.ephemeral_recipe()
        self.assertIn(seed.LUA_548_SHA256, recipe)
        for url in seed.SOURCES:
            self.assertIn(url, recipe)
        self.assertIn("get(self, url=", recipe)


if __name__ == "__main__":
    unittest.main()
