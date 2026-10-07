"""Project bundle forwarding and incremental cache controls; no native build."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bundle_delivery_test', ROOT / 'tools/build/delivery.py')
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class BundleInputs(unittest.TestCase):
    def setUp(self):
        base = ROOT / 'build/tests/project-bundle-inputs'
        base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=base)
        self.root = Path(self.temp.name).resolve()
        assert self.root.is_relative_to(base.resolve())
        self.bundle = self.root / 'one.jbundle'
        self.bundle.write_bytes(b'argument fixture, not a validated archive')

    def tearDown(self):
        self.temp.cleanup()

    def test_empty_project_clears_previous_bundle_cache(self):
        self.assertEqual(policy.bundle_cmake_args(self.root, {}),
                         ['-U', 'JCE_PROJECT_BUNDLES', '-U', 'JCE_PROJECT_PREBUILT_BUNDLE_DIR'])

    def test_relative_input_resolves_once(self):
        args = policy.bundle_cmake_args(self.root, {'bundles': ['one.jbundle']})
        self.assertEqual(args[-1], '-DJCE_PROJECT_BUNDLES=' + self.bundle.as_posix())

    def test_explicit_absolute_external_input(self):
        self.assertEqual(policy.bundle_cmake_args(self.root / 'project',
                         {'bundles': [str(self.bundle)]})[-1],
                         '-DJCE_PROJECT_BUNDLES=' + self.bundle.as_posix())

    def test_order_is_preserved(self):
        other = self.root / 'two.jbundle'
        other.write_bytes(b'argument fixture')
        args = policy.bundle_cmake_args(self.root, {'bundles': ['two.jbundle', 'one.jbundle']})
        self.assertTrue(args[-1].endswith(other.as_posix() + ';' + self.bundle.as_posix()))

    def test_malformed_and_missing_inputs_fail(self):
        for bundles in (None, 'one.jbundle', [False], [''], ['missing.jbundle'], ['one.pak'], ['bad\n.jbundle']):
            with self.subTest(bundles=bundles), self.assertRaises(ValueError):
                policy.bundle_cmake_args(self.root, {'bundles': bundles})

    def test_duplicate_resolved_input_fails(self):
        with self.assertRaises(ValueError):
            policy.bundle_cmake_args(self.root, {'bundles': ['one.jbundle', './one.jbundle']})

    def test_unicode_spaces_and_semicolon_are_one_cmake_list_item(self):
        path = self.root / 'space \u4e2d\u6587;directory' / 'asset.jbundle'
        path.parent.mkdir()
        path.write_bytes(b'argument fixture')
        args = policy.bundle_cmake_args(self.root, {'bundles': [str(path)]})
        self.assertEqual(args[-1], '-DJCE_PROJECT_BUNDLES=' + path.as_posix().replace(';', '\\;'))

    def test_supported_driver_uses_shared_argument_owner(self):
        source = (ROOT / 'tools/build/jce.py').read_text(encoding='utf-8')
        self.assertIn('*_delivery_module().bundle_cmake_args(project, manifest)', source)


class NativeArtifactNames(unittest.TestCase):
    def test_windows_template_and_explicit_extension(self):
        self.assertEqual(policy.native_artifact_name('Project', 'windows'), 'Project.exe')
        self.assertEqual(policy.native_artifact_name('Project.exe', 'windows'), 'Project.exe')
        self.assertEqual(policy.native_artifact_name('Project.EXE', 'windows'), 'Project.EXE')

    def test_non_windows_name_is_unchanged(self):
        for host in ('linux', 'macos'):
            self.assertEqual(policy.native_artifact_name('Project', host), 'Project')

    def test_invalid_names_fail(self):
        for name in ('', None, False):
            with self.subTest(name=name), self.assertRaises(ValueError):
                policy.native_artifact_name(name, 'windows')


if __name__ == '__main__':
    unittest.main()
