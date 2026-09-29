"""Exercise SDK shader include resolution through the public lint CLI."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

LINT = Path(__file__).resolve().parents[1] / "shader_lint.py"


class ShaderIncludes(unittest.TestCase):
    def test_sdk_include_closure_and_missing_declarations(self):
        with tempfile.TemporaryDirectory(prefix="shader include roots ") as tmp:
            root = Path(tmp)
            shaders, sdk = root / "project", root / "sdk include"
            shaders.mkdir()
            sdk.mkdir()
            shader = shaders / "fs_probe.sc"
            shader.write_bytes(b'#include <effects.sh>\nvoid main() { gl_FragColor = u_effect; }\n')
            (sdk / "effects.sh").write_bytes(b'#include "parameters.sh"\n')
            declarations = sdk / "parameters.sh"
            declarations.write_bytes(b'uniform vec4 u_effect;\n')

            def run(*extra):
                return subprocess.run([sys.executable, str(LINT), "--quiet",
                                       *extra, str(shaders)], capture_output=True,
                                      text=True)

            self.assertEqual(run().returncode, 1)  # missing search root
            self.assertEqual(run("-I", str(sdk)).returncode, 0)
            self.assertEqual(run("--include-dir", str(sdk)).returncode, 0)
            declarations.write_bytes(b'uniform vec4 u_other;\n')
            missing = run("-I", str(sdk))
            self.assertEqual(missing.returncode, 1)
            self.assertIn("E003", missing.stdout)
            self.assertIn("u_effect", missing.stdout)
            shader.write_bytes(b'/*\n#include <effects.sh>\n*/\nvoid main() { gl_FragColor = u_other; }\n')
            self.assertEqual(run("-I", str(sdk)).returncode, 1)


    def test_include_directory_order_matches_compiler(self):
        with tempfile.TemporaryDirectory(prefix="shader include order ") as tmp:
            root = Path(tmp)
            shader = root / "fs_probe.sc"
            shader.write_bytes(b'#include <shared.sh>\nvoid main() { gl_FragColor = u_expected; }\n')
            first, second = root / "z first", root / "a second"
            first.mkdir()
            second.mkdir()
            (first / "shared.sh").write_bytes(b'uniform vec4 u_expected;\n')
            (second / "shared.sh").write_bytes(b'uniform vec4 u_wrong;\n')

            def run(a, b):
                return subprocess.run([sys.executable, str(LINT), "--quiet",
                                       "-I", str(a), "-I", str(b), str(shader)],
                                      capture_output=True, text=True)

            self.assertEqual(run(first, second).returncode, 0)
            self.assertEqual(run(second, first).returncode, 1)


if __name__ == "__main__":
    unittest.main()
