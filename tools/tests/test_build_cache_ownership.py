"""Driver ownership scopes without invoking any build subprocess."""
import importlib.util
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("cache_ownership_driver", ROOT / "tools/build/jce.py")
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


class DriverCacheOwnershipTests(unittest.TestCase):
    def test_sdk_paths_match_actual_debug_web_and_install_prefix(self):
        target = {"key": "fixture", "host": "windows", "arch": "x64"}
        with patch.object(driver, "preset_binary_dir", side_effect=lambda t, kind: ROOT / "build" / kind), \
                patch.object(driver, "sdk_install_dir", return_value=ROOT / "dist/sdk/fixture"), \
                patch.object(driver, "conan_dir", return_value=ROOT / "build/generators"):
            for config in ("Release", "Debug"):
                paths = driver._sdk_cache_paths({"t": target, "variant": "release", "config": config})
                self.assertIn(ROOT / "build" / ("debug" if config == "Debug" else "release-sdk"), paths)
                self.assertIn(ROOT / "build/generators", paths)
                self.assertIn(ROOT / "dist/sdk" / ("fixture-debug" if config == "Debug" else "fixture"), paths)
            paths = driver._sdk_cache_paths({"t": dict(target, emscripten=True), "variant": "dist", "config": "Release"})
            self.assertIn(ROOT / "build/web/wasm-dist-sdk", paths)
            for host_path in driver._host_cache_paths():
                self.assertIn(host_path, paths)
            native = driver._sdk_cache_paths({"t":target,"variant":"dist","config":"Release"})
            for host_path in driver._host_cache_paths():
                self.assertNotIn(host_path, native)

    def test_native_embedded_editor_does_not_hold_unrelated_host_tools(self):
        target = {"key":"fixture","host":"windows","arch":"x64","cross":False}
        args = SimpleNamespace(arch="x64",variant="dist",delivery_profile="embedded",standalone=True)
        with patch.object(driver, "resolve_target", return_value=target):
            paths = driver._editor_cache_paths({"args":args})
        for host_path in driver._host_cache_paths():
            self.assertNotIn(host_path, paths)

    def test_app_locks_project_tree_and_selected_sdk(self):
        target = {"host": "linux", "arch": "arm64"}
        args = SimpleNamespace(project=str(ROOT / "examples/fixture"), arch="arm64", variant="release")
        with patch.object(driver, "resolve_target", return_value=target), \
                patch.object(driver, "resolve_sdk", return_value=ROOT / "dist/sdk/chosen"):
            paths = driver._app_cache_paths({"args": args})
        self.assertIn(ROOT / "examples/fixture/build/linux-arm64-release", paths)
        self.assertIn(ROOT / "examples/fixture/build/delivery-admission", paths)
        self.assertIn(ROOT / "dist/sdk/chosen", paths)

    def test_actual_app_admission_runs_inside_the_shared_inspection_lease(self):
        import tempfile
        from unittest.mock import Mock
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            sdk = project / 'selected-sdk'
            owned = project / 'build/delivery-admission'
            args = SimpleNamespace(project=str(project), arch='x64', variant='dist',
                delivery_profile='embedded', registry_proof=None)
            target = {'host':driver.HOST,'arch':'x86_64'}
            active, events = [], []

            @contextmanager
            def lease(paths):
                self.assertIn(owned, paths)
                active.extend(paths)
                events.append('acquired')
                try:
                    yield
                finally:
                    active.clear()
                    events.append('released')

            def inspect(*unused, **keywords):
                self.assertIn(owned, active)
                events.append('actual-admission-dispatch')
                raise ValueError('stop before compiler')

            delivery = Mock()
            delivery.admit_project.side_effect = inspect
            with patch.dict(driver.sys.modules, {driver.__name__:driver}), \
                 patch.object(driver,'DRY_RUN',False), \
                 patch.object(driver,'resolve_target',return_value=target), \
                 patch.object(driver,'resolve_sdk',return_value=sdk), \
                 patch.object(driver,'_sdk_host_tools',return_value={}), \
                 patch.object(driver,'_confirmed_app_sdk_tools',return_value={}), \
                 patch.object(driver,'_cache_scope',side_effect=lease), \
                 patch.object(driver,'_delivery_module',return_value=delivery), \
                 patch.object(driver,'run') as run:
                with self.assertRaises(SystemExit):
                    driver.cmd_app(args)
                run.assert_not_called()
            self.assertEqual(events,['acquired','actual-admission-dispatch','released'])

    def test_whole_lifecycle_exception_release_and_dry_run(self):
        events = []
        @contextmanager
        def scope(paths):
            events.append(("acquire", paths))
            try:
                yield
            finally:
                events.append("release")
        @driver._cache_owned(lambda values: [values["tree"]])
        def lifecycle(tree):
            events.extend(["configure", "build", "install"])
            raise ValueError("mock failed install")
        with patch.object(driver, "DRY_RUN", False), patch.object(driver, "_cache_scope", side_effect=scope):
            with self.assertRaises(ValueError):
                lifecycle("fixture")
        self.assertEqual([("acquire", ["fixture"]), "configure", "build", "install", "release"], events)
        events.clear()
        with patch.object(driver, "DRY_RUN", True), patch.object(driver, "_cache_scope") as locked:
            with self.assertRaises(ValueError):
                lifecycle("fixture")
            locked.assert_not_called()


if __name__ == "__main__":
    unittest.main()
