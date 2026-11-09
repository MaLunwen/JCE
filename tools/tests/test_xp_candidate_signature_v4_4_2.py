"""JCE XP candidate archive call contract; no toolchain or network required."""
from __future__ import annotations

import ast
import hashlib
import os
from pathlib import Path
import stat
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools/build/xp_editor.py"
NAME = "_archive_unproved_editor_candidate"
EXPECTED = ("candidate", "cache", "run", "receipt", "abi_digest", "profile",
            "build_profile", "bgfx_bx_debug")


def _nodes():
    tree = ast.parse(SOURCE.read_text(encoding="utf-8"), filename=str(SOURCE))
    helper = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == NAME]
    build = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "build"]
    if len(helper) != 1 or len(build) != 1:
        raise AssertionError("XP candidate owner/helper must each appear exactly once")
    calls = [n for n in ast.walk(build[0]) if isinstance(n, ast.Call)
             and isinstance(n.func, ast.Name) and n.func.id == NAME]
    if len(calls) != 1:
        raise AssertionError("XP candidate archive must have exactly one build call")
    return helper[0], build[0], calls[0]


class XPCandidateSignatureV442(unittest.TestCase):
    def test_build_passes_every_required_helper_argument(self):
        helper, build, call = _nodes()
        self.assertEqual(tuple(arg.arg for arg in helper.args.args), EXPECTED)
        self.assertEqual(helper.args.defaults, [])
        self.assertEqual(tuple(v.id for v in call.args if isinstance(v, ast.Name)), EXPECTED[:5])
        self.assertEqual(len(call.args), 5)
        self.assertEqual(tuple(k.arg for k in call.keywords), EXPECTED[5:])
        self.assertEqual(tuple(k.value.id for k in call.keywords if isinstance(k.value, ast.Name)), EXPECTED[5:])
        # Verify all variables are visible in build(), not accidentally globals.
        arguments = {arg.arg for arg in build.args.args + build.args.kwonlyargs}
        locals_assigned = {node.id for node in ast.walk(build) if isinstance(node, ast.Name)
                           and isinstance(node.ctx, ast.Store)}
        self.assertTrue(set(EXPECTED).issubset(arguments | locals_assigned))

    def test_archive_restores_existing_exe_on_failed_compile(self):
        helper, build, call = _nodes()
        tree = ast.parse(SOURCE.read_text(encoding="utf-8"), filename=str(SOURCE))
        restore = next(n for n in tree.body if isinstance(n, ast.FunctionDef)
                       and n.name == "_restore_archived_editor_candidate")
        ns = {"os": os, "stat": stat, "_ordinary": lambda p: Path(p).resolve(),
              "_owned_child": lambda p, parent: Path(p).resolve(),
              "_sha": lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest(),
              "print": lambda *a, **k: None}
        exec(compile(ast.Module(body=[helper, restore], type_ignores=[]), str(SOURCE), "exec"), ns)
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            candidate = directory / "release/jce_editor.exe"
            candidate.parent.mkdir()
            candidate.write_bytes(b"preexisting-verified-byte-sequence")
            cache = directory / "obj/release"
            cache.mkdir(parents=True)
            run = directory / "validation"
            run.mkdir()
            receipt = {"platform_port": {"headers_sha256": "a" * 64},
                       "consumer_ports": {}}
            archived = ns[NAME](candidate, cache, run, receipt, "abi", Path("host"),
                                 Path("build"), "auto")
            self.assertIsNotNone(archived)
            self.assertFalse(candidate.exists())
            self.assertEqual(archived.read_bytes(), b"preexisting-verified-byte-sequence")
            candidate.write_bytes(b"partial-link-output")
            ns["_restore_archived_editor_candidate"](candidate, archived, run, receipt)
            self.assertEqual(candidate.read_bytes(), b"preexisting-verified-byte-sequence")
            archived_partial = receipt["archived_preexisting_candidate"]["failed_build_output"]
            self.assertEqual(Path(archived_partial).read_bytes(), b"partial-link-output")
            self.assertTrue(receipt["archived_preexisting_candidate"]["restored_after_failure"])

    def test_build_has_failure_rollback(self):
        _, build, call = _nodes()
        restores = [n for n in ast.walk(build) if isinstance(n, ast.Call)
                    and isinstance(n.func, ast.Name)
                    and n.func.id == "_restore_archived_editor_candidate"]
        self.assertEqual(len(restores), 1)
        guarded = [node for node in ast.walk(build) if isinstance(node, ast.Try)
                   and any(n is restores[0] for handler in node.handlers
                           for stmt in handler.body for n in ast.walk(stmt))]
        self.assertEqual(len(guarded), 1)
        self.assertTrue(any(n is call for node in ast.walk(build)
                            if isinstance(node, ast.Assign) for n in ast.walk(node)))


if __name__ == "__main__":
    unittest.main()
