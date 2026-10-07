"""SDK tool selection and host-cache ownership without compiling."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))


class SDKHostToolsTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('sdk_tools_driver', ROOT / 'tools/build/jce.py')
        self.driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.driver)
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'build')
        self.addCleanup(self.temp.cleanup)
        self.sdk = Path(self.temp.name) / 'sdk'
        self.names = ('jce_cook', 'jce_pak', 'jce_bin2obj')
        self.target = {'host': 'windows', 'arch': 'x64'}
        self.args = SimpleNamespace(project=self.temp.name, arch='x64', variant='dist')

    def populate(self, directory='bin', names=None):
        for name in names or self.names:
            path = self.sdk / directory / (name + '.exe')
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'probe fixture')

    def paths(self, probe):
        with patch.object(self.driver, 'HOST', 'windows'), \
             patch.object(self.driver, 'resolve_target', return_value=self.target), \
             patch.object(self.driver, 'resolve_sdk', return_value=self.sdk), \
             patch.object(self.driver, '_host_cache_paths', return_value=[Path('host-lock')]), \
             patch.object(self.driver.subprocess, 'run', side_effect=probe):
            return self.driver._app_cache_paths({'args': self.args})

    def test_complete_native_sdk_avoids_host_lock_and_repo_tools(self):
        self.populate()
        calls = []
        def probe(cmd, **kwargs):
            calls.append(Path(cmd[0]))
            return SimpleNamespace(returncode=1)
        self.assertNotIn(Path('host-lock'), self.paths(probe))
        self.assertEqual(set(calls), {self.sdk / 'bin' / (name + '.exe') for name in self.names})
        self.assertEqual(len(self.args._sdk_host_tools), 3)

    def test_missing_tool_keeps_host_lock(self):
        self.populate(names=self.names[:2])
        self.assertIn(Path('host-lock'), self.paths(lambda *a, **k: SimpleNamespace(returncode=0)))

    def test_loader_error_timeout_and_crash_keep_host_lock(self):
        self.populate()
        for failure in (OSError('wrong architecture'), subprocess.TimeoutExpired('probe', 30), -11, 0xc0000135):
            def probe(*args, **kwargs):
                if isinstance(failure, Exception):
                    raise failure
                return SimpleNamespace(returncode=failure)
            self.assertIn(Path('host-lock'), self.paths(probe))

    def test_cross_prefers_host_executable(self):
        self.populate()
        self.populate('bin/host')
        self.target['cross'] = True
        self.paths(lambda *a, **k: SimpleNamespace(returncode=0))
        self.assertTrue(all(path.parent == self.sdk / 'bin/host' for path in self.args._sdk_host_tools.values()))

    def test_cross_skips_unlaunchable_target_bin(self):
        self.populate()
        self.populate('bin/host')
        def probe(cmd, **kwargs):
            if Path(cmd[0]).parent == self.sdk / 'bin':
                raise OSError('target executable')
            return SimpleNamespace(returncode=0)
        self.assertNotIn(Path('host-lock'), self.paths(probe))
        self.assertTrue(all(path.parent == self.sdk / 'bin/host' for path in self.args._sdk_host_tools.values()))

    def test_dry_run_still_requires_actual_probe(self):
        self.populate()
        with patch.object(self.driver, 'DRY_RUN', True):
            self.assertIn(Path('host-lock'), self.paths(lambda *a, **k: SimpleNamespace(returncode=0xc000007b)))

    def test_complete_prelock_set_lost_after_lock_requires_retry(self):
        original = {name: self.sdk / 'bin' / (name + '.exe') for name in self.names}
        self.args._sdk_host_tools = original
        for changed in ({}, dict(list(original.items())[:2]),
                        {name: self.sdk / 'bin/host' / (name + '.exe') for name in self.names}):
            with patch.object(self.driver, '_sdk_host_tools', return_value=changed):
                with self.assertRaises(SystemExit):
                    self.driver._confirmed_app_sdk_tools(self.args, self.sdk, self.target)

    def test_partial_prelock_set_retains_owned_fallback(self):
        self.args._sdk_host_tools = {'jce_pak': self.sdk / 'bin/jce_pak.exe'}
        with patch.object(self.driver, '_sdk_host_tools', return_value={}):
            self.assertEqual(self.driver._confirmed_app_sdk_tools(self.args, self.sdk, self.target), {})

    def test_confirmed_cook_map_does_not_reprobe_or_fallback(self):
        cooker = self.sdk / 'bin/jce_cook.exe'
        cooker.parent.mkdir(parents=True)
        cooker.write_bytes(b'fixture')
        args = SimpleNamespace(project=self.temp.name, sdk=self.sdk, sdk_target=self.target,
                               sdk_tools={'jce_cook': cooker})
        commands = []
        with patch.object(self.driver, 'read_manifest', return_value={}), \
             patch.object(self.driver, 'resolve_target', return_value=self.target), \
             patch.object(self.driver, '_sdk_host_tools', side_effect=AssertionError('second probe')), \
             patch.object(self.driver, 'ensure_host_tools', side_effect=AssertionError('fallback')), \
             patch.object(self.driver, 'run', side_effect=lambda cmd, **kw: commands.append(cmd)):
            self.driver.cmd_cook(args)
        self.assertEqual(commands[0][0], str(cooker))

    def test_cook_explicit_sdk_prefers_selected_tool(self):
        self.populate()
        commands = []
        args = SimpleNamespace(project=self.temp.name, sdk=self.sdk, sdk_target=self.target)
        with patch.object(self.driver, 'HOST', 'windows'), \
             patch.object(self.driver, 'read_manifest', return_value={}), \
             patch.object(self.driver, 'resolve_target', return_value=self.target), \
             patch.object(self.driver.subprocess, 'run', return_value=SimpleNamespace(returncode=0)), \
             patch.object(self.driver, 'run', side_effect=lambda cmd, **kw: commands.append(cmd)), \
             patch.object(self.driver, 'ensure_host_tools', side_effect=AssertionError('repo fallback')):
            self.driver.cmd_cook(args)
        self.assertEqual(commands[0][0], str(self.sdk / 'bin/jce_cook.exe'))


if __name__ == '__main__':
    unittest.main()
