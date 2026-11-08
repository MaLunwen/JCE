"""Actual SDK producer dispatch with counted ports, not a native SDK certificate.

Temporary archives are presence-only metadata fixtures. No compiler, Conan,
native application, renderer or GPU is executed by these controls.
"""
import contextlib
import importlib.util
import io
import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def load(name, path, source=None):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    if source is None:
        spec.loader.exec_module(module)
    else:
        exec(compile(source, str(path), "exec"), module.__dict__)
    return module


OPTIONS = load("sdk_outputs_controls", ROOT / "tools/build/sdk_outputs.py")
STANDALONE = load("sdk_standalone_controls", ROOT / "tools/build/standalone.py")


class Fixture:
    def __init__(self):
        base = ROOT / "build/tests/sdk-output-policy"
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=base)
        self.root = Path(self.temporary.name).resolve()
        self.driver = load("sdk_outputs_driver_controls", ROOT / "tools/build/jce.py")
        self.driver.ROOT = self.root
        self.driver.DRY_RUN = False
        version = re.search(r"project\(JCE VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C\)",
            (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")).group(1)
        (self.root / "CMakeLists.txt").write_text("project(JCE VERSION " + version + " LANGUAGES C)\n", encoding="utf-8")
        self.driver.HOST = "windows"
        self.target = dict(self.driver.TARGETS["windows-x64"], key="windows-x64", cross=False)
        self.args = SimpleNamespace(arch="x64", variant="dist", clean=False, smoke=False,
            delivery_profile="embedded", standalone=False, script_java=False, profiling=None,
            staging_dir=None, build_dir=self.root / "build/fixtures/isolated", install_dir=self.root / "dist/sdk/isolated",
            embed_engine_shaders="off")
        self.commands = []
        self.plan = OPTIONS.resolve(self.driver, self.args, self.target)

    def stage(self):
        self.args.build_dir = self.args.install_dir = None
        self.args.staging_dir = self.root / "build/sdk/windows-x64-dist-embedded"
        self.args.embed_engine_shaders = "auto"
        self.plan = OPTIONS.resolve(self.driver, self.args, self.target)
        return self.plan

    def close(self):
        self.temporary.cleanup()

    def cache(self, plan=None, **changes):
        plan = plan or self.plan
        values = dict(CMAKE_HOME_DIRECTORY=str(plan["source"]), CMAKE_CACHEFILE_DIR=str(plan["build"]),
            CMAKE_INSTALL_PREFIX=str(plan["install"]), CMAKE_TOOLCHAIN_FILE=str(plan["toolchain"]),
            CMAKE_BUILD_TYPE="Release", JCE_BUILD_VARIANT=plan["variant"],
            JCE_DELIVERY_PROFILE="embedded", JCE_EDITOR_STANDALONE="ON",
            JCE_ENABLE_PROFILING="OFF", JCE_BUILD_SCRIPT_VM_PYTHON="OFF",
            JCE_BUILD_SCRIPT_JAVA="OFF", JCE_BUILD_SCRIPT_CSHARP="OFF", JCE_BUILD_EDITOR="ON",
            JCE_ENABLE_SDK_INSTALL="ON", JCE_EMBED_ENGINE_SHADERS=plan["shaders"])
        values.update(changes)
        plan["build"].mkdir(parents=True, exist_ok=True)
        (plan["build"] / "CMakeCache.txt").write_text("\n".join(
            key + ":STRING=" + value for key, value in values.items()) + "\n", encoding="utf-8")
        plan["toolchain"].parent.mkdir(parents=True, exist_ok=True)
        plan["toolchain"].write_text('set(CMAKE_MSVC_RUNTIME_LIBRARY "$<$<CONFIG:Release>:MultiThreaded>")\n')
        compiler = plan["build"] / "CMakeFiles/3.27.9/CMakeCCompiler.cmake"
        compiler.parent.mkdir(parents=True, exist_ok=True)
        compiler.write_text('set(CMAKE_C_COMPILER_ID "MSVC")\nset(CMAKE_C_COMPILER_ARCHITECTURE_ID x64)\n')

    def installed(self, **changes):
        prefix = self.plan["install"]
        library = prefix / "lib/Release"
        library.mkdir(parents=True, exist_ok=True)
        data = dict(schema=1, system="Windows", arch="x86_64", pointer_bits=64,
            config="Release", variant="dist", delivery="embedded", crt="MultiThreaded",
            archive="jce_engine_core.lib")
        data.update(changes)
        (library / "jce-sdk-target.json").write_text(json.dumps(data), encoding="utf-8")
        (library / "jce_engine_core.lib").write_bytes(b"presence-only archive; never linked")
        # Real minimal installed identity closure; archives remain unlinked fixtures.
        version = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        selected = re.search(r"project\(JCE VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C\)", version).group(1)
        (self.root / "CMakeLists.txt").write_text("project(JCE VERSION " + selected + " LANGUAGES C)\n", encoding="utf-8")
        major, minor, patch = selected.split(".")
        files = {"include/jce/api.h": b"uncompiled public-header fixture\n",
            "include/jce/jce_version.h": (f"#define JCE_VERSION_MAJOR {major}\n#define JCE_VERSION_MINOR {minor}\n"
                f"#define JCE_VERSION_PATCH {patch}\n#define JCE_VERSION_STR \"{selected}\"\n"
                "#define JCE_API_VERSION ((JCE_VERSION_MAJOR << 24) | (JCE_VERSION_MINOR << 16) | (JCE_VERSION_PATCH << 8))\n").encode()}
        for name in ("JCEConfig.cmake", "JCESDKTarget.cmake", "JCESDKHelpers.cmake"):
            files["lib/cmake/JCE/" + name] = b"unexecuted CMake fixture\n"
        for name in ("jce_pak", "jce_cook", "jce_bin2obj", "jce_bundle_pack"):
            files["bin/" + name + ".exe"] = b"unexecuted host-tool fixture\n"
        for relative, content in files.items():
            path = prefix / relative; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(content)
        self.plan["build"].mkdir(parents=True, exist_ok=True)
        roster = [str(p.resolve()) for p in prefix.rglob("*") if p.is_file() and p.name != "jce-sdk-product.json"]
        (self.plan["build"] / "install_manifest.txt").write_text("\n".join(sorted(roster)) + "\n", encoding="utf-8")
        if getattr(self, "install_output_options", True):
            OPTIONS._atomic_json(prefix / OPTIONS.OPTIONS_FILE, {"schema": 1, "request": self.plan["request"]})

    def capture(self, command, **unused):
        self.commands.append(command)
        if command[0] == "cmake" and "-S" in command:
            assert Path(command[command.index("-B") + 1]) == self.plan["build"], "custom cache was ignored"
            assert "-DCMAKE_INSTALL_PREFIX=" + str(self.plan["install"]) in command, "custom prefix was ignored"
            assert "-DJCE_EMBED_ENGINE_SHADERS=" + self.plan["shaders"] in command, "shader selection wire is missing"
            self.cache()
            if self.plan["shaders"] == "ON":
                (self.plan["build"] / "generated/engine_assets/shaders").mkdir(parents=True, exist_ok=True)
        elif command[0] == "cmake" and "--build" in command:
            assert Path(command[2]) == self.plan["build"], "build target used another cache"
        elif command[0] == "cmake" and "--install" in command:
            assert Path(command[2]) == self.plan["build"], "install used another cache"
            self.installed()
            installed = [path.resolve() for path in self.plan["install"].rglob("*") if path.is_file()]
            (self.plan["build"] / "install_manifest.txt").write_text(
                "\n".join(str(path) for path in installed) + "\n", encoding="utf-8")

    @contextlib.contextmanager
    def producer_ports(self, standalone=STANDALONE, run=None):
        driver = self.driver
        with patch.object(driver, "_standalone_module", return_value=standalone), \
             patch.object(driver, "resolve_target", return_value=self.target), \
             patch.object(driver, "msvc_env", return_value={"CMAKE_BUILD_PARALLEL_LEVEL": "2"}) as environment, \
             patch.object(driver, "sync_conan_hooks") as hooks, \
             patch.object(driver, "export_conan_recipes") as recipes, \
             patch.object(driver, "find_vs_install", return_value=None), \
             patch.object(driver, "host_build_profile", return_value="windows-x64"), \
             patch.object(driver, "bgfx_graphics_conan_args", return_value=[]), \
             patch.object(driver, "git_short_sha", return_value="presence-fixture"), \
             patch.object(driver, "sdk_shipped_script_backends", return_value=["c", "cpp", "js"]), \
             patch.object(driver, "run", side_effect=run or self.capture) as commands, \
             patch.object(driver, "_cache_scope", return_value=contextlib.nullcontext()) as lease, \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            yield SimpleNamespace(environment=environment, hooks=hooks, recipes=recipes,
                                  commands=commands, lease=lease)


class SdkOutputControls(unittest.TestCase):
    def test_isolated_request_does_not_admit_unrelated_default_editor_objects(self):
        fixture = self.fixture()
        default = fixture.root / "build/desktop/windows-x64/obj"
        default.mkdir(parents=True)
        cache = default / "CMakeCache.txt"
        cache.write_bytes(b"JCE_DELIVERY_PROFILE:STRING=authoring\n")
        with fixture.producer_ports():
            fixture.driver.cmd_sdk(fixture.args)
        self.assertEqual(cache.read_bytes(), b"JCE_DELIVERY_PROFILE:STRING=authoring\n")
        self.assertEqual(fixture.plan["build"], fixture.args.build_dir)

    def fixture(self):
        result = Fixture()
        self.addCleanup(result.close)
        return result

    def test_defaults_are_shipping_on_and_keep_paths(self):
        fixture = self.fixture()
        fixture.args.build_dir = fixture.args.install_dir = None
        for selection in ("auto", "on"):
            fixture.args.embed_engine_shaders = selection
            plan = OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)
            self.assertFalse(plan["custom"])
            self.assertEqual(plan["shaders"], "ON")
            self.assertEqual(plan["build"], fixture.root / "build/desktop/windows-x64/obj")
            self.assertEqual(plan["conan"], fixture.root / "build/desktop/windows-x64/conan")
            self.assertEqual(plan["install"], fixture.root / "dist/sdk/win32-x86_64-dist-single")
        fixture.args.embed_engine_shaders = "off"
        with self.assertRaisesRegex(ValueError, "requires isolated"):
            OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)

    def test_parser_alias_exposes_the_same_three_options(self):
        fixture = self.fixture()
        for prefix in (("sdk",), ("package", "sdk")):
            parsed = fixture.driver.build_parser().parse_args([*prefix,
                "--build-dir", str(fixture.args.build_dir), "--install-dir", str(fixture.args.install_dir),
                "--embed-engine-shaders", "off"])
            self.assertEqual(parsed.build_dir, fixture.args.build_dir)
            self.assertEqual(parsed.install_dir, fixture.args.install_dir)
            self.assertEqual(parsed.embed_engine_shaders, "off")
            self.assertEqual(parsed.func, fixture.driver.cmd_sdk)

    def test_unsupported_selection_is_refused_before_any_dispatch(self):
        fixture = self.fixture()
        for changes in ({"delivery_profile": "authoring"}, {"clean": True}, {"variant": "both"},
                        {"install_dir": None}, {"build_dir": None}, {"embed_engine_shaders": "bad"}):
            with self.subTest(changes=changes):
                args = SimpleNamespace(**vars(fixture.args))
                vars(args).update(changes)
                with fixture.producer_ports() as ports, self.assertRaises((SystemExit, ValueError)):
                    fixture.driver.cmd_sdk(args)
                self.assertFalse(ports.environment.called)
                self.assertFalse(ports.lease.called)
                self.assertFalse(ports.commands.called)
        for target in (dict(fixture.target, cross=True), dict(fixture.target, emscripten=True)):
            with self.assertRaisesRegex(ValueError, "native embedded"):
                OPTIONS.selection(fixture.args, target, "embedded")
        with self.assertRaisesRegex(ValueError, "matching native host"):
            OPTIONS.resolve(fixture.driver, fixture.args, dict(fixture.target, host="linux"))

    def test_path_roots_default_prefixes_and_shared_caches_are_protected(self):
        fixture = self.fixture()
        original = vars(fixture.args).copy()
        invalid = [dict(build_dir=fixture.root / "build"), dict(build_dir=fixture.root / "source"),
            dict(build_dir=fixture.root / "build/desktop/another"),
            dict(build_dir=fixture.root / "build/host/another"), dict(build_dir=fixture.root / "build/web/another"),
            dict(install_dir=fixture.root / "dist/sdk"), dict(install_dir=fixture.root / "dist/apps/output")]
        for target in fixture.driver.TARGETS.values():
            for variant in ("release", "dist"):
                prefix = fixture.driver.sdk_install_dir(target, variant)
                invalid.extend(dict(install_dir=path) for path in
                    (prefix, prefix.with_name(prefix.name + "-single"), prefix / "nested"))
        for changes in invalid:
            with self.subTest(changes=changes):
                fixture.args = SimpleNamespace(**original)
                vars(fixture.args).update(changes)
                with self.assertRaises(ValueError):
                    OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)
        self.assertFalse(fixture.plan["build"].exists())
        self.assertFalse(fixture.plan["install"].exists())

    def test_actual_shared_producer_uses_custom_cache_prefix_and_off(self):
        fixture = self.fixture()
        fixture.args.smoke = True
        with fixture.producer_ports(), patch.object(fixture.driver, "_smoke_one", return_value="passed") as c99, \
             patch.object(fixture.driver, "_smoke_scripting_one", return_value="passed") as script:
            fixture.driver.cmd_sdk(fixture.args)
        self.assertEqual(len(fixture.commands), 4)
        conan, configure, build, install = fixture.commands
        self.assertIn("compiler.runtime=static", conan)
        self.assertIn("*:shared=False", conan)
        self.assertEqual(conan[conan.index("--output-folder") + 1], str(fixture.plan["conan"]))
        self.assertIn("-DJCE_BUILD_EDITOR=OFF", configure)
        self.assertIn("jce_sdk_fat_lib", build)
        self.assertIn("jce_scripting_sdk_artifacts", build)
        self.assertEqual(build[-2:], ["-j", "2"])
        self.assertEqual(install[-2:], ["--config", "Release"])
        state = json.loads((fixture.plan["build"] / OPTIONS.REQUEST_FILE).read_text())
        self.assertEqual(state["state"], "installed")
        self.assertEqual(c99.call_args.kwargs["build_dir"], fixture.plan["build"] / "sdk-smoke/c99")
        self.assertEqual(script.call_args.kwargs["build_dir"], fixture.plan["build"] / "sdk-smoke/scripting")
        resources = OPTIONS.cache_paths(vars(fixture.driver), fixture.args, fixture.target)
        self.assertIn(fixture.plan["install"], resources)
        self.assertIn(fixture.plan["conan"], resources)
        self.assertIn(fixture.plan["build"] / "sdk-smoke/c99", resources)
        self.assertIn(fixture.plan["build"] / "sdk-smoke/scripting", resources)
        # A request stamp is not compiler metadata. This reader independently
        # admits the generated metadata presence fixture after the install port.
        OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)

    def test_default_auto_explicitly_resets_cached_shader_off(self):
        fixture = self.fixture()
        fixture.args.build_dir = fixture.args.install_dir = None
        fixture.args.embed_engine_shaders = "auto"
        fixture.driver.DRY_RUN = True
        with fixture.producer_ports(run=lambda command, **unused: fixture.commands.append(command)):
            fixture.driver.cmd_sdk(fixture.args)
        self.assertIn("-DJCE_EMBED_ENGINE_SHADERS=ON", fixture.commands[1])
        self.assertFalse((fixture.root / "build").exists())

    def test_preflight_refuses_cache_changes_before_lease_environment_and_hooks(self):
        fixture = self.fixture()
        OPTIONS.state(fixture.plan, "configured")
        for key, wrong in (("CMAKE_HOME_DIRECTORY", str(fixture.root / "other")),
            ("CMAKE_CACHEFILE_DIR", str(fixture.root / "another")),
            ("CMAKE_INSTALL_PREFIX", str(fixture.root / "dist/sdk/another")),
            ("CMAKE_TOOLCHAIN_FILE", str(fixture.root / "other.cmake")),
            ("JCE_BUILD_VARIANT", "release"), ("JCE_DELIVERY_PROFILE", "authoring"),
            ("CMAKE_BUILD_TYPE", "Debug"), ("JCE_EMBED_ENGINE_SHADERS", "ON")):
            with self.subTest(key=key):
                fixture.cache(**{key: wrong})
                before = (fixture.plan["build"] / OPTIONS.REQUEST_FILE).read_bytes()
                with fixture.producer_ports() as ports, self.assertRaises(ValueError):
                    fixture.driver.cmd_sdk(fixture.args)
                self.assertFalse(ports.lease.called)
                self.assertFalse(ports.environment.called)
                self.assertFalse(ports.hooks.called)
                self.assertFalse(ports.commands.called)
                self.assertEqual(before, (fixture.plan["build"] / OPTIONS.REQUEST_FILE).read_bytes())

    def test_first_conan_failure_and_complete_failed_configure_are_retryable(self):
        fixture = self.fixture()
        def fail_conan(command, **unused):
            raise RuntimeError("counted dependency failure")
        with fixture.producer_ports(run=fail_conan), self.assertRaisesRegex(RuntimeError, "dependency failure"):
            fixture.driver.cmd_sdk(fixture.args)
        stamp = fixture.plan["build"] / OPTIONS.REQUEST_FILE
        self.assertEqual(json.loads(stamp.read_text())["state"], "configure_failed")
        OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)
        def fail_configure(command, **unused):
            fixture.capture(command)
            if "-S" in command:
                raise RuntimeError("counted configure failure")
        with fixture.producer_ports(run=fail_configure), self.assertRaisesRegex(RuntimeError, "configure failure"):
            fixture.driver.cmd_sdk(fixture.args)
        OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)
        with fixture.producer_ports():
            fixture.driver.cmd_sdk(fixture.args)
        self.assertEqual(json.loads(stamp.read_text())["state"], "installed")
        self.assertEqual(list(stamp.parent.glob(stamp.name + ".*.tmp")), [])

    def test_changed_owner_and_unowned_existing_cache_fail_closed(self):
        fixture = self.fixture()
        fixture.plan["build"].mkdir(parents=True)
        (fixture.plan["build"] / "unowned.txt").write_text("unowned", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "without its owner"):
            OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)
        OPTIONS.state(fixture.plan, "configure_failed")
        for changes in ({"variant": "release"}, {"embed_engine_shaders": "on"},
                        {"install_dir": fixture.root / "dist/sdk/another"}):
            args = SimpleNamespace(**vars(fixture.args))
            vars(args).update(changes)
            with self.assertRaisesRegex(ValueError, "request identity"):
                OPTIONS.resolve(fixture.driver, args, fixture.target)
        with self.assertRaisesRegex(ValueError, "request identity"):
            OPTIONS.resolve(fixture.driver, fixture.args, dict(fixture.target, arch="i686"))
        fixture.cache(JCE_ENABLE_SDK_INSTALL="OFF")
        with self.assertRaisesRegex(ValueError, "cache identity"):
            OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)

    def test_actual_compiler_metadata_and_archive_presence_admit_reuse(self):
        fixture = self.fixture()
        OPTIONS.state(fixture.plan, "installed")
        fixture.cache()
        fixture.installed()
        OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)
        for changes in ({"arch": "i686", "pointer_bits": 32}, {"variant": "release"},
                        {"delivery": "authoring"}, {"crt": "MultiThreadedDLL"}):
            fixture.installed(**changes)
            with self.assertRaises(ValueError):
                OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)
        fixture.installed()
        (fixture.plan["install"] / "lib/Release/jce_engine_core.lib").unlink()
        with self.assertRaisesRegex(ValueError, "archive missing"):
            OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)

    def test_bounded_unique_stamps_and_atomic_failure_keep_prior_request(self):
        fixture = self.fixture()
        OPTIONS.state(fixture.plan, "configured")
        stamp = fixture.plan["build"] / OPTIONS.REQUEST_FILE
        original = stamp.read_bytes()
        with patch.object(OPTIONS.os, "replace", side_effect=OSError("counted rename refusal")), \
             self.assertRaisesRegex(OSError, "rename refusal"):
            OPTIONS.state(fixture.plan, "build_failed")
        self.assertEqual(stamp.read_bytes(), original)
        self.assertEqual(list(stamp.parent.glob(stamp.name + ".*.tmp")), [])
        with self.assertRaisesRegex(ValueError, "request exceeds"):
            OPTIONS._atomic_json(stamp, {"oversized": "x" * 16384})
        self.assertEqual(stamp.read_bytes(), original)
        for bad in (b" " * 16385, b'{"schema":1,"schema":1}', b'{"value":NaN}', b"[]"):
            stamp.write_bytes(bad)
            with self.assertRaises(ValueError):
                OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)

    def test_dry_run_uses_custom_outputs_without_writing_request(self):
        fixture = self.fixture()
        fixture.driver.DRY_RUN = True
        with fixture.producer_ports(run=lambda command, **unused: fixture.commands.append(command)):
            fixture.driver.cmd_sdk(fixture.args)
        self.assertEqual(Path(fixture.commands[1][fixture.commands[1].index("-B") + 1]), fixture.plan["build"])
        self.assertIn("-DJCE_EMBED_ENGINE_SHADERS=OFF", fixture.commands[1])
        self.assertFalse(fixture.plan["build"].exists())
        self.assertFalse(fixture.plan["install"].exists())

    def test_ordinary_production_wire_negative_controls(self):
        for control in ("old-output-paths", "missing-shader-wire", "missing-preflight-wire"):
            result = subprocess.run([sys.executable, "-B", str(Path(__file__).resolve()),
                                     "--negative-control", control], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
            self.assertIn("SDK_OUTPUT_CONTROL_REFUSED: " + control, result.stdout)


class SdkStagingControls(unittest.TestCase):
    def fixture(self):
        result = Fixture()
        self.addCleanup(result.close)
        return result

    def test_parser_aliases_share_explicit_staging_selection(self):
        fixture = self.fixture()
        for prefix in (("sdk",), ("package", "sdk")):
            try:
                parsed = fixture.driver.build_parser().parse_args([*prefix,
                    "--staging-dir", str(fixture.root / "build/sdk/current"),
                    "--arch", "x64", "--variant", "dist"])
            except SystemExit:
                self.fail("supported SDK parser rejected explicit staging")
            self.assertEqual(parsed.staging_dir, fixture.root / "build/sdk/current")
            self.assertEqual(parsed.func, fixture.driver.cmd_sdk)

    def test_staging_retains_canonical_tuple_and_shared_cache_compatibility(self):
        fixture = self.fixture()
        plan = fixture.stage()
        self.assertEqual(plan["install"], fixture.args.staging_dir)
        self.assertEqual(plan["build"], fixture.root / "build/desktop/windows-x64/obj")
        self.assertEqual(plan["conan"], fixture.root / "build/desktop/windows-x64/conan")
        self.assertFalse(plan["custom"])
        self.assertEqual(plan["shaders"], "ON")
        fixture.cache(CMAKE_INSTALL_PREFIX=str(fixture.root / "dist/sdk/prior"),
                      JCE_ENABLE_SDK_INSTALL="OFF")
        OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)
        for key, value in (("CMAKE_HOME_DIRECTORY", str(fixture.root / "other")),
            ("CMAKE_TOOLCHAIN_FILE", str(fixture.root / "wrong.cmake")),
            ("CMAKE_BUILD_TYPE", "Debug"), ("JCE_BUILD_VARIANT", "release"),
            ("JCE_DELIVERY_PROFILE", "authoring"), ("JCE_EDITOR_STANDALONE", "OFF"),
            ("JCE_EMBED_ENGINE_SHADERS", "OFF")):
            with self.subTest(key=key):
                fixture.cache(**{key: value})
                with self.assertRaisesRegex(ValueError, "cache identity"):
                    OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)

    def test_actual_producer_stages_without_touching_default_prefix_or_cache_stamp(self):
        fixture = self.fixture()
        fixture.stage()
        self.assertEqual(fixture.plan["install"], fixture.args.staging_dir)
        fixture.install_output_options = False  # verifier owns this metadata
        default = fixture.root / "dist/sdk/win32-x86_64-dist-single"
        default.mkdir(parents=True)
        sentinel = default / "untouched"
        sentinel.write_bytes(b"original default SDK")
        with patch.object(STANDALONE, "sdk_outputs", return_value=OPTIONS):
            with fixture.producer_ports():
                fixture.driver.cmd_sdk(fixture.args)
        configure = fixture.commands[1]
        self.assertIn("-DCMAKE_INSTALL_PREFIX=" + str(fixture.args.staging_dir), configure)
        self.assertIn("-DJCE_EMBED_ENGINE_SHADERS=ON", configure)
        self.assertEqual(sentinel.read_bytes(), b"original default SDK")
        self.assertEqual(list(default.iterdir()), [sentinel])
        self.assertFalse((fixture.plan["build"] / OPTIONS.REQUEST_FILE).exists())
        self.assertTrue((fixture.plan["install"] / OPTIONS.OPTIONS_FILE).is_file(),
                        "actual verifier omitted staged output options")
        self.assertTrue((fixture.plan["install"] / "jce-sdk-product.json").is_file())
        self.assertEqual(OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)["install"], fixture.args.staging_dir)
        resources = OPTIONS.cache_paths(vars(fixture.driver), fixture.args, fixture.target)
        self.assertEqual(resources[:3], [fixture.plan["build"], fixture.plan["conan"], fixture.plan["install"]])

    def test_staging_conflicts_refuse_before_any_producer_dispatch(self):
        fixture = self.fixture()
        fixture.stage()
        def no_command(*args, **kwargs):
            raise AssertionError("invalid staging request reached a producer command")
        for changes in ({"build_dir": fixture.root / "build/custom"},
            {"install_dir": fixture.root / "dist/sdk/custom"}, {"clean": True},
            {"embed_engine_shaders": "off"}, {"variant": "both"},
            {"delivery_profile": "authoring"}):
            args = SimpleNamespace(**vars(fixture.args))
            vars(args).update(changes)
            with self.subTest(changes=changes), patch.object(STANDALONE, "sdk_outputs", return_value=OPTIONS):
                with fixture.producer_ports(run=no_command) as ports, self.assertRaises((ValueError, SystemExit)):
                    fixture.driver.cmd_sdk(args)
                self.assertFalse(ports.lease.called)
                self.assertFalse(ports.environment.called)
                self.assertFalse(ports.commands.called)
        for target in (dict(fixture.target, cross=True), dict(fixture.target, emscripten=True)):
            with self.assertRaisesRegex(ValueError, "native embedded"):
                OPTIONS.selection(fixture.args, target, "embedded")
        with self.assertRaisesRegex(ValueError, "matching native host"):
            OPTIONS.resolve(fixture.driver, fixture.args, dict(fixture.target, host="linux"))

    def test_staging_boundaries_aliases_and_reparse_points_are_refused(self):
        fixture = self.fixture()
        fixture.stage()
        for value in (fixture.root, fixture.root / "build/sdk", fixture.root / "dist/sdk/private",
            fixture.root / "build/desktop/current", fixture.root / "build/sdk/../escape"):
            with self.subTest(value=value):
                args = SimpleNamespace(**vars(fixture.args));args.staging_dir = value
                with self.assertRaises(ValueError):
                    OPTIONS.resolve(fixture.driver, args, fixture.target)
        ordinary_lstat = Path.lstat
        blocked = fixture.root / "build/sdk"
        def reparse(path, *args, **kwargs):
            if path == blocked:
                return SimpleNamespace(st_mode=0o040755, st_file_attributes=0x400)
            return ordinary_lstat(path, *args, **kwargs)
        with patch.object(Path, "lstat", reparse), self.assertRaisesRegex(ValueError, "ordinary directory"):
            OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)

    def test_staging_reuse_requires_actual_installed_target_and_owned_output_options(self):
        fixture = self.fixture()
        fixture.stage()
        self.assertEqual(fixture.plan["install"], fixture.args.staging_dir)
        fixture.plan["install"].mkdir(parents=True)
        (fixture.plan["install"] / "unowned").write_bytes(b"not an SDK")
        with self.assertRaises(ValueError):
            OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)
        fixture.cache()
        fixture.installed()
        OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)
        fixture.installed(arch="i686", pointer_bits=32)
        with self.assertRaises(ValueError):
            OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)
        fixture.installed()
        options = fixture.plan["install"] / OPTIONS.OPTIONS_FILE
        data = json.loads(options.read_text());data["request"]["shaders"] = "OFF"
        options.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "output identity"):
            OPTIONS.resolve(fixture.driver, fixture.args, fixture.target)


def negative_control(control):
    fixture = Fixture()
    try:
        source = (ROOT / "tools/build/standalone.py").read_text(encoding="utf-8")
        helper = OPTIONS
        if control == "old-output-paths":
            old = 'build = plan["build"] if plan else driver.ROOT / "build/desktop" / stem'
            assert source.count(old) == 1, "mutation target is not unique"
            source = source.replace(old, 'build = driver.ROOT / "build/desktop" / stem')
        elif control == "missing-shader-wire":
            old = '*(["-DJCE_EMBED_ENGINE_SHADERS=" + plan["shaders"]] if plan else []),'
            assert source.count(old) == 1, "mutation target is not unique"
            source = source.replace(old, "")
        elif control == "missing-preflight-wire":
            helper_source = (ROOT / "tools/build/sdk_outputs.py").read_text(encoding="utf-8")
            old = '    plan = resolve(driver, args, target)\n    result = [plan["build"], plan["conan"], plan["install"]]'
            assert helper_source.count(old) == 1, "mutation target is not unique"
            helper_source = helper_source.replace(old, '    result = [args.build_dir, args.install_dir]')
            helper = load("sdk_output_missing_preflight", ROOT / "tools/build/sdk_outputs.py", helper_source)
            OPTIONS.state(fixture.plan, "configured")
            fixture.cache(JCE_BUILD_VARIANT="release")
        else:
            raise ValueError("unknown negative control")
        module = load("sdk_output_mutant_standalone", ROOT / "tools/build/standalone.py", source)
        with patch.object(module, "sdk_outputs", return_value=helper):
            with fixture.producer_ports(module) as ports:
                if control == "missing-preflight-wire":
                    try:
                        fixture.driver.cmd_sdk(fixture.args)
                    except ValueError:
                        pass
                    assert not ports.lease.called, "preflight omission acquired mutable cache ownership"
                    assert not ports.environment.called, "preflight omission entered environment setup"
                else:
                    fixture.driver.cmd_sdk(fixture.args)
        raise RuntimeError("negative control unexpectedly accepted")
    except AssertionError as error:
        print("SDK_OUTPUT_CONTROL_REFUSED: " + control + ": " + str(error))
        return 4
    finally:
        fixture.close()


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--negative-control":
        sys.exit(negative_control(sys.argv[2]))
    unittest.main()
