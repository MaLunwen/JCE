import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.build import native_registry as registry
from tools.process_sampler import run_process


def fixture():
    asset = dict(path='scripts/controller.jcec', type='script', representation='c.class-ref',
                 asset_id='a'*16, content_id='b'*16, size=9, dependencies=[])
    manifest = dict(native_script_factories=['factory', 'factory'],
                    native_script_refs={'scripts/controller.jcec': 'scripts/Controller.jcec'})
    return manifest, [asset]


class NativeRegistryTests(unittest.TestCase):
    def test_plan_and_complete_closure_binding(self):
        manifest, assets = fixture()
        value = registry.plan(manifest, assets, 'application')
        self.assertEqual(value['state'], 'PENDING_STATIC_REGISTRY_EXECUTION')
        self.assertEqual(value['factories'], ['factory'])
        self.assertIn('scripts/Controller.jcec', registry.cmake_args(value)[1])
        changed = [dict(assets[0], content_id='c'*16)]
        self.assertNotEqual(value['closure_sha256'], registry.plan(manifest, changed, 'application')['closure_sha256'])
        self.assertIsNone(registry.plan({}, [], 'application'))

    def test_invalid_binding_and_empty_native_requirement(self):
        manifest, assets = fixture()
        for bad in ({}, {'scripts/controller.jcec': 'scripts/Other.jcec'},
                    {'scripts/Controller.jcec': 'scripts/Controller.jcec'}):
            with self.assertRaises(ValueError):
                registry.plan(dict(manifest, native_script_refs=bad), assets, 'application')
        with self.assertRaises(ValueError):
            registry.plan(dict(manifest, required_languages=['c']), [], 'application')
        with self.assertRaises(ValueError):
            registry.plan(dict(manifest, native_script_factories=['bad;symbol']), assets, 'application')
        with self.assertRaises(ValueError):
            registry.plan(manifest, [dict(assets[0], type='texture')], 'application')

    def test_receipt_strict_types_and_all_fields(self):
        request = dict(nonce='a'*64, closure_sha256='b'*64, target='app', selected_count=1, factory_count=1)
        good = dict(request, schema=1, resolved_count=1, registered_factory_count=1, ok=True)
        registry._receipt(good, request)
        for key, value in (('schema', True), ('nonce', 'c'*64), ('resolved_count', 0), ('ok', False), ('factory_count', True)):
            with self.assertRaises(ValueError):
                registry._receipt(dict(good, **{key: value}), request)

    def test_real_child_receipt_and_isolated_copy(self):
        # CPU protocol fixture only: injected sampler launches an actual child,
        # not an SDK product or a claim of Python single-EXE relocation.
        manifest, assets = fixture()
        value = registry.plan(manifest, assets, 'application')
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as folder:
            folder = Path(folder)
            artifact = folder/'fixture.exe'
            artifact.write_bytes(b'protocol-fixture-artifact')
            args = SimpleNamespace(project=folder, registry_proof=None)
            def child(command, **kwargs):
                self.assertEqual(list(Path(kwargs['cwd']).iterdir()), [Path(command[0])])
                request = json.loads((Path(kwargs['output_dir']).parent/'request.json').read_text())
                code = ('import json,os; r=' + repr(dict(request, schema=1, resolved_count=1,
                        registered_factory_count=1, ok=True)) + ';'
                        'json.dump(r,open(os.environ["JCE_NATIVE_REGISTRY_REPORT"],"w"))')
                return run_process([sys.executable, '-c', code], **kwargs)
            with patch.object(registry, 'run_process', child):
                result = registry.verify(None, args, artifact, value, {})
            self.assertEqual(result['state'], 'STATIC_REGISTRY_EXECUTION_VERIFIED')
            with patch.object(registry, 'run_process', return_value={'returncode': 0, 'timed_out': False}):
                with self.assertRaisesRegex(ValueError, 'no receipt'):
                    registry.verify(None, args, artifact, value, {})

    def test_web_pending_reuse_and_stale_proof(self):
        manifest, assets = fixture()
        value = registry.plan(manifest, assets, 'application')
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as folder:
            folder = Path(folder)
            artifact = folder/'application.html'
            artifact.write_text('<html></html>')
            args = SimpleNamespace(project=folder, registry_proof=None)
            with self.assertRaisesRegex(ValueError, 'pending'):
                registry.verify(None, args, artifact, value, {})
            request_path = folder/'build/native-registry-proof/web-request.json'
            request = json.loads(request_path.read_text())
            with self.assertRaisesRegex(ValueError, 'pending'):
                registry.verify(None, args, artifact, value, {})
            self.assertEqual(request, json.loads(request_path.read_text()))
            receipt = dict(request, resolved_count=1, registered_factory_count=1, ok=True)
            proof = folder/'external.json'
            proof.write_text(json.dumps(dict(request_sha256=registry._sha(request_path),
                artifact_sha256=registry._sha(artifact), receipt=receipt)))
            args.registry_proof = proof
            self.assertEqual(registry.verify(None, args, artifact, value, {})['state'],
                             'EXTERNAL_STATIC_REGISTRY_EXECUTION_VERIFIED')
            artifact.write_text('<html>changed</html>')
            with self.assertRaisesRegex(ValueError, 'stale'):
                registry.verify(None, args, artifact, value, {})


if __name__ == '__main__':
    unittest.main()
