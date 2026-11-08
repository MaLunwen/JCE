"""CPU source-policy parity; not linked EXE or target runtime acceptance."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
OWNER = ROOT / "editor/src/core/jce_dist_audit.cpp"
MANAGER = ROOT / "editor/src/core/jce_build_manager.cpp"
SPEC = importlib.util.spec_from_file_location("jce_dist_pe_standalone", ROOT / "tools/build/standalone.py")
STANDALONE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STANDALONE)


def dll_policy(source: str) -> set[str]:
    blocks = re.findall(r"constexpr const char \*system_dlls\[\] = \{(.*?)\n\};", source, re.S)
    if len(blocks) != 1:
        raise AssertionError("expected one actual Editor OS DLL policy")
    names = re.findall(r'"([a-z0-9_]+)"', blocks[0])
    if not names or len(names) != len(set(names)):
        raise AssertionError("empty or duplicate Editor OS DLL policy")
    return set(names)


class DistPePolicyTests(unittest.TestCase):
    def test_gui_os_dll_policy_is_the_cli_policy(self):
        self.assertEqual(dll_policy(OWNER.read_text(encoding="utf-8")), STANDALONE.SYSTEM_DLLS)

    def test_policy_negative_catches_extra_external_runtime(self):
        source = OWNER.read_text(encoding="utf-8")
        mutant = source.replace('    "advapi32",', '    "vcruntime140", "advapi32",', 1)
        self.assertNotEqual(source, mutant)
        with self.assertRaises(AssertionError):
            self.assertEqual(dll_policy(mutant), STANDALONE.SYSTEM_DLLS)

    def test_prefix_case_rules_keep_cli_acceptance(self):
        source = OWNER.read_text(encoding="utf-8")
        prefixes = re.findall(r'name\.compare\(0, ([0-9]+), "([a-z-]+)"\) == 0', source)
        self.assertEqual(prefixes, [("16", "api-ms-win-core-"), ("11", "ext-ms-win-")])
        for size, prefix in prefixes:
            self.assertEqual(int(size), len(prefix))
        self.assertIn('value >= \'A\' && value <= \'Z\'', source)
        self.assertIn('name.compare(name.size() - 4u, 4, ".dll") != 0', source)

    def test_gui_binds_captured_request_not_host_machine(self):
        source = MANAGER.read_text(encoding="utf-8")
        block = '''    input.expected_pe_machine = g_pending.arch == "i686" ? 0x014cu :
        g_pending.arch == "x86_64" ? 0x8664u :
        g_pending.arch == "aarch64" ? 0xaa64u : UINT16_MAX;'''
        self.assertEqual(source.count(block), 1)
        function = source[source.index("bool run_dist_audit("):source.index("/* Export before collecting PAK inputs")]
        self.assertIn(block, function)
        self.assertNotIn("jce_project_build_host_arch", function)


if __name__ == "__main__":
    unittest.main(verbosity=2)
