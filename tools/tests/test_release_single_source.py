"""Real Git hook/index controls for one-authority JCE release versioning.

Tests run in temporary repositories; never touch the caller's Git index.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ReleaseSingleSourceControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="jce-version-fixture-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ("tools/lint/check_release_version.py", "tools/lint/sync_release_mirrors.py",
                     "tools/hooks/commit-msg", "tools/hooks/pre-commit"):
            dest = self.root / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, dest)
            if name.startswith("tools/hooks/"):
                dest.chmod(0o755)
        self.cmake = self.root / "CMakeLists.txt"
        self.cmake.write_text("cmake_minimum_required(VERSION 3.27)\nproject(JCE VERSION 0.14.0 LANGUAGES C)\n", encoding="utf-8")
        self.java = self.root / "engine/java/com/jce/JceRuntime.java"
        self.java.parent.mkdir(parents=True)
        self.java.write_text("package com.jce;\npublic final class JceRuntime {\n"
                             "    private static final int EXPECTED_API_VERSION = 0x000E0000;\n}\n", encoding="utf-8")
        self.readme = self.root / "README.md"
        self.readme.write_text("# JCE\n| Version | `0.14.0` (authoritative: `CMakeLists.txt`) |\n", encoding="utf-8")
        (self.root / ".gitignore").write_text("__pycache__/\n", encoding="utf-8")
        self.git("init", "-q")
        self.git("config", "user.name", "JCE Version Fixture")
        self.git("config", "user.email", "jce-version-fixture@example.invalid")
        self.git("add", ".")
        # Bootstrap HEAD, then turn on hooks. HEAD is a required input of
        # the original release gate; no external checkout or toolchain needed.
        self.git("-c", "core.hooksPath=nonexistent-hooks", "commit", "-qm", "v-0.14.0")
        self.git("config", "core.hooksPath", "tools/hooks")

    def git(self, *args, check=True):
        result = subprocess.run(["git", *args], cwd=self.root, capture_output=True, text=True)
        if check and result.returncode:
            self.fail("git " + " ".join(args) + " failed:\n" + result.stdout + "\n" + result.stderr)
        return result

    def tool(self, *args):
        return subprocess.run([sys.executable, str(self.root / "tools/lint/sync_release_mirrors.py"), *args],
                              cwd=self.root, capture_output=True, text=True)

    def stage_new_version(self):
        self.cmake.write_text(self.cmake.read_text().replace("0.14.0", "0.14.1"), encoding="utf-8")
        self.git("add", "CMakeLists.txt")

    def test_only_staged_cmake_change_commits_all_mirrors(self):
        self.stage_new_version()
        result = self.git("commit", "-m", "v-0.14.1, source version update", check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # Git may suppress hook stdout on successful commits; verify the actual commit tree.
        self.assertIn("0x000E0100", self.git("show", "HEAD:engine/java/com/jce/JceRuntime.java").stdout)
        self.assertIn("| Version | `0.14.1`", self.git("show", "HEAD:README.md").stdout)
        self.assertFalse(self.git("status", "--porcelain").stdout.strip())

    def test_existing_commit_message_contract_not_bypassed(self):
        self.stage_new_version()
        result = self.git("commit", "-m", "ordinary fix", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("commit subject must start v-0.14.1", result.stdout + result.stderr)
        self.assertEqual(self.git("log", "-1", "--format=%s").stdout.strip(), "v-0.14.0")
        self.assertIn("0x000E0100", self.git("show", ":engine/java/com/jce/JceRuntime.java").stdout)

    def test_unstaged_content_is_not_silently_added(self):
        self.stage_new_version()
        self.java.write_text(self.java.read_text() + "// intentionally unstaged change\n", encoding="utf-8")
        self.readme.write_text(self.readme.read_text() + "Unstaged design notes\n", encoding="utf-8")
        result = self.git("commit", "-m", "v-0.14.1", check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("intentionally unstaged", self.git("show", "HEAD:engine/java/com/jce/JceRuntime.java").stdout)
        self.assertNotIn("Unstaged design", self.git("show", "HEAD:README.md").stdout)
        self.assertIn("intentionally unstaged", self.java.read_text())
        self.assertIn("Unstaged design", self.readme.read_text())
        self.assertIn(" M engine/java/com/jce/JceRuntime.java", self.git("status", "--short").stdout)

    def test_readonly_check_does_not_write_or_stage(self):
        self.stage_new_version()
        index_before = (self.root / ".git/index").read_bytes()
        before = (self.java.read_bytes(), self.readme.read_bytes())
        result = self.tool("--staged", "--check")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(index_before, (self.root / ".git/index").read_bytes())
        self.assertEqual(before, (self.java.read_bytes(), self.readme.read_bytes()))

    def test_partial_staged_cmake_version_is_refused_without_mutation(self):
        self.stage_new_version()
        self.cmake.write_text(self.cmake.read_text().replace("0.14.1", "0.14.2"), encoding="utf-8")
        before = self.git("ls-files", "--stage").stdout
        result = self.tool("--staged")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("staged and worktree CMake versions differ", result.stderr)
        self.assertEqual(before, self.git("ls-files", "--stage").stdout)

    def test_idempotence_and_negative_jni_binding_control(self):
        self.stage_new_version()
        self.assertEqual(self.tool("--staged").returncode, 0)
        before = self.git("ls-files", "--stage").stdout
        self.assertEqual(self.tool("--staged").returncode, 0)
        self.assertEqual(before, self.git("ls-files", "--stage").stdout)
        self.assertEqual(self.tool("--staged", "--check").returncode, 0)
        self.java.write_text(self.java.read_text().replace("0x000E0100", "0x000E0000"), encoding="utf-8")
        self.git("add", "engine/java/com/jce/JceRuntime.java")
        msg = self.root / "COMMIT_MSG"
        msg.write_text("v-0.14.1", encoding="utf-8")
        gate = subprocess.run([sys.executable, "tools/lint/check_release_version.py",
                               "--commit-message-file", str(msg)], cwd=self.root,
                              capture_output=True, text=True)
        self.assertNotEqual(gate.returncode, 0)
        self.assertIn("0x000E0100", gate.stdout)


if __name__ == "__main__":
    unittest.main()
