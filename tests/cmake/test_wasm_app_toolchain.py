import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
JCE_SCRIPT = ROOT / "scripts" / "jce.py"


class WasmAppToolchainContractTests(unittest.TestCase):
    def test_skips_cmake_linker_depfile_probe_for_emscripten(self):
        script = JCE_SCRIPT.read_text(encoding="utf-8")

        marker = "if t.get(\"emscripten\"):\n        cfg.extend(["
        self.assertIn(marker, script)
        wasm_block = script.split(marker, 1)[1].split("elif HOST", 1)[0]
        self.assertIn("-DCMAKE_C_LINKER_DEPFILE_SUPPORTED=FALSE", wasm_block)
        self.assertIn("-DCMAKE_CXX_LINKER_DEPFILE_SUPPORTED=FALSE", wasm_block)

    def test_editor_clean_removes_build_and_conan_state_before_install(self):
        script = JCE_SCRIPT.read_text(encoding="utf-8")

        editor = script.split("def cmd_editor(args) -> None:", 1)[1]
        editor = editor.split("# ── Subcommand: app", 1)[0]
        self.assertIn("if args.clean:", editor)
        self.assertIn("for directory in (bdir, conan_dir(t)):", editor)
        self.assertLess(editor.index("if args.clean:"), editor.index("conan_install("))


if __name__ == "__main__":
    unittest.main()
