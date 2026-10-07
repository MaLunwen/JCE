"""Run the production resolver with real filesystem and launch controls."""
import ast
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
source = ast.parse((ROOT / "tools/build/jce.py").read_text(encoding="utf-8"))
selected = ast.Module(body=[node for node in source.body
    if isinstance(node, ast.FunctionDef) and node.name in
    {"find_host_shaderc", "_shaderc_runs", "_resolve_shaderc", "cross_asset_tool_overrides"}], type_ignores=[])


class HostShadercTests(unittest.TestCase):
    def setUp(self):
        base = ROOT / "build/tests/host-shaderc"
        base.mkdir(parents=True, exist_ok=True)
        self.scratch = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.namespace = dict(Path=Path, os=os, subprocess=subprocess,
                              ROOT=self.root, HOST="windows", log=lambda _: None)
        exec(compile(selected, "tools/build/jce.py", "exec"), self.namespace)

    def file(self, name):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"control")
        return path

    def environment(self, **extra):
        return mock.patch.dict(os.environ, {"CONAN_HOME": str(self.root / "conan"),
            **extra}, clear=True)

    def test_inspection_reuses_host_resolver(self):
        sentinel = self.file("native.exe")
        resolver = mock.Mock(return_value=sentinel)
        self.namespace["find_host_shaderc"] = resolver
        self.assertEqual(self.namespace["_resolve_shaderc"](), sentinel)
        resolver.assert_called_once_with()

    def test_rejects_foreign_explicit_and_uses_compatible_alias(self):
        foreign, native = self.file("foreign.exe"), self.file("native.exe")
        self.namespace["_shaderc_runs"] = lambda p: p == native
        with self.environment(JCE_SHADERC_EXECUTABLE=str(foreign), BGFX_SHADERC=str(native)):
            self.assertEqual(self.namespace["find_host_shaderc"](), native)

    def test_foreign_sdk_and_architecture_are_not_selected(self):
        self.file("dist/sdk/linux-x86_64/bin/shaderc")
        self.file("dist/sdk/win32-aarch64/bin/shaderc.exe")
        native = self.file("dist/sdk/win32-x86_64/bin/shaderc.exe")
        seen = []
        def runnable(p):
            seen.append(p)
            return p == native
        self.namespace["_shaderc_runs"] = runnable
        with self.environment():
            self.assertEqual(self.namespace["find_host_shaderc"](), native)
        self.assertIn(native, seen)
        self.assertNotIn(self.root / "dist/sdk/linux-x86_64/bin/shaderc", seen)

    def test_no_compatible_candidate_is_unavailable(self):
        self.file("dist/sdk/win32-aarch64/bin/shaderc.exe")
        self.namespace["_shaderc_runs"] = lambda p: False
        with self.environment():
            self.assertIsNone(self.namespace["find_host_shaderc"]())

    def test_cross_assets_forward_both_refreshed_native_tools(self):
        pak, wrapper = self.file("native tools/jce_pak.exe"), self.file("native tools/jce_bin2obj.exe")
        resolver = mock.Mock(return_value={"jce_pak": pak, "jce_bin2obj": wrapper})
        self.namespace["ensure_host_tools"] = resolver
        env = {"target": "wasm"}
        args = self.namespace["cross_asset_tool_overrides"](env)
        resolver.assert_called_once_with(env, ("jce_pak", "jce_bin2obj"))
        self.assertEqual(args, [f"-DJCE_PAK_EXECUTABLE={pak}",
                               f"-DJCE_BIN2OBJ_EXECUTABLE={wrapper}"])

    def test_cross_tool_failure_cannot_fall_back_to_target_binary(self):
        self.namespace["ensure_host_tools"] = mock.Mock(side_effect=RuntimeError("native build failed"))
        with self.assertRaisesRegex(RuntimeError, "native build failed"):
            self.namespace["cross_asset_tool_overrides"]({})

    def test_launch_error_and_bad_exit_are_rejected(self):
        resolver = self.namespace["_shaderc_runs"]
        with mock.patch.object(subprocess, "run", side_effect=OSError("wrong executable format")):
            self.assertFalse(resolver(Path("foreign")))
        with mock.patch.object(subprocess, "run", return_value=mock.Mock(returncode=1)):
            self.assertFalse(resolver(Path("broken")))
        with mock.patch.object(subprocess, "run", return_value=mock.Mock(returncode=0)):
            self.assertTrue(resolver(Path("native")))


if __name__ == "__main__":
    unittest.main()
