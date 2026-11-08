"""SDK smoke-driver protocol and compiler-free runtime-stage controls.

Real bounded metadata/roster files and kernel lease owners are consumed. Compiler,
shipped-tool execution and consumer processes are counted ports; passing this
file is CPU protocol evidence, never installed-SDK linkage/runtime/GPU evidence.
The two consumer/integration CTest gates supply that separate native evidence.
"""
import argparse
from contextlib import contextmanager, redirect_stderr
import importlib.util
import io
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "tests/consumers/sdk_smoke_gate.py"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class SdkConsumerGateTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="jce-sdk-consumer-protocol-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.sdk = self.root / "renamed-installed-sdk"
        self.cmake = self.sdk / "lib/cmake/JCE"
        self.cmake.mkdir(parents=True)
        (self.cmake / "JCEConfig.cmake").write_text("# presence-only CPU fixture\n")
        self.driver = load("jce_sdk_consumer_control", ROOT / "tools/build/jce.py")
        self.cache = load("jce_sdk_consumer_cache", ROOT / "tools/build/cache_lock.py")
        self.target = self.driver.resolve_target(None)
        self.bdir = self.root / "consumer-scratch"
        self.release = self.sdk / "lib/Release"
        self.release.mkdir()
        system = {"windows": "Windows", "linux": "Linux", "darwin": "Darwin"}[self.driver.HOST]
        self.data = dict(schema=1, system=system, arch=self.target["arch"],
                         pointer_bits=self.target["bits"], config="Release", variant="release",
                         delivery="authoring", crt="MultiThreaded" if system == "Windows" else "none",
                         archive="jce_engine_core.lib" if system == "Windows" else "jce_engine_core.a")
        self.write_metadata()
        (self.release / self.data["archive"]).write_bytes(b"PRESENCE_ONLY_NOT_ABI")
        (self.sdk / "bin").mkdir()
        suffix = ".exe" if self.driver.HOST == "windows" else ""
        (self.sdk / "bin" / ("jce_cook" + suffix)).write_bytes(b"COUNTED_TOOL_PORT")
        self.lease_resources = []

    def write_metadata(self):
        (self.release / "jce-sdk-target.json").write_text(json.dumps(self.data), encoding="utf-8")

    def args(self, consumer="c99", **changes):
        values = dict(arch=self.target["arch"], sdk=str(self.sdk), variant="release",
                      consumer=consumer, require_run=True, build_dir=self.bdir,
                      delivery_profile="authoring", introspect="", introspect_out="")
        values.update(changes)
        return argparse.Namespace(**values)

    @contextmanager
    def lease(self, resources):
        self.lease_resources.append([Path(resource).resolve() for resource in resources])
        with self.cache.cache_locks(resources, self.root / "locks", timeout=0):
            yield

    def artifacts(self, scripting=False):
        self.bdir.mkdir(exist_ok=True)
        stem = "jce_sdk_smoke_scripting" if scripting else "jce_sdk_smoke"
        suffix = ".exe" if self.driver.HOST == "windows" else ""
        (self.bdir / (stem + suffix)).write_bytes(b"COUNTED_RUNTIME_PORT" * 20)
        if not scripting:
            cooked = self.bdir / "JceSdkSmoke_cooked"
            cooked.mkdir(exist_ok=True)
            (cooked / "smoke.png").write_bytes(b"JCEA" + b"COOKED_PORT")
            (self.bdir / "JceSdkSmoke_assets.pak").write_bytes(b"PAK_PORT")

    def roster(self, language, complete=True, text=None):
        destination = self.cmake / "backends" / language / "jce_backend.cmake"
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(text or f'set(JCE_BACKEND_LANGUAGE "{language}")\n'
            f'set(JCE_BACKEND_VM_TARGET "jce_script_vm_{language}")\n'
            'set(JCE_BACKEND_NEEDS_SCRIPT_API OFF)\n', encoding="utf-8")
        if complete:
            (self.release / f"jce_script_vm_{language}.lib").write_bytes(b"LINK_PRESENCE_ONLY")

    def availability(self, catalog=("c", "cpp"), compiled=("c", "cpp")):
        text = ('set(JCE_SCRIPT_API_STATIC ON)\n'
                'set(JCE_SCRIPT_AVAILABILITY_SCHEMA 1)\n'
                'set(JCE_SCRIPT_CATALOG_LANGUAGES "' + ';'.join(catalog) + '")\n'
                'set(JCE_SCRIPT_COMPILED_LANGUAGES "' + ';'.join(compiled) + '")\n')
        (self.cmake / "JCEScripting.cmake").write_text(text, encoding="utf-8")
        return text

    @contextmanager
    def ports(self, stdout="JCE_SMOKE: OK version=cpu-protocol pak_assets=1\n", exit_code=0):
        resolve = self.driver.resolve_sdk
        def owned_admission(args, target):
            self.assertEqual(len(self.cache._HELD), 2, "metadata must be read inside existing SDK/scratch lease")
            return resolve(args, target)
        with patch.object(self.driver, "_cache_scope", self.lease), \
             patch.object(self.driver, "resolve_sdk", side_effect=owned_admission), \
             patch.object(self.driver, "msvc_env", return_value={**os.environ, "CMAKE_BUILD_PARALLEL_LEVEL": "2"}), \
             patch.object(self.driver, "_tool_runs", return_value=True), \
             patch.object(self.driver, "run") as commands, \
             patch.object(self.driver.subprocess, "run", return_value=subprocess.CompletedProcess(
                 ["COUNTED_RUNTIME_PORT"], exit_code, stdout, "")) as runtime:
            yield commands, runtime
        self.assertFalse(self.cache._HELD, "kernel leases must unwind on success/failure")

    def test_missing_explicit_sdk_is_registered_skip_and_has_no_scratch(self):
        result = subprocess.run([sys.executable, "-B", str(GATE), "--consumer=c99", "--sdk=",
            "--build-dir=" + str(self.bdir)], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 77, result.stderr)
        self.assertIn("JCE_TEST_SDK_DIR", result.stdout)
        self.assertIn("SKIPPED", result.stdout)
        self.assertNotIn("PASS", result.stdout)
        self.assertFalse(self.bdir.exists())

    def test_explicit_invalid_sdk_fails_before_any_consumer_build(self):
        controls = ("missing_config", "missing_archive", "wrong_arch", "wrong_variant", "wrong_crt")
        for control in controls:
            with self.subTest(control=control):
                config = self.cmake / "JCEConfig.cmake"
                archive = self.release / self.data["archive"]
                config.write_text("# fixture config\n")
                archive.write_bytes(b"PRESENCE_ONLY")
                old = dict(self.data)
                if control == "missing_config": config.unlink()
                if control == "missing_archive": archive.unlink()
                if control == "wrong_arch":
                    self.data["arch"] = "aarch64" if old["arch"] != "aarch64" else "x86_64"
                    self.data["pointer_bits"] = 64
                if control == "wrong_variant": self.data["variant"] = "dist"
                if control == "wrong_crt": self.data["crt"] = "INVALID_CRT"
                self.write_metadata()
                with self.ports() as (commands, runtime):
                    with self.assertRaises(SystemExit) as failed:
                        self.driver.cmd_smoke(self.args())
                    self.assertNotIn(failed.exception.code, (0, 77))
                    commands.assert_not_called()
                    runtime.assert_not_called()
                self.data = old
                self.write_metadata()
        self.assertFalse(self.bdir.exists())

    def test_c99_actual_owner_consumes_installed_sdk_and_isolated_lease(self):
        self.artifacts()
        with self.ports() as (commands, runtime):
            self.driver.cmd_smoke(self.args())
            self.assertEqual(commands.call_count, 2)
            configure, build = [call.args[0] for call in commands.call_args_list]
            self.assertEqual(configure[configure.index("-S") + 1], str(ROOT / "tests/sdk_smoke"))
            self.assertEqual(configure[configure.index("-B") + 1], str(self.bdir))
            self.assertIn("-DJCE_DIR=" + str(self.cmake), configure)
            self.assertIn("-DJCE_BUILD_VARIANT=release", configure)
            self.assertIn("-DJCE_DELIVERY_PROFILE=authoring", configure)
            self.assertEqual(build, ["cmake", "--build", str(self.bdir), "--parallel", "2"])
            self.assertEqual(runtime.call_count, 1)
            self.assertEqual(runtime.call_args.kwargs["env"]["JCE_BACKEND"], "noop")
            self.assertEqual(runtime.call_args.kwargs["cwd"], str(self.bdir))
        self.assertEqual(self.lease_resources, [[self.bdir, self.sdk]])

    def test_dist_consumer_keeps_metadata_and_authenticated_asset_owner(self):
        self.data.update(variant="dist", delivery="embedded")
        self.write_metadata()
        self.artifacts()
        generated = self.bdir / "jce_generated"
        generated.mkdir()
        (generated / "project_assets.pak").write_bytes(b"COUNTED_AUTH_PAK_PORT")
        flags = ["-DJCE_PROJECT_PREBUILT_ASSETS_OBJ=" + str(generated / "project_assets.obj"),
                 "-DJCE_PROJECT_PREBUILT_PAK_KEY_C=" + str(generated / "jce_pak_key.c")]
        with self.ports() as (commands, runtime), \
             patch.object(self.driver, "_sdk_smoke_assets_module") as load_assets:
            owner = load_assets.return_value
            owner.prepare.return_value = flags
            self.driver.cmd_smoke(self.args(variant="dist", delivery_profile="embedded"))
            configure = commands.call_args_list[0].args[0]
            self.assertIn("-DJCE_BUILD_VARIANT=dist", configure)
            self.assertIn("-DJCE_DELIVERY_PROFILE=embedded", configure)
            self.assertIn("JCE_PROJECT_PREBUILT_ASSETS_*", configure)
            for flag in flags:
                self.assertIn(flag, configure)
            owner.prepare.assert_called_once()
            owner.verify.assert_called_once_with(self.bdir)
            self.assertEqual(runtime.call_count, 1)

    def test_dist_asset_admission_failure_never_configures_or_runs(self):
        self.data.update(variant="dist", delivery="embedded")
        self.write_metadata()
        with self.ports() as (commands, runtime), \
             patch.object(self.driver, "_sdk_smoke_assets_module") as load_assets:
            load_assets.return_value.prepare.side_effect = ValueError("actual selected SDK tool unavailable")
            with self.assertRaises(SystemExit) as failed:
                self.driver.cmd_smoke(self.args(variant="dist", delivery_profile="embedded"))
            self.assertNotIn(failed.exception.code, (0, 77))
            commands.assert_not_called()
            runtime.assert_not_called()
        self.assertFalse(self.bdir.exists())

    def test_dist_proof_failure_after_link_never_runs_consumer(self):
        self.data.update(variant="dist", delivery="embedded")
        self.write_metadata()
        self.artifacts()
        with self.ports() as (commands, runtime), \
             patch.object(self.driver, "_sdk_smoke_assets_module") as load_assets:
            load_assets.return_value.prepare.return_value = []
            load_assets.return_value.verify.side_effect = ValueError("authenticated BOM changed after admission")
            with self.assertRaises(SystemExit) as failed:
                self.driver.cmd_smoke(self.args(variant="dist", delivery_profile="embedded"))
            self.assertNotIn(failed.exception.code, (0, 77))
            self.assertEqual(commands.call_count, 2)
            runtime.assert_not_called()

    def scripting_dist_inputs(self):
        self.data.update(variant="dist", delivery="embedded")
        self.write_metadata()
        self.availability()
        self.roster("c")
        self.roster("cpp")

    def test_scripting_dist_uses_its_fixture_and_verifies_after_build_before_launch(self):
        self.scripting_dist_inputs()
        self.artifacts(scripting=True)
        events = []
        generated = self.bdir / "jce_generated"
        flags = ["-DJCE_PROJECT_PREBUILT_ASSETS_OBJ=" + str(generated / "project_assets.obj"),
                 "-DJCE_PROJECT_PREBUILT_PAK_KEY_C=" + str(generated / "jce_pak_key.c")]
        stdout = "JCE_SMOKE_SCRIPTING: OK version=cpu-protocol languages=3\n"
        with self.ports(stdout) as (commands, runtime), \
             patch.object(self.driver, "_sdk_smoke_assets_module") as load_assets:
            owner = load_assets.return_value

            def prepare(driver, target, sdk, directory, environment, *, fixture):
                self.assertIs(driver, self.driver)
                self.assertEqual(target, self.target)
                self.assertEqual(sdk, self.sdk)
                self.assertEqual(directory, self.bdir)
                self.assertEqual(fixture, "scripting")
                self.assertEqual(len(self.cache._HELD), 2)
                self.assertEqual(environment["CMAKE_BUILD_PARALLEL_LEVEL"], "2")
                events.append("scripting-admission")
                return flags

            def command(arguments, **_):
                events.append("build" if arguments[1] == "--build" else "configure")

            def verify(directory, *, fixture):
                self.assertEqual(directory, self.bdir)
                self.assertEqual(fixture, "scripting")
                self.assertEqual(events, ["scripting-admission", "configure", "build"])
                runtime.assert_not_called()
                events.append("scripting-proof")
                return {"fixture": "scripting", "scope": "COUNTED_PROOF_PORT"}

            def launch(*args, **kwargs):
                self.assertEqual(events[-1], "scripting-proof")
                events.append("launch")
                return subprocess.CompletedProcess(args[0], 0, stdout, "")

            owner.prepare.side_effect = prepare
            owner.verify.side_effect = verify
            commands.side_effect = command
            runtime.side_effect = launch
            self.driver.cmd_smoke(self.args("scripting", variant="dist", delivery_profile="embedded"))
            configure, build = [call.args[0] for call in commands.call_args_list]
            self.assertEqual(configure[configure.index("-S") + 1], str(ROOT / "tests/sdk_smoke_scripting"))
            self.assertIn("-DJCE_DIR=" + str(self.cmake), configure)
            self.assertIn("-DJCE_BUILD_VARIANT=dist", configure)
            self.assertIn("-DJCE_DELIVERY_PROFILE=embedded", configure)
            self.assertIn("-DJCE_SMOKE_SCRIPT_BACKENDS=c;cpp", configure)
            self.assertIn("-DJCE_SMOKE_CAN_RUN_TARGET=ON", configure)
            unsets = {configure[index + 1] for index, argument in enumerate(configure[:-1]) if argument == "-U"}
            self.assertTrue({"JCE_PROJECT_PREBUILT_ASSETS_*", "JCE_PROJECT_PREBUILT_PAK_KEY_C",
                             "JCE_PROJECT_STAGE_LOOSE_ASSETS"} <= unsets)
            for flag in flags:
                self.assertIn(flag, configure)
            self.assertEqual(build, ["cmake", "--build", str(self.bdir), "--parallel", "2"])
            owner.prepare.assert_called_once()
            owner.verify.assert_called_once_with(self.bdir, fixture="scripting")
            self.assertEqual(runtime.call_count, 1)
            self.assertEqual(Path(runtime.call_args.args[0][0]).stem, "jce_sdk_smoke_scripting")
            self.assertEqual(runtime.call_args.kwargs["env"]["JCE_BACKEND"], "noop")
        self.assertEqual(events, ["scripting-admission", "configure", "build", "scripting-proof", "launch"])
        self.assertEqual(self.lease_resources, [[self.bdir, self.sdk]])

    def test_scripting_dist_sdk_mismatch_blocks_asset_admission_and_launch(self):
        self.scripting_dist_inputs()
        valid = dict(self.data)
        wrong_arch = "aarch64" if self.target["arch"] != "aarch64" else "x86_64"
        for key, wrong in (("variant", "release"), ("delivery", "authoring"), ("arch", wrong_arch)):
            with self.subTest(key=key):
                self.data = {**valid, key: wrong}
                self.write_metadata()
                with self.ports() as (commands, runtime), \
                     patch.object(self.driver, "_sdk_smoke_assets_module") as load_assets:
                    with self.assertRaises(SystemExit) as failed:
                        self.driver.cmd_smoke(self.args("scripting", variant="dist", delivery_profile="embedded"))
                    self.assertNotIn(failed.exception.code, (0, 77))
                    load_assets.assert_not_called()
                    commands.assert_not_called()
                    runtime.assert_not_called()
        self.data = valid
        self.write_metadata()
        self.assertFalse(self.bdir.exists())

    def test_scripting_dist_admission_failure_prevents_configure_and_launch(self):
        self.scripting_dist_inputs()
        for error in (ValueError("selected scripting fixture/source mismatch"),
                      OSError("selected SDK Bundle tool unavailable")):
            with self.subTest(error=error), self.ports() as (commands, runtime), \
                 patch.object(self.driver, "_sdk_smoke_assets_module") as load_assets, \
                 redirect_stderr(io.StringIO()) as diagnostic:
                owner = load_assets.return_value
                owner.prepare.side_effect = error
                with self.assertRaises(SystemExit) as failed:
                    self.driver.cmd_smoke(self.args("scripting", variant="dist", delivery_profile="embedded"))
                self.assertNotIn(failed.exception.code, (0, 77))
                self.assertEqual(owner.prepare.call_args.kwargs, {"fixture": "scripting"})
                self.assertEqual(owner.prepare.call_args.args[2:4], (self.sdk, self.bdir))
                self.assertIn("smoke-scripting: authenticated DIST asset admission failed", diagnostic.getvalue())
                owner.verify.assert_not_called()
                commands.assert_not_called()
                runtime.assert_not_called()
        self.assertFalse(self.bdir.exists())

    def test_scripting_dist_changed_proof_after_link_never_launches_stale_exe(self):
        self.scripting_dist_inputs()
        self.artifacts(scripting=True)  # A stale executable must not bypass failed proof.
        for error in (ValueError("scripting authenticated BOM changed"), OSError("script proof missing")):
            with self.subTest(error=error), self.ports() as (commands, runtime), \
                 patch.object(self.driver, "_sdk_smoke_assets_module") as load_assets, \
                 redirect_stderr(io.StringIO()) as diagnostic:
                owner = load_assets.return_value
                owner.prepare.return_value = []

                def reject_proof(directory, *, fixture):
                    self.assertEqual(directory, self.bdir)
                    self.assertEqual(fixture, "scripting")
                    self.assertEqual(commands.call_count, 2)
                    self.assertEqual(commands.call_args_list[-1].args[0][1], "--build")
                    raise error

                owner.verify.side_effect = reject_proof
                with self.assertRaises(SystemExit) as failed:
                    self.driver.cmd_smoke(self.args("scripting", variant="dist", delivery_profile="embedded"))
                self.assertNotIn(failed.exception.code, (0, 77))
                self.assertEqual(owner.prepare.call_args.kwargs, {"fixture": "scripting"})
                owner.verify.assert_called_once_with(self.bdir, fixture="scripting")
                self.assertIn("smoke-scripting: authenticated DIST asset proof failed", diagnostic.getvalue())
                runtime.assert_not_called()

    def test_scripting_dist_failed_configure_or_build_never_verifies_or_launches(self):
        self.scripting_dist_inputs()
        self.artifacts(scripting=True)
        for phase in ("configure", "build"):
            with self.subTest(phase=phase), self.ports() as (commands, runtime), \
                 patch.object(self.driver, "_sdk_smoke_assets_module") as load_assets:
                owner = load_assets.return_value
                owner.prepare.return_value = []

                def failed_command(arguments, **_):
                    current = "build" if arguments[1] == "--build" else "configure"
                    if current == phase:
                        raise SystemExit(9)  # Counted compiler failure, never an actual build.

                commands.side_effect = failed_command
                with self.assertRaises(SystemExit) as failed:
                    self.driver.cmd_smoke(self.args("scripting", variant="dist", delivery_profile="embedded"))
                self.assertEqual(failed.exception.code, 9)
                self.assertEqual(owner.prepare.call_args.kwargs, {"fixture": "scripting"})
                self.assertEqual(commands.call_count, 1 if phase == "configure" else 2)
                owner.verify.assert_not_called()
                runtime.assert_not_called()

    def test_native_marker_is_complete_unique_and_exit_success_required(self):
        self.artifacts()
        controls = (("JCE_SMOKE: OK version=port pak_assets=1\n", 0, True),
                    ("prefix JCE_SMOKE: OK version=port\n", 0, False),
                    ("JCE_SMOKE: OKAY\n", 0, False), ("", 0, False),
                    ("JCE_SMOKE: OK\nJCE_SMOKE: OK\n", 0, False),
                    ("JCE_SMOKE: OK\nJCE_SMOKE: FAIL resource\n", 0, False),
                    ("JCE_SMOKE: OK version=port\n", 1, False))
        for stdout, status, accepted in controls:
            with self.subTest(stdout=stdout, status=status), self.ports(stdout, status):
                if accepted:
                    self.driver.cmd_smoke(self.args())
                else:
                    with self.assertRaises(SystemExit) as failed:
                        self.driver.cmd_smoke(self.args())
                    self.assertNotIn(failed.exception.code, (0, 77))

    def test_cross_requires_actual_run_and_never_builds_or_passes(self):
        target = dict(self.target, cross=True)
        with patch.object(self.driver, "resolve_target", return_value=target), self.ports() as (commands, runtime):
            with self.assertRaises(SystemExit) as skipped:
                self.driver.cmd_smoke(self.args())
            self.assertEqual(skipped.exception.code, 77)
            commands.assert_not_called()
            runtime.assert_not_called()
        self.assertFalse(self.bdir.exists())

    def test_no_scripting_fragment_skips_but_declared_incomplete_payload_fails(self):
        with self.ports() as (commands, runtime):
            with self.assertRaises(SystemExit) as skipped:
                self.driver.cmd_smoke(self.args("scripting"))
            self.assertEqual(skipped.exception.code, 77)
            commands.assert_not_called()
            runtime.assert_not_called()
        self.availability()
        self.roster("c")
        self.roster("cpp", complete=False)
        with self.ports() as (commands, runtime):
            with self.assertRaises(SystemExit) as failed:
                self.driver.cmd_smoke(self.args("scripting"))
            self.assertNotIn(failed.exception.code, (0, 77))
            commands.assert_not_called()
            runtime.assert_not_called()
        self.roster("cpp", text="set(UNREADABLE_ROSTER ON)\n", complete=False)
        with self.ports() as (commands, runtime):
            with self.assertRaises(SystemExit): self.driver.cmd_smoke(self.args("scripting"))
            commands.assert_not_called()
            runtime.assert_not_called()

    def test_scripting_actual_owner_requires_complete_installed_roster_and_run(self):
        self.availability()
        self.roster("c")
        self.roster("cpp")
        self.artifacts(scripting=True)
        stdout = "JCE_SMOKE_SCRIPTING: c OK (port)\nJCE_SMOKE_SCRIPTING: cpp OK (port)\nJCE_SMOKE_SCRIPTING: OK version=port languages=3\n"
        with self.ports(stdout) as (commands, runtime):
            self.driver.cmd_smoke(self.args("scripting"))
            configure = commands.call_args_list[0].args[0]
            self.assertEqual(configure[configure.index("-S") + 1], str(ROOT / "tests/sdk_smoke_scripting"))
            self.assertIn("-DJCE_SMOKE_SCRIPT_BACKENDS=c;cpp", configure)
            self.assertIn("-DJCE_SMOKE_CAN_RUN_TARGET=ON", configure)
            self.assertEqual(commands.call_count, 2)
            self.assertEqual(runtime.call_count, 1)
        for stdout, status in (("prefix JCE_SMOKE_SCRIPTING: OK\n", 0),
                               ("JCE_SMOKE_SCRIPTING: OK\n", 5)):
            with self.subTest(stdout=stdout, status=status), self.ports(stdout, status):
                with self.assertRaises(SystemExit): self.driver.cmd_smoke(self.args("scripting"))

    def test_explicit_uncompiled_optional_catalog_is_unavailable_not_missing_payload(self):
        self.availability(("c", "cpp", "csharp", "java", "js", "python"), ("c", "cpp", "js"))
        for language in ("c", "cpp", "js"):
            self.roster(language)
        for language in ("csharp", "java", "python"):
            self.roster(language, complete=False)
        self.artifacts(scripting=True)
        stdout = "JCE_SMOKE_SCRIPTING: OK version=port languages=4\n"
        with patch.object(self.driver, "log") as messages, self.ports(stdout) as (commands, runtime):
            self.driver.cmd_smoke(self.args("scripting"))
            configure = commands.call_args_list[0].args[0]
            self.assertIn("-DJCE_SMOKE_SCRIPT_BACKENDS=c;cpp;js", configure)
            self.assertEqual(commands.call_count, 2)
            self.assertEqual(runtime.call_count, 1)
            self.assertTrue(any("UNAVAILABLE (not compiled in this SDK): csharp, java, python" in call.args[0]
                                for call in messages.call_args_list))

    def test_compiled_cpp_loss_fails_and_debug_sibling_cannot_back_release(self):
        self.availability()
        self.roster("c")
        self.roster("cpp", complete=False)
        debug = self.sdk / "lib/Debug"
        debug.mkdir()
        (debug / "jce_script_vm_cpp.lib").write_bytes(b"SIBLING_NOT_SELECTED")
        with self.ports() as (commands, runtime), redirect_stderr(io.StringIO()) as diagnostic:
            with self.assertRaises(SystemExit) as failed:
                self.driver.cmd_smoke(self.args("scripting"))
            self.assertEqual(failed.exception.code, 1)
            self.assertIn("missing=cpp", diagnostic.getvalue())
            commands.assert_not_called()
            runtime.assert_not_called()

    def test_availability_absent_malformed_duplicate_or_outside_catalog_fails_refresh(self):
        valid = self.availability()
        self.roster("c")
        self.roster("cpp")
        controls = ("set(JCE_SCRIPT_API_STATIC ON)\n",
                    valid.replace("AVAILABILITY_SCHEMA 1", "AVAILABILITY_SCHEMA 2"),
                    valid.replace('COMPILED_LANGUAGES "c;cpp"', 'COMPILED_LANGUAGES "c;c"'),
                    valid.replace('COMPILED_LANGUAGES "c;cpp"', 'COMPILED_LANGUAGES "c;CPP"'),
                    valid.replace('COMPILED_LANGUAGES "c;cpp"', 'COMPILED_LANGUAGES "c;missing"'),
                    valid.replace('CATALOG_LANGUAGES "c;cpp"', 'CATALOG_LANGUAGES "c;c;cpp"'),
                    valid + 'set(JCE_SCRIPT_COMPILED_LANGUAGES "c;cpp")\n')
        for text in controls:
            with self.subTest(text=text), self.ports() as (commands, runtime), redirect_stderr(io.StringIO()) as diagnostic:
                (self.cmake / "JCEScripting.cmake").write_text(text, encoding="utf-8")
                with self.assertRaises(SystemExit) as failed:
                    self.driver.cmd_smoke(self.args("scripting"))
                self.assertEqual(failed.exception.code, 1)
                self.assertIn("refresh the SDK", diagnostic.getvalue())
                commands.assert_not_called()
                runtime.assert_not_called()

    def test_uncompiled_catalog_rosters_are_validated_and_missing_or_duplicate_fails(self):
        self.availability(("c", "cpp", "python"), ("c", "cpp"))
        self.roster("c")
        self.roster("cpp")
        self.roster("python", complete=False)
        roster = self.cmake / "backends/python/jce_backend.cmake"
        valid = roster.read_text(encoding="utf-8")
        controls = (None, "set(UNREADABLE_ROSTER ON)\n",
                    valid + 'set(JCE_BACKEND_LANGUAGE "python")\n',
                    valid.replace('LANGUAGE "python"', 'LANGUAGE "cpp"'))
        for text in controls:
            with self.subTest(text=text), self.ports() as (commands, runtime):
                if text is None:
                    roster.unlink()
                else:
                    roster.write_text(text, encoding="utf-8")
                with self.assertRaises(SystemExit) as failed:
                    self.driver.cmd_smoke(self.args("scripting"))
                self.assertNotIn(failed.exception.code, (0, 77))
                commands.assert_not_called()
                runtime.assert_not_called()
                roster.write_text(valid, encoding="utf-8")

    def test_uncompiled_complete_release_payload_is_not_admitted_as_compiled(self):
        self.availability(("c", "cpp", "python"), ("c", "cpp"))
        self.roster("c")
        self.roster("cpp")
        self.roster("python")
        with self.ports() as (commands, runtime), redirect_stderr(io.StringIO()) as diagnostic:
            with self.assertRaises(SystemExit) as failed:
                self.driver.cmd_smoke(self.args("scripting"))
            self.assertEqual(failed.exception.code, 1)
            self.assertIn("unexpected=python", diagnostic.getvalue())
            commands.assert_not_called()
            runtime.assert_not_called()

    def test_scratch_admission_and_dry_run_cannot_fake_a_native_pass(self):
        for directory in (ROOT, ROOT.parent, ROOT / "tests/sdk_smoke", self.sdk,
                          self.sdk / "scratch", self.root):
            with self.subTest(directory=directory), patch.object(self.driver, "run") as commands:
                with self.assertRaises(SystemExit):
                    self.driver.cmd_smoke(self.args(build_dir=directory))
                commands.assert_not_called()
        with patch.object(self.driver, "DRY_RUN", True), patch.object(self.driver, "run") as commands:
            with self.assertRaises(SystemExit) as failed:
                self.driver.cmd_smoke(self.args())
            self.assertNotIn(failed.exception.code, (0, 77))
            commands.assert_not_called()


class ScriptingRuntimeStageTests(unittest.TestCase):
    """Real compiler-free CMake TYPE/roster decisions; copy registration is a port.

    This is not a compiler, linker, post-build copy or SDK runtime certificate.
    The installed-SDK native smoke gates retain those separate responsibilities.
    """

    @staticmethod
    def _owner():
        source = (ROOT / "scripting/cmake/JCEScripting.cmake.in").read_text(encoding="utf-8")
        begin = source.index("function(jce_script_stage_runtime TARGET_NAME)")
        end = source.index("endfunction()", begin) + len("endfunction()")
        return source[begin:end]

    @staticmethod
    def _roster(language, targets="", files=""):
        return (f'set(JCE_BACKEND_LANGUAGE "{language}")\n'
                f'set(JCE_BACKEND_STAGE_TARGETS "{targets}")\n'
                f'set(JCE_BACKEND_STAGE_FILES "{files}")\n')

    def _configure(self, name, *, compiled=(), rosters=None, targets="",
                   metadata="valid", mutation=None):
        cmake = shutil.which("cmake")
        ninja = shutil.which("ninja")
        if not cmake or not ninja:
            self.skipTest("CMake/Ninja unavailable for compiler-free scripting staging control")
        rosters = rosters or {}
        scratch = ROOT / "build/tests/scripting-runtime-stage"
        scratch.mkdir(parents=True, exist_ok=True)
        root = Path(tempfile.mkdtemp(prefix=name + "-", dir=scratch))
        for language, text in rosters.items():
            destination = root / "backends" / language / "jce_backend.cmake"
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(text, encoding="utf-8", newline="\n")
        (root / "present-runtime.bin").write_bytes(b"CPU_CONFIGURE_FILE_PRESENCE_ONLY")
        (root / "runtime-directory").mkdir()
        authority = ""
        if metadata is not None:
            catalog = sorted(set(rosters) | set(compiled))
            values = dict(schema="1", catalog=";".join(catalog), compiled=";".join(compiled))
            if isinstance(metadata, dict):
                values.update(metadata)
            authority = (f'set(JCE_SCRIPT_AVAILABILITY_SCHEMA "{values["schema"]}")\n'
                         f'set(JCE_SCRIPT_CATALOG_LANGUAGES "{values["catalog"]}")\n'
                         f'set(JCE_SCRIPT_COMPILED_LANGUAGES "{values["compiled"]}")\n')
        owner = self._owner()
        if mutation:
            before, after = mutation
            self.assertEqual(owner.count(before), 1, "mutation must consume one actual owner wire")
            owner = owner.replace(before, after, 1)
        recipe = '''cmake_minimum_required(VERSION 3.20)
project(ScriptingStageControl LANGUAGES NONE)
set(JCE_SCRIPT_API_STATIC ON)
set(_jce_s_cmake "${CMAKE_SOURCE_DIR}")
add_custom_target(app)
set_property(GLOBAL PROPERTY JCE_STAGE_COPY_REGISTRATIONS 0)
# Keep actual target TYPE and filesystem queries. No compiler or copy runs;
# only the public CMake post-build registration operation is a counted port.
function(add_custom_command)
  get_property(count GLOBAL PROPERTY JCE_STAGE_COPY_REGISTRATIONS)
  math(EXPR count "${count}+1")
  set_property(GLOBAL PROPERTY JCE_STAGE_COPY_REGISTRATIONS "${count}")
endfunction()
'''
        recipe += authority + targets + "\n" + owner + '''
jce_script_stage_runtime(app)
get_property(count GLOBAL PROPERTY JCE_STAGE_COPY_REGISTRATIONS)
message(STATUS "JCE_STAGE_COPY_REGISTRATIONS=${count}")
'''
        (root / "CMakeLists.txt").write_text(recipe, encoding="utf-8", newline="\n")
        command = [cmake, "-S", str(root), "-B", str(root / "out"), "-G", "Ninja",
                   "-DCMAKE_MAKE_PROGRAM=" + str(Path(ninja).resolve())]
        result = subprocess.run(command, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=30)
        (root / "configure.stdout.log").write_text(result.stdout, encoding="utf-8", newline="\n")
        (root / "configure.stderr.log").write_text(result.stderr, encoding="utf-8", newline="\n")
        (root / "configure-command.json").write_text(json.dumps(command, ensure_ascii=False),
                                                    encoding="utf-8", newline="\n")
        self.assertEqual(result.returncode, 0,
                         f"configure failed; preserved raw logs: {root}\n{result.stdout}{result.stderr}")
        return result.stdout, result.stderr, root

    def _assert_static(self, result):
        stdout, stderr, root = result
        self.assertIn("static SDK roster declares no external script runtime to stage.", stdout,
                      f"raw logs: {root}")
        self.assertNotIn("CMake Warning", stderr, f"raw logs: {root}\n{stderr}")
        self.assertIn("JCE_STAGE_COPY_REGISTRATIONS=0", stdout)

    def test_static_empty_roster_is_status(self):
        self._assert_static(self._configure("empty-static"))

    def test_resolved_static_interface_and_alias_targets_are_status(self):
        roster = self._roster("c", "static_runtime;static_alias;interface_runtime")
        targets = '''add_library(static_runtime STATIC IMPORTED GLOBAL)
set_target_properties(static_runtime PROPERTIES IMPORTED_LOCATION
  "${CMAKE_SOURCE_DIR}/present-runtime.bin")
add_library(static_alias ALIAS static_runtime)
add_library(interface_runtime INTERFACE IMPORTED GLOBAL)
'''
        self._assert_static(self._configure("resolved-static", compiled=("c",),
                                           rosters={"c": roster}, targets=targets))

    def test_actual_catalog_inactive_optional_backends_are_not_obligations(self):
        rosters = {language: (ROOT / "scripting" / language / "jce_backend.cmake").read_text(
            encoding="utf-8") for language in ("c", "cpp", "csharp", "java", "js", "python")}
        self._assert_static(self._configure("actual-inactive-catalog",
                                           compiled=("c", "cpp", "js"), rosters=rosters))

    def test_active_missing_target_retains_warning(self):
        stdout, stderr, root = self._configure(
            "missing-target", compiled=("python",),
            rosters={"python": self._roster("python", "Python3::Python")})
        self.assertIn("CMake Warning", stderr, f"raw logs: {root}")
        self.assertIn("missing target Python3::Python", stderr)
        self.assertNotIn("declares no external script runtime", stdout)
        self.assertIn("JCE_STAGE_COPY_REGISTRATIONS=0", stdout)

    def test_missing_file_or_directory_retains_warning(self):
        for leaf in ("missing-runtime.dll", "runtime-directory"):
            with self.subTest(leaf=leaf):
                stdout, stderr, root = self._configure(
                    "missing-file", compiled=("csharp",),
                    rosters={"csharp": self._roster(
                        "csharp", files="${CMAKE_SOURCE_DIR}/" + leaf)})
                self.assertIn("CMake Warning", stderr, f"raw logs: {root}")
                self.assertIn("missing runtime file", stderr)
                self.assertIn(leaf, stderr)
                self.assertNotIn("declares no external script runtime", stdout)
                self.assertIn("JCE_STAGE_COPY_REGISTRATIONS=0", stdout)

    def test_partial_external_registration_does_not_hide_missing_obligation(self):
        targets = '''add_library(native_runtime SHARED IMPORTED GLOBAL)
set_target_properties(native_runtime PROPERTIES IMPORTED_LOCATION
  "${CMAKE_SOURCE_DIR}/present-runtime.bin")
'''
        stdout, stderr, root = self._configure(
            "partial-runtime", compiled=("python",),
            rosters={"python": self._roster("python", "native_runtime;missing_runtime")},
            targets=targets)
        self.assertIn("CMake Warning", stderr, f"raw logs: {root}")
        self.assertIn("missing target missing_runtime", stderr)
        self.assertIn("JCE_STAGE_COPY_REGISTRATIONS=1", stdout)

    def test_nonstatic_target_without_file_cannot_be_satisfied(self):
        stdout, stderr, root = self._configure(
            "nonstatic-no-file", compiled=("c",), targets="add_custom_target(helper)\n",
            rosters={"c": self._roster("c", "helper")})
        self.assertIn("CMake Warning", stderr, f"raw logs: {root}")
        self.assertIn("has no stageable file (UTILITY)", stderr)
        self.assertIn("JCE_STAGE_COPY_REGISTRATIONS=0", stdout)

    def test_missing_or_malformed_compiled_authority_cannot_become_empty_status(self):
        controls = (None, {"schema": "2"}, {"compiled": "c;c"}, {"compiled": "unknown"})
        for metadata in controls:
            with self.subTest(metadata=metadata):
                stdout, stderr, root = self._configure(
                    "bad-authority", compiled=("c",),
                    rosters={"c": self._roster("c")}, metadata=metadata)
                self.assertIn("CMake Warning", stderr, f"raw logs: {root}")
                self.assertIn("compiled roster authority is missing or malformed", stderr)
                self.assertNotIn("declares no external script runtime", stdout)

    def test_missing_compiled_roster_retains_warning(self):
        stdout, stderr, root = self._configure("missing-compiled-roster", compiled=("python",))
        self.assertIn("CMake Warning", stderr, f"raw logs: {root}")
        self.assertIn("missing compiled roster python", stderr)
        self.assertNotIn("declares no external script runtime", stdout)

    def test_restored_old_empty_list_guard_reproduces_false_warning(self):
        result = self._configure(
            "old-empty-guard", compiled=("c",),
            rosters={"c": self._roster("c", "static_runtime")},
            targets="add_library(static_runtime STATIC IMPORTED GLOBAL)\n",
            mutation=("JCE_SCRIPT_API_STATIC AND _static_only AND NOT _stage_files",
                      "JCE_SCRIPT_API_STATIC AND NOT _stage AND NOT _stage_files"))
        stdout, stderr, root = result
        self.assertIn("CMake Warning", stderr, f"raw logs: {root}")
        self.assertIn("call is a no-op rather than a guarantee", stderr)
        self.assertNotIn("declares no external script runtime", stdout)
        self.assertIn("JCE_STAGE_COPY_REGISTRATIONS=0", stdout)

    def test_removed_actual_compiled_filter_reproduces_inactive_missing_warning(self):
        before = '''\t\t\tif(NOT JCE_BACKEND_LANGUAGE IN_LIST _compiled)
\t\t\t\tcontinue()
\t\t\tendif()'''
        stdout, stderr, root = self._configure(
            "removed-compiled-filter", compiled=("c",),
            rosters={"c": self._roster("c"),
                     "python": self._roster("python", "Python3::Python")},
            mutation=(before, "\t\t\tif(FALSE)\n\t\t\t\tcontinue()\n\t\t\tendif()"))
        self.assertIn("CMake Warning", stderr, f"raw logs: {root}")
        self.assertIn("missing target Python3::Python", stderr)
        self.assertNotIn("declares no external script runtime", stdout)


if __name__ == "__main__":
    unittest.main()
