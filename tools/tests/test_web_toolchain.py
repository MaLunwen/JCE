"""Read-only SDK identity, same-path upgrades and Conan host args controls."""
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.build import web_toolchain as web

ROOT = Path(__file__).resolve().parents[2]


class WebToolchainTests(unittest.TestCase):
    def setUp(self):
        output = ROOT / "build/tests/0134-web-toolchain"
        output.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=output)
        self.addCleanup(self.temporary.cleanup)
        self.sdk = Path(self.temporary.name)
        self.directory = self.sdk / "upstream/emscripten"
        self.directory.mkdir(parents=True)
        for name in ("emcc.bat", "em++.bat"):
            (self.directory / name).write_text("fixture wrapper", encoding="utf-8")
        (self.directory / "emscripten-version.txt").write_text('"5.0.3"', encoding="utf-8")
        for name in web._HEADERS:
            path = self.directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture " + name, encoding="utf-8")
        toolchain = self.directory / "cmake/Modules/Platform/Emscripten.cmake"
        toolchain.parent.mkdir(parents=True)
        toolchain.write_text("fixture toolchain", encoding="utf-8")
        self.paths = {"cc": str(self.directory / "emcc.bat"),
                      "cxx": str(self.directory / "em++.bat"),
                      "toolchain": str(toolchain)}
        self.banner = "emcc (Emscripten replacement) 5.0.3 (commit123)"
        self.clang = "clang version 23.0.0git (commit456)"
        mock = patch.object(web, "_version_output", side_effect=self.version_output)
        self.addCleanup(mock.stop)
        self.query = mock.start()

    def version_output(self, executable, env):
        return self.banner if executable.name == "emcc.bat" else self.clang

    def test_actual_clang_major_and_full_emcc_abi_identity(self):
        identity = web.probe_toolchain(self.paths)
        self.assertEqual(identity["emcc_version"], "5.0.3")
        self.assertIn("23.0.0git", identity["clang_banner"])
        args = web.conan_host_args(identity, ROOT / "conan/profiles/wasm")
        self.assertIn("compiler=clang", args)
        self.assertIn("compiler.version=23", args)
        self.assertEqual(identity["clang_major"], "23")
        self.assertNotIn("compiler.version=19", args)
        self.assertIn("user.jce:web_toolchain_abi=" + identity["abi_sha256"], args)

    def test_identical_inputs_stable_and_same_path_header_upgrade_invalidates(self):
        first = web.probe_toolchain(self.paths)
        self.assertEqual(first, web.probe_toolchain(self.paths))
        (self.directory / web._HEADERS[1]).write_text("changed bool ABI", encoding="utf-8")
        second = web.probe_toolchain(self.paths)
        self.assertNotEqual(first["abi_sha256"], second["abi_sha256"])
        self.assertNotEqual(web.conan_host_args(first, ROOT / "conan/profiles/wasm"),
                            web.conan_host_args(second, ROOT / "conan/profiles/wasm"))

    def test_same_path_patch_and_compiler_upgrade_invalidates(self):
        first = web.probe_toolchain(self.paths)
        self.banner = "emcc (Emscripten replacement) 5.0.4 (commit789)"
        (self.directory / "emscripten-version.txt").write_text('"5.0.4"', encoding="utf-8")
        second = web.probe_toolchain(self.paths)
        self.assertNotEqual(first["abi_sha256"], second["abi_sha256"])
        self.assertEqual(second["emcc_version"], "5.0.4")
        self.clang = "clang version 24.0.0git (newcommit)"
        self.assertNotEqual(second["abi_sha256"], web.probe_toolchain(self.paths)["abi_sha256"])

    def test_compiler_install_location_is_not_a_binary_abi_setting(self):
        original = (self.directory / "first-sdk/bin").as_posix()
        relocated = (self.directory / "relocated-sdk/bin").as_posix()
        self.clang += "\nInstalledDir: " + original
        first = web.probe_toolchain(self.paths)
        self.clang = self.clang.replace(original, relocated)
        second = web.probe_toolchain(self.paths)
        self.assertNotEqual(first["clang_banner"], second["clang_banner"])
        self.assertEqual(first["abi_sha256"], second["abi_sha256"])

    def test_version_disagreement_fails_closed(self):
        (self.directory / "emscripten-version.txt").write_text('"4.0.10"', encoding="utf-8")
        with self.assertRaises(ValueError):
            web.probe_toolchain(self.paths)

    def test_missing_header_cannot_implicitly_reuse_old_identity(self):
        (self.directory / web._HEADERS[1]).unlink()
        with self.assertRaises(FileNotFoundError):
            web.probe_toolchain(self.paths)

    def test_missing_wrapper_is_rejected_without_subprocess(self):
        Path(self.paths["cxx"]).unlink()
        with self.assertRaises(ValueError):
            web.probe_toolchain(self.paths)
        self.query.assert_not_called()

    def test_clang_banner_or_malformed_version_is_not_emcc(self):
        for banner in ("clang version 23.0.0git", "emcc (replacement) nineteen", ""):
            with self.assertRaises(ValueError):
                web.parse_emcc_version(banner)
        self.assertEqual(web.parse_emcc_version("emcc (replacement) 5.0.3-git (commit)"), "5.0.3-git")

    def test_malformed_or_forged_clang_identity_is_rejected(self):
        for banner in ("clang version 23", "clang version 0.0.0", "gcc version 23.0.0", ""):
            with self.assertRaises(ValueError):
                web.parse_clang_major(banner)
        identity = web.probe_toolchain(self.paths)
        identity["clang_major"] = "19"
        with self.assertRaises(ValueError):
            web.conan_host_args(identity, ROOT / "conan/profiles/wasm")

    def test_profile_without_global_conf_identity_is_rejected(self):
        identity = web.probe_toolchain(self.paths)
        profile = self.sdk / "badprofile"
        profile.write_text("[settings]\ncompiler=clang\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            web.conan_host_args(identity, profile)
        profile.write_text("[settings]\ncompiler=emcc\ncompiler.version=5.0.3\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            web.conan_host_args(identity, profile)


if __name__ == "__main__":
    unittest.main()
