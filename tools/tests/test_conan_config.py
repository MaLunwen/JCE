"""Conan settings overlay controls; no install, cache database or compiler."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import sys

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

try:
    from conan.internal.model.conf import ConfDefinition
    from conan.internal.model.settings import load_settings_yml
except ModuleNotFoundError:
    CONAN_AVAILABLE = False
else:
    CONAN_AVAILABLE = True

from tools.build.conan_config import prepare_toolchain_home


@unittest.skipUnless(CONAN_AVAILABLE, "Conan 2 settings subject unavailable")
class ConanConfigurationTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "build/tests/0134-conan-config"
        parent.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.base = self.root / "original"
        self.overlay = self.root / "overlay"
        self.base.mkdir()
        # Minimal but real Conan settings schema; user extension must APPEND.
        (self.base / "settings.yml").write_text(
            'compiler:\n  clang:\n    version: ["19", "20", "21"]\n    cppstd: [20]\n', encoding="utf-8")
        (self.base / "settings_user.yml").write_text(
            'compiler:\n  clang:\n    version: ["22"]\ncustom: ["preserved"]\n', encoding="utf-8")
        (self.base / "global.conf").write_text("tools.build:jobs=2\n", encoding="utf-8")
        (self.base / "profiles").mkdir()
        (self.base / "profiles/default").write_text("[settings]\ncompiler=clang\n", encoding="utf-8")
        (self.base / "auth.db").write_bytes(b"private-auth-sentinel")
        (self.base / "remotes.json").write_text(
            '{"remotes":[{"name":"private","url":"https://private.invalid"}]}', encoding="utf-8")
        (self.base / "p").mkdir()
        (self.base / "p/cache.sqlite3").write_bytes(b"database-sentinel")

    def original_hashes(self):
        return {str(p.relative_to(self.base)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in self.base.rglob("*") if p.is_file()}

    def test_actual_conan_merge_adds_23_preserves_original_and_shared_database(self):
        before = self.original_hashes()
        result = prepare_toolchain_home(self.base, self.overlay, "23")
        settings = load_settings_yml(str(self.overlay))
        settings.compiler = "clang"
        for version in ("19", "21", "22", "23"):
            settings.compiler.version = version
            settings.compiler.version.validate()
        self.assertEqual(before, self.original_hashes())
        self.assertEqual(Path(result["storage"]), self.base / "p")
        self.assertFalse((self.overlay / "p").exists())
        self.assertFalse((self.overlay / "auth.db").exists())
        user = json.loads((self.overlay / "settings_user.yml").read_text())
        self.assertEqual(user["custom"], ["preserved"])
        self.assertEqual(user["compiler"]["clang"]["version"], ["22", "23"])
        remotes = json.loads((self.overlay / "remotes.json").read_text())
        self.assertEqual(remotes["remotes"][0]["url"], "https://center2.conan.io")

    def test_respects_explicit_user_storage_and_filters_authentication(self):
        store = self.root / "real custom storage"
        store.mkdir()
        (self.base / "global.conf").write_text(
            "core.cache:storage_path=" + store.as_posix() + "\n"
            "tools.build:jobs=2\nuser.company:token=private-sentinel\n"
            "user.company:headers={'Authorization': 'Bearer private-header'}\n", encoding="utf-8")
        result = prepare_toolchain_home(self.base, self.overlay, "23")
        self.assertEqual(Path(result["storage"]), store)
        text = (self.overlay / "global.conf").read_text()
        self.assertNotIn("private-sentinel", text)
        self.assertNotIn("private-header", text)
        configuration = ConfDefinition()
        configuration.loads(text)
        self.assertEqual(Path(configuration.get("core.cache:storage_path")), store)
        self.assertEqual(configuration.get("tools.build:jobs"), 2)

    def test_repeated_prepare_and_version_upgrade_are_idempotent(self):
        prepare_toolchain_home(self.base, self.overlay, "23")
        prepare_toolchain_home(self.base, self.overlay, "23")
        user = json.loads((self.overlay / "settings_user.yml").read_text())
        self.assertEqual(user["compiler"]["clang"]["version"].count("23"), 1)
        prepare_toolchain_home(self.base, self.overlay, "24")
        settings = load_settings_yml(str(self.overlay))
        settings.compiler = "clang"
        settings.compiler.version = "24"
        settings.compiler.version.validate()

    def test_original_or_overlapping_home_is_rejected(self):
        before = self.original_hashes()
        for path in (self.base, self.base / "nested", self.root):
            with self.assertRaises(ValueError):
                prepare_toolchain_home(self.base, path, "23")
        self.assertEqual(before, self.original_hashes())

    def test_unowned_overlay_is_not_overwritten(self):
        self.overlay.mkdir()
        existing = self.overlay / "existing"
        existing.write_bytes(b"do-not-overwrite")
        with self.assertRaises(ValueError):
            prepare_toolchain_home(self.base, self.overlay, "23")
        self.assertEqual(existing.read_bytes(), b"do-not-overwrite")

    def test_secret_profile_and_invalid_major_fail_before_writes(self):
        (self.base / "profiles/default").write_text("[buildenv]\nPASSWORD=private\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            prepare_toolchain_home(self.base, self.overlay, "23")
        self.assertFalse(self.overlay.exists())
        for version in ("19.0.0", "", "0", 23):
            with self.assertRaises(ValueError):
                prepare_toolchain_home(self.base, self.overlay, version)


if __name__ == "__main__":
    if not CONAN_AVAILABLE:
        print("SKIP: Conan 2 settings subject unavailable (exit 77)")
        raise SystemExit(77)
    unittest.main()
