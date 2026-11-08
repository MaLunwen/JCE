"""Conan settings overlay controls; no install, cache database or compiler."""
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import importlib.util
import json
import inspect
import os
from pathlib import Path
import tempfile
import unittest
import sys
import types
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

try:
    from conan.internal.model.conf import ConfDefinition
    from conan.internal.model.settings import load_settings_yml
except ModuleNotFoundError:
    CONAN_AVAILABLE = False
else:
    CONAN_AVAILABLE = True

from tools.build.conan_config import configuration_digest, prepare_configuration_home, prepare_toolchain_home
from tools.build import conan_config


@unittest.skipUnless(CONAN_AVAILABLE, "Conan 2 settings subject unavailable")
class ConanConfigurationTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "build/tests/0134-conan-config"
        parent.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.base = self.root / "original"
        self.overlay = self.root / "overlay"
        self._default_hook_fixture = None
        self.base.mkdir()
        # Minimal but real Conan settings schema; user extension must APPEND.
        (self.base / "settings.yml").write_text(
            'compiler:\n  clang:\n    version: ["19", "20", "21"]\n    cppstd: [20]\n', encoding="utf-8")
        (self.base / "settings_user.yml").write_text(
            'compiler:\n  clang:\n    version: ["22"]\ncustom: ["preserved"]\n', encoding="utf-8")
        (self.base / "global.conf").write_text("tools.build:jobs=2\n", encoding="utf-8")
        (self.base / "profiles").mkdir()
        (self.base / "profiles/default").write_text("[settings]\ncompiler=clang\n", encoding="utf-8")
        (self.base / "auth.db").write_bytes(b"private-auth-sentinel")
        (self.base / "remotes.json").write_text(
            '{"remotes":[{"name":"private","url":"https://private.invalid"}]}', encoding="utf-8")
        (self.base / "p").mkdir()
        (self.base / "p/cache.sqlite3").write_bytes(b"database-sentinel")

    def original_hashes(self):
        return {str(p.relative_to(self.base)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in self.base.rglob("*") if p.is_file()}

    def default_hook_driver(self):
        if self._default_hook_fixture is not None:
            return self._default_hook_fixture
        user = self.root / "user"
        user.mkdir()
        default = user / ".conan2"
        self.assertIn(self.root.resolve(), self.base.resolve().parents)
        self.assertIn(self.root.resolve(), default.resolve().parents)
        self.base.rename(default)
        self.base = default
        repository = self.root / "repository"
        (repository / "conan/hooks").mkdir(parents=True)
        (repository / "contracts").mkdir()
        name = "hook_bgfx_wasm_fix.py"
        latest = b"# current configuration-only hook fixture\n"
        (repository / "conan/hooks" / name).write_bytes(latest)
        (repository / "contracts/conan-source-policy.json").write_text(
            json.dumps({"retired_hooks": {name: hashlib.sha256(b"retired generation").hexdigest()}}))
        installed = self.base / "extensions/hooks" / name
        installed.parent.mkdir(parents=True)
        installed.write_bytes(b"# another installed generation; preserve exactly\n")
        spec = importlib.util.spec_from_file_location("jce_default_hook_home", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        self._default_hook_fixture = (driver, repository, user, name, latest)
        return self._default_hook_fixture

    def test_default_hook_conflict_selects_shared_store_overlay_and_reuses_tuple(self):
        driver, repository, user, name, latest = self.default_hook_driver()
        store = self.root / "effective-package-store"
        store.mkdir(exist_ok=True)
        database = store / "cache.sqlite3"
        if not database.exists():
            database.write_bytes(b"existing cached packages")
        self.assertEqual(database.read_bytes(), b"existing cached packages")
        (self.base / "global.conf").write_text(
            "core.cache:storage_path=" + store.as_posix() + "\n"
            "tools.build:jobs=2\nuser.company:token=private-sentinel\n")
        before = self.original_hashes()
        process_home = os.environ.get("CONAN_HOME")
        with patch.object(driver, "ROOT", repository), patch.object(driver.Path, "home", return_value=user):
            env = {}
            driver.sync_conan_hooks(env)
            self.assertIn("CONAN_HOME", env, "selected home must reach the intended process")
            selected = Path(env["CONAN_HOME"])
            self.assertTrue(selected.is_relative_to(repository / "build/conan-config"))
            self.assertEqual((selected / "extensions/hooks" / name).read_bytes(), latest)
            first_hook_stat = (selected / "extensions/hooks" / name).stat()
            fresh_env = {}
            driver.sync_conan_hooks(fresh_env)
            self.assertEqual(fresh_env, env)
            self.assertEqual((selected / "extensions/hooks" / name).stat().st_mtime_ns, first_hook_stat.st_mtime_ns)
            driver.sync_conan_hooks(env)  # Already selected current owned home is valid explicitly.
        self.assertEqual(os.environ.get("CONAN_HOME"), process_home)
        self.assertEqual(self.original_hashes(), before)
        conf = ConfDefinition()
        conf.loads((selected / "global.conf").read_text())
        self.assertEqual(Path(conf.get("core.cache:storage_path")), store)
        self.assertEqual((selected / "settings.yml").read_bytes(), (self.base / "settings.yml").read_bytes())
        self.assertEqual((selected / "settings_user.yml").read_bytes(), (self.base / "settings_user.yml").read_bytes())
        self.assertEqual((selected / "profiles/default").read_text(), "[settings]\ncompiler=clang\n")
        self.assertNotIn("private-sentinel", (selected / "global.conf").read_text())
        self.assertFalse((selected / "auth.db").exists())
        self.assertFalse((selected / "p").exists())
        self.assertEqual((store / "cache.sqlite3").read_bytes(), b"existing cached packages")

    def test_default_hook_fixture_supports_baseline_then_mutant_on_same_instance(self):
        prepared = self.default_hook_driver()
        before = self.fixture_tree_state()
        self.assertEqual(self.default_hook_driver(), prepared)
        self.assertEqual(self.fixture_tree_state(), before, "prepared fixture reuse must not rewrite inputs")
        self.test_default_hook_conflict_selects_shared_store_overlay_and_reuses_tuple()
        self.test_omitted_selected_home_forwarding_fails_ordinary_process_assertion()

    def test_omitted_selected_home_forwarding_fails_ordinary_process_assertion(self):
        source = inspect.getsource(conan_config.sync_hooks)
        wire = '        env["CONAN_HOME"] = config["home"]\n'
        self.assertEqual(source.count(wire), 1)
        namespace = dict(conan_config.__dict__)
        exec(compile(source.replace(wire, "", 1), conan_config.__file__, "exec"), namespace)
        compiled = namespace["sync_hooks"]
        mutant = types.FunctionType(compiled.__code__, conan_config.__dict__, argdefs=compiled.__defaults__)
        mutant.__kwdefaults__ = compiled.__kwdefaults__
        with patch.object(conan_config, "sync_hooks", mutant), self.assertRaisesRegex(AssertionError, "selected home"):
            self.test_default_hook_conflict_selects_shared_store_overlay_and_reuses_tuple()

    def test_default_hook_home_reaches_actual_export_install_and_generation_reuse(self):
        driver, repository, user, _, _ = self.default_hook_driver()
        before = self.original_hashes()
        tc = self.root / "generators/conan_toolchain.cmake"
        calls, exports = [], []
        def run(command, **kwargs):
            calls.append((command, dict(kwargs["env"])))
            tc.parent.mkdir(exist_ok=True)
            tc.write_text("counted Conan generator output; no compiler or install executed\n")
        target = dict(host="linux", profile="fixture", arch="x86_64")
        with patch.object(driver, "ROOT", repository), patch.object(driver, "HOST", "linux"), \
             patch.object(driver.Path, "home", return_value=user), \
             patch.object(driver, "toolchain_path", return_value=tc), \
             patch.object(driver, "conan_dir", return_value=tc.parent), \
             patch.object(driver, "host_build_profile", return_value="fixture"), \
             patch.object(driver, "export_conan_recipes", side_effect=lambda env: exports.append(dict(env))), \
             patch.object(driver, "run", side_effect=run):
            driver.conan_install(target, "Release", {})
            driver.conan_install(target, "Release", {})
        self.assertEqual(len(calls), 1)
        self.assertEqual(len(exports), 1)
        self.assertEqual(calls[0][1]["CONAN_HOME"], exports[0]["CONAN_HOME"])
        self.assertNotEqual(Path(calls[0][1]["CONAN_HOME"]), self.base)
        self.assertEqual(before, self.original_hashes())

    def test_explicit_different_hook_home_still_refuses_without_outputs(self):
        driver, repository, user, _, _ = self.default_hook_driver()
        before = self.original_hashes()
        with patch.object(driver, "ROOT", repository), patch.object(driver.Path, "home", return_value=user):
            with self.assertRaises(SystemExit):
                driver.sync_conan_hooks({"CONAN_HOME": str(self.base)})
        self.assertFalse((repository / "build").exists())
        self.assertEqual(before, self.original_hashes())

    def test_default_installed_hook_symlink_refuses_before_managed_outputs(self):
        driver, repository, user, name, _ = self.default_hook_driver()
        installed = self.base / "extensions/hooks" / name
        original = self.root / "untouched-installed-hook.py"
        installed.rename(original)
        try:
            installed.symlink_to(original)
        except OSError as error:
            original.rename(installed)
            self.skipTest("symlink creation unavailable: " + str(error))
        original_bytes = original.read_bytes()
        with patch.object(driver, "ROOT", repository), patch.object(driver.Path, "home", return_value=user):
            with self.assertRaises(SystemExit):
                driver.sync_conan_hooks({})
        self.assertFalse((repository / "build").exists())
        self.assertEqual(original.read_bytes(), original_bytes)
        self.assertTrue(installed.is_symlink())

    def test_overlay_temporary_symlink_refuses_before_rewriting_any_configuration(self):
        prepare_configuration_home(self.base, self.overlay)
        external = self.root / "untouched-temporary-target"
        external.write_bytes(b"do not overwrite through temporary configuration link")
        temporary = self.overlay / "settings.yml.jce-config.tmp"
        try:
            temporary.symlink_to(external)
        except OSError as error:
            self.skipTest("symlink creation unavailable: " + str(error))
        def metadata():
            return {str(path.relative_to(self.overlay)): (path.read_bytes(), path.stat().st_mtime_ns)
                    for path in self.overlay.rglob("*") if path.is_file()}
        before = metadata()
        with self.assertRaises(ValueError):
            prepare_configuration_home(self.base, self.overlay)
        self.assertEqual(metadata(), before)
        self.assertEqual(external.read_bytes(), b"do not overwrite through temporary configuration link")

    def test_native_configuration_symlinks_refuse_before_any_overlay_output(self):
        for name in ("settings.yml", "settings_user.yml", "global.conf", "profiles"):
            with self.subTest(name=name):
                subject = self.base / name
                original = self.root / ("actual-" + name)
                subject.rename(original)
                try:
                    subject.symlink_to(original, target_is_directory=original.is_dir())
                except OSError as error:
                    original.rename(subject)
                    self.skipTest("symlink creation unavailable: " + str(error))
                try:
                    with self.assertRaises(ValueError):
                        prepare_configuration_home(self.base, self.overlay)
                    self.assertFalse(self.overlay.exists())
                finally:
                    subject.unlink()
                    original.rename(subject)

    def test_unsafe_default_profile_refuses_before_managed_home_or_original_hook_writes(self):
        driver, repository, user, _, _ = self.default_hook_driver()
        (self.base / "profiles/default").write_text("[buildenv]\nPASSWORD=private\n")
        before = self.original_hashes()
        with patch.object(driver, "ROOT", repository), patch.object(driver.Path, "home", return_value=user):
            with self.assertRaises(SystemExit):
                driver.sync_conan_hooks({})
        self.assertFalse((repository / "build").exists())
        self.assertEqual(before, self.original_hashes())

    def check_hook_gate(self, repository, user, env=None):
        spec = importlib.util.spec_from_file_location(
            "conan_hook_gate_subject", ROOT / "tools/lint/check_conan_hooks_synced.py")
        gate = importlib.util.module_from_spec(spec)
        output, errors = io.StringIO(), io.StringIO()
        with patch.dict(os.environ, env or {}, clear=True), \
             patch.object(Path, "home", return_value=user), \
             patch.object(sys, "argv", [str(spec.origin)]), \
             redirect_stdout(output), redirect_stderr(errors):
            spec.loader.exec_module(gate)
            gate.REPO_ROOT = repository
            gate.SRC = repository / "conan/hooks"
            result = gate.main()
        return result, output.getvalue(), errors.getvalue()

    def fixture_tree_state(self):
        return {str(path.relative_to(self.root)): (
                    path.stat().st_mode, path.stat().st_mtime_ns,
                    path.read_bytes() if path.is_file() else None)
                for path in self.root.rglob("*")}

    def test_hook_gate_default_conflict_checks_actual_managed_overlay_read_only(self):
        driver, repository, user, _, _ = self.default_hook_driver()
        env = {}
        with patch.object(driver, "ROOT", repository), patch.object(Path, "home", return_value=user):
            driver.sync_conan_hooks(env)
        selected = Path(env["CONAN_HOME"])
        before = self.fixture_tree_state()
        with patch.object(conan_config, "select_hook_home", wraps=conan_config.select_hook_home) as select:
            result, output, errors = self.check_hook_gate(repository, user)
        self.assertEqual(select.call_count, 1, "gate must consume the shared selector")
        self.assertEqual(select.call_args.args[0], repository)
        self.assertEqual(result, 0, errors)
        self.assertIn(str(selected), output, "gate must inspect the build-selected home")
        self.assertIn("planned", output.lower())
        self.assertIn("execution", output.lower())
        self.assertEqual(before, self.fixture_tree_state(), "read-only gate wrote configuration")

    def test_hook_gate_disconnected_selector_fails_ordinary_status_assertion(self):
        driver, repository, user, _, _ = self.default_hook_driver()
        env = {}
        with patch.object(driver, "ROOT", repository), patch.object(Path, "home", return_value=user):
            driver.sync_conan_hooks(env)
        original = conan_config.select_hook_home
        def disconnected(repository, env):
            selection = original(repository, env)
            selection["home"] = selection["base"]
            selection["destination"] = selection["base"] / "extensions/hooks"
            return selection
        before = self.fixture_tree_state()
        with patch.object(conan_config, "select_hook_home", side_effect=disconnected), \
             self.assertRaisesRegex(AssertionError, "1 != 0"):
            result, _, errors = self.check_hook_gate(repository, user)
            self.assertEqual(result, 0, errors)
        self.assertEqual(before, self.fixture_tree_state())

    def test_hook_home_selection_is_read_only_and_does_not_mutate_environment(self):
        _, repository, user, _, _ = self.default_hook_driver()
        env = {}
        before = self.fixture_tree_state()
        with patch.object(Path, "home", return_value=user):
            selection = conan_config.select_hook_home(repository, env)
        self.assertEqual(env, {})
        self.assertNotEqual(selection["home"], selection["base"])
        self.assertFalse(selection["home"].exists())
        self.assertFalse((repository / "build").exists())
        self.assertEqual(before, self.fixture_tree_state())

    def test_hook_gate_missing_clean_home_retains_not_installed_semantics(self):
        _, repository, user, name, _ = self.default_hook_driver()
        (self.base / "extensions/hooks" / name).unlink()
        before = self.fixture_tree_state()
        result, output, errors = self.check_hook_gate(repository, user)
        self.assertEqual(result, 0, errors)
        self.assertIn(str(self.base), output)
        self.assertIn("not installed", output.lower())
        self.assertFalse((repository / "build").exists())
        self.assertEqual(before, self.fixture_tree_state())

    def test_hook_gate_rejects_stale_selected_overlay_without_reselection(self):
        driver, repository, user, name, _ = self.default_hook_driver()
        env = {}
        with patch.object(driver, "ROOT", repository), patch.object(Path, "home", return_value=user):
            driver.sync_conan_hooks(env)
        selected = Path(env["CONAN_HOME"])
        (selected / "extensions/hooks" / name).write_bytes(b"stale selected hook\n")
        before = self.fixture_tree_state()
        result, output, errors = self.check_hook_gate(repository, user)
        self.assertEqual(result, 1)
        self.assertIn(str(selected), output + errors)
        self.assertIn(name, errors)
        self.assertEqual(before, self.fixture_tree_state())

    def test_hook_gate_explicit_stale_home_refuses_and_never_creates_overlay(self):
        _, repository, user, name, _ = self.default_hook_driver()
        before = self.fixture_tree_state()
        result, output, errors = self.check_hook_gate(
            repository, user, {"CONAN_HOME": str(self.base)})
        self.assertEqual(result, 1)
        self.assertIn(str(self.base), output + errors)
        self.assertIn(name, errors)
        self.assertFalse((repository / "build").exists())
        self.assertEqual(before, self.fixture_tree_state())

    def test_hook_gate_changed_hook_tuple_only_plans_new_uninstalled_home(self):
        driver, repository, user, name, _ = self.default_hook_driver()
        env = {}
        with patch.object(driver, "ROOT", repository), patch.object(Path, "home", return_value=user):
            driver.sync_conan_hooks(env)
        old_selected = Path(env["CONAN_HOME"])
        (repository / "conan/hooks" / name).write_bytes(b"# next owned hook tuple\n")
        before = self.fixture_tree_state()
        result, output, errors = self.check_hook_gate(repository, user)
        self.assertEqual(result, 0, errors)
        self.assertNotIn(str(old_selected), output + errors)
        self.assertIn("not installed", output.lower())
        self.assertIn("planned", output.lower())
        self.assertEqual(before, self.fixture_tree_state())

    def test_hook_gate_missing_default_overlay_does_not_create_it(self):
        _, repository, user, _, _ = self.default_hook_driver()
        before = self.fixture_tree_state()
        result, output, errors = self.check_hook_gate(repository, user)
        self.assertEqual(result, 0, errors)
        self.assertIn(str(repository / "build/conan-config"), output)
        self.assertIn("not installed", output.lower())
        self.assertFalse((repository / "build").exists())
        self.assertEqual(before, self.fixture_tree_state())

    def test_hook_gate_retired_source_hook_in_selected_home_still_fails(self):
        driver, repository, user, _, _ = self.default_hook_driver()
        env = {}
        with patch.object(driver, "ROOT", repository), patch.object(Path, "home", return_value=user):
            driver.sync_conan_hooks(env)
        retired_name, retired_bytes = "hook_retired_source_writer.py", b"retired source writer\n"
        # Select the new policy tuple first, then install retired bytes into
        # its exact destination. Installed bytes must not create another tuple.
        policy_path = repository / "contracts/conan-source-policy.json"
        policy = json.loads(policy_path.read_text())
        policy["retired_hooks"][retired_name] = hashlib.sha256(retired_bytes).hexdigest()
        policy_path.write_text(json.dumps(policy))
        env = {}
        with patch.object(driver, "ROOT", repository), patch.object(Path, "home", return_value=user):
            driver.sync_conan_hooks(env)
        selected = Path(env["CONAN_HOME"])
        (selected / "extensions/hooks" / retired_name).write_bytes(retired_bytes)
        before = self.fixture_tree_state()
        result, output, errors = self.check_hook_gate(repository, user)
        self.assertEqual(result, 1)
        self.assertIn(retired_name, errors)
        self.assertIn("RETIRED", errors)
        self.assertEqual(before, self.fixture_tree_state())

    def check_native_configuration(self, prepare):
        settings_bytes = b'compiler:\n  msvc:\n    version: ["193"]\n    cppstd: [20]\n'
        user_bytes = (b'# Native settings must not gain a made-up Clang version.\n'
                      b'compiler:\n  msvc:\n    version: ["194"]\ncustom: ["native"]\n')
        (self.base / "settings.yml").write_bytes(settings_bytes)
        (self.base / "settings_user.yml").write_bytes(user_bytes)
        (self.base / "profiles/default").write_text(
            "[settings]\ncompiler=msvc\ncompiler.version=194\ncompiler.cppstd=20\n", encoding="utf-8")
        store = self.root / "shared native store"
        store.mkdir()
        database = store / "cache.sqlite3"
        database.write_bytes(b"existing-native-package-database")
        (self.base / "global.conf").write_text(
            "core.cache:storage_path=" + store.as_posix() + "\n"
            "tools.build:jobs=2\nuser.company:token=private-sentinel\n", encoding="utf-8")
        old_hook = self.base / "extensions/hooks/hook_bgfx_wasm_fix.py"
        old_hook.parent.mkdir(parents=True)
        old_hook.write_bytes(b"# old first-party installed configuration; preserve original\n")
        before = self.original_hashes()
        result = prepare(self.base, self.overlay)
        self.assertEqual(before, self.original_hashes())
        self.assertEqual((self.overlay / "settings.yml").read_bytes(), settings_bytes)
        self.assertEqual((self.overlay / "settings_user.yml").read_bytes(), user_bytes)
        self.assertIsNone(result["clang_major"])
        merged = load_settings_yml(str(self.overlay))
        merged.compiler = "msvc"
        for version in ("193", "194"):
            merged.compiler.version = version
            merged.compiler.version.validate()
        self.assertEqual(Path(result["storage"]), store)
        configuration = ConfDefinition()
        configuration.loads((self.overlay / "global.conf").read_text())
        self.assertEqual(Path(configuration.get("core.cache:storage_path")), store)
        self.assertEqual(database.read_bytes(), b"existing-native-package-database")
        self.assertNotIn("private-sentinel", (self.overlay / "global.conf").read_text())
        for excluded in ("p", "auth.db", "extensions"):
            self.assertFalse((self.overlay / excluded).exists())
        return result, before

    def test_native_configuration_preserves_real_settings_store_and_original_hooks(self):
        result, before = self.check_native_configuration(prepare_configuration_home)
        # Exercise the actual shared synchronizer against the new owned home.
        # It must never overwrite the original installed hook or copy auth.
        spec = importlib.util.spec_from_file_location("jce_native_overlay_hooks", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        driver.sync_conan_hooks({"CONAN_HOME": result["home"]})
        new_hook = self.overlay / "extensions/hooks/hook_bgfx_wasm_fix.py"
        self.assertEqual(new_hook.read_bytes(), (ROOT / "conan/hooks/hook_bgfx_wasm_fix.py").read_bytes())
        self.assertEqual(before, self.original_hashes())
        again = prepare_configuration_home(self.base, self.overlay)
        self.assertEqual(again, result)
        self.assertEqual(new_hook.read_bytes(), (ROOT / "conan/hooks/hook_bgfx_wasm_fix.py").read_bytes())

    def test_native_missing_user_settings_preservation_is_an_ordinary_failure(self):
        path = ROOT / "tools/build/conan_config.py"
        source = path.read_text(encoding="utf-8")
        wire = "user_output = user_bytes"
        self.assertEqual(source.count(wire), 1, "expected one native settings-preservation wire")
        namespace = {}
        exec(compile(source.replace(wire, 'user_output = b"{}\\n"'), str(path), "exec"), namespace)
        # The complete common owner still prepares valid directories/settings.
        # Losing the Native extension must fail unchanged behavioral assertions.
        with self.assertRaises(AssertionError):
            self.check_native_configuration(namespace["prepare_configuration_home"])

    def test_configuration_digest_is_bounded_and_excludes_auth_store_hooks(self):
        before = self.original_hashes()
        initial = configuration_digest(self.base)
        self.assertEqual(initial, configuration_digest(self.base))
        self.assertEqual(before, self.original_hashes())
        (self.base / "auth.db").write_bytes(b"changed private auth")
        (self.base / "p/cache.sqlite3").write_bytes(b"changed package database")
        hook = self.base / "extensions/hooks/ignored.py"
        hook.parent.mkdir(parents=True)
        hook.write_bytes(b"# installed hooks are not configuration-digest inputs\n")
        self.assertEqual(initial, configuration_digest(self.base))
        profile = self.base / "profiles/default"
        profile.write_text(profile.read_text() + "compiler.version=22\n")
        self.assertNotEqual(initial, configuration_digest(self.base))
        profile.unlink()
        missing = configuration_digest(self.root / "absent-home")
        self.assertEqual(missing, configuration_digest(self.root / "absent-home"))
        settings = self.base / "settings.yml"
        settings.write_bytes(b"x" * (1024 * 1024 + 1))
        with self.assertRaisesRegex(ValueError, "small-file budget"):
            configuration_digest(self.base)

    def test_configuration_digest_access_errors_are_never_missing_inputs(self):
        original_stat = Path.stat
        for subject in (self.base, *(self.base / name for name in
                        ("settings.yml", "settings_user.yml", "global.conf", "profiles"))):
            def denied(path, *args, **kwargs):
                if path == subject:
                    raise PermissionError("configuration access denied sentinel")
                return original_stat(path, *args, **kwargs)
            with self.subTest(subject=subject.name), patch.object(Path, "stat", denied):
                with self.assertRaisesRegex(PermissionError, "configuration access denied sentinel"):
                    configuration_digest(self.base)
        original_scan = conan_config.os.scandir
        def denied_profiles(path):
            if Path(path) == self.base / "profiles":
                raise PermissionError("profile enumeration denied sentinel")
            return original_scan(path)
        with patch.object(conan_config.os, "scandir", denied_profiles):
            with self.assertRaisesRegex(PermissionError, "profile enumeration denied sentinel"):
                configuration_digest(self.base)

    def exercise_native_configuration_reuse(self, driver):
        self.check_native_configuration(prepare_configuration_home)
        source_conf = self.base / "global.conf"
        source_conf.write_text(source_conf.read_text() + 'tools.build:defines=["CALLER_SETTING=0"]\n')
        prepare_configuration_home(self.base, self.overlay)
        tc = self.root / "generators/conan_toolchain.cmake"
        tc.parent.mkdir(); tc.touch()
        commands = []
        target = {"host": "linux", "profile": "fixture", "arch": "x86_64"}
        env = {"CONAN_HOME": str(self.overlay), "JCE_BGFX_CPU_PROFILER": "on"}
        with patch.object(driver, "ROOT", self.root), patch.object(driver, "HOST", "linux"), \
                patch.object(driver, "sync_conan_hooks"), patch.object(driver, "export_conan_recipes"), \
                patch.object(driver, "toolchain_path", return_value=tc), \
                patch.object(driver, "conan_dir", return_value=self.root / "generators"), \
                patch.object(driver, "host_build_profile", return_value="fixture"), \
                patch.object(driver, "run", side_effect=lambda command, **kwargs: commands.append(command)):
            driver.conan_install(target, "Release", env)
            driver.conan_install(target, "Release", env)
            self.assertEqual(len(commands), 1)
            source_conf.write_text(source_conf.read_text())  # Same bytes, different write time.
            prepare_configuration_home(self.base, self.overlay)
            driver.conan_install(target, "Release", env)
            self.assertEqual(len(commands), 1)
            source_conf.write_text(source_conf.read_text().replace("CALLER_SETTING=0", "CALLER_SETTING=1"))
            prepare_configuration_home(self.base, self.overlay)
            before = self.original_hashes()
            driver.conan_install(target, "Release", env)
            self.assertEqual(len(commands), 2, "native configuration refresh must regenerate metadata")
            self.assertEqual(before, self.original_hashes())
            driver.conan_install(target, "Release", env)
            self.assertEqual(len(commands), 2)
            with patch.object(driver.Path, "home", return_value=self.root):
                # Explicit and default-home selection both consume actual config.
                default = self.root / ".conan2"
                default.mkdir()
                first = driver.conan_generation_fingerprint(["fixture"], {})
                (default / "global.conf").write_text("tools.build:jobs=2\n")
                self.assertNotEqual(first, driver.conan_generation_fingerprint(["fixture"], {}))
            bad = self.overlay / "settings.yml"
            bad.unlink(); bad.mkdir()
            with self.assertRaises(SystemExit):
                driver.conan_install(target, "Release", env)
            self.assertEqual(len(commands), 2)

    def native_reuse_driver(self, source=None):
        path = ROOT / "tools/build/jce.py"
        spec = importlib.util.spec_from_file_location("jce_native_config_reuse", path)
        driver = importlib.util.module_from_spec(spec)
        if source is None:
            spec.loader.exec_module(driver)
        else:
            exec(compile(source, str(path), "exec"), driver.__dict__)
        return driver

    def test_native_same_home_config_change_regenerates_and_identical_config_reuses(self):
        self.exercise_native_configuration_reuse(self.native_reuse_driver())

    def test_omitted_configuration_digest_wire_fails_ordinary_reuse_assertion(self):
        source = (ROOT / "tools/build/jce.py").read_text(encoding="utf-8")
        wire = '"configuration_sha256": configuration_sha256'
        self.assertEqual(source.count(wire), 1)
        driver = self.native_reuse_driver(source.replace(wire, '"configuration_sha256": None'))
        with self.assertRaisesRegex(AssertionError, "native configuration refresh must regenerate metadata"):
            self.exercise_native_configuration_reuse(driver)

    def test_actual_conan_merge_adds_23_preserves_original_and_shared_database(self):
        before = self.original_hashes()
        result = prepare_toolchain_home(self.base, self.overlay, "23")
        settings = load_settings_yml(str(self.overlay))
        settings.compiler = "clang"
        for version in ("19", "21", "22", "23"):
            settings.compiler.version = version
            settings.compiler.version.validate()
        self.assertEqual(before, self.original_hashes())
        self.assertEqual(Path(result["storage"]), self.base / "p")
        self.assertFalse((self.overlay / "p").exists())
        self.assertFalse((self.overlay / "auth.db").exists())
        user = json.loads((self.overlay / "settings_user.yml").read_text())
        self.assertEqual(user["custom"], ["preserved"])
        self.assertEqual(user["compiler"]["clang"]["version"], ["22", "23"])
        remotes = json.loads((self.overlay / "remotes.json").read_text())
        self.assertEqual(remotes["remotes"][0]["url"], "https://center2.conan.io")

    def test_respects_explicit_user_storage_and_filters_authentication(self):
        store = self.root / "real custom storage"
        store.mkdir()
        (self.base / "global.conf").write_text(
            "core.cache:storage_path=" + store.as_posix() + "\n"
            "tools.build:jobs=2\nuser.company:token=private-sentinel\n"
            "user.company:headers={'Authorization': 'Bearer private-header'}\n", encoding="utf-8")
        result = prepare_toolchain_home(self.base, self.overlay, "23")
        self.assertEqual(Path(result["storage"]), store)
        text = (self.overlay / "global.conf").read_text()
        self.assertNotIn("private-sentinel", text)
        self.assertNotIn("private-header", text)
        configuration = ConfDefinition()
        configuration.loads(text)
        self.assertEqual(Path(configuration.get("core.cache:storage_path")), store)
        self.assertEqual(configuration.get("tools.build:jobs"), 2)

    def test_repeated_prepare_and_version_upgrade_are_idempotent(self):
        prepare_toolchain_home(self.base, self.overlay, "23")
        prepare_toolchain_home(self.base, self.overlay, "23")
        user = json.loads((self.overlay / "settings_user.yml").read_text())
        self.assertEqual(user["compiler"]["clang"]["version"].count("23"), 1)
        prepare_toolchain_home(self.base, self.overlay, "24")
        settings = load_settings_yml(str(self.overlay))
        settings.compiler = "clang"
        settings.compiler.version = "24"
        settings.compiler.version.validate()

    def test_original_or_overlapping_home_is_rejected(self):
        before = self.original_hashes()
        for path in (self.base, self.base / "nested", self.root):
            with self.assertRaises(ValueError):
                prepare_toolchain_home(self.base, path, "23")
            with self.assertRaises(ValueError):
                prepare_configuration_home(self.base, path)
        self.assertEqual(before, self.original_hashes())

    def test_unowned_overlay_is_not_overwritten(self):
        self.overlay.mkdir()
        existing = self.overlay / "existing"
        existing.write_bytes(b"do-not-overwrite")
        with self.assertRaises(ValueError):
            prepare_toolchain_home(self.base, self.overlay, "23")
        with self.assertRaises(ValueError):
            prepare_configuration_home(self.base, self.overlay)
        self.assertEqual(existing.read_bytes(), b"do-not-overwrite")

    def test_secret_profile_and_invalid_major_fail_before_writes(self):
        (self.base / "profiles/default").write_text("[buildenv]\nPASSWORD=private\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            prepare_toolchain_home(self.base, self.overlay, "23")
        self.assertFalse(self.overlay.exists())
        for version in ("19.0.0", "", "0", 23):
            with self.assertRaises(ValueError):
                prepare_toolchain_home(self.base, self.overlay, version)


if __name__ == "__main__":
    if not CONAN_AVAILABLE:
        print("SKIP: Conan 2 settings subject unavailable (exit 77)")
        raise SystemExit(77)
    unittest.main()
