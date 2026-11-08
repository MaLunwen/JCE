"""CPU registration controls; these do not exercise engine/editor or GPU behavior."""
import importlib.util
import contextlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
METADATA = ROOT / "tests/cmake/JceTestMetadata.cmake"


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ctest"), "CMake/CTest unavailable")
class TestDomainRegistration(unittest.TestCase):
    def fixture(self, children, root_code="", metadata_text=None):
        temporary = tempfile.TemporaryDirectory(prefix="jce-domain-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        metadata = METADATA
        if metadata_text is not None:
            metadata = root / "JceTestMetadata.cmake"
            metadata.write_text(metadata_text, encoding="utf-8")
        lines = ["cmake_minimum_required(VERSION 3.20)", "project(Metadata NONE)",
                 "enable_testing()", f'include("{metadata.as_posix()}")',
                 # Only the authored fixture changes the classification root;
                 # no source/build-tree registrations or dependencies are read.
                 'set(_JCE_TEST_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")',
                 "add_custom_target(jce_tests)", "jce_create_test_domain_targets()",
                 root_code, "jce_register_test_directory()",
                 "cmake_language(DEFER CALL jce_assert_test_metadata)"]
        for directory, code in children:
            child = root / directory
            child.mkdir(parents=True)
            (child / "CMakeLists.txt").write_text(code, encoding="utf-8")
            lines.append(f'add_subdirectory("{directory}")')
        (root / "CMakeLists.txt").write_text("\n".join(lines), encoding="utf-8")
        build = root / "out"
        result = subprocess.run(["cmake", "-S", str(root), "-B", str(build)],
                                capture_output=True, text=True, timeout=30)
        return build, result

    @staticmethod
    def registration(name, kind="unit", labels="unit", domain=None):
        lines = ["jce_register_test_directory()",
                 f'add_test(NAME {name} COMMAND "${{CMAKE_COMMAND}}" -E true)']
        if kind is not None:
            lines.append(f"jce_set_test_kind({name} {kind})")
        if labels:
            lines.append(f'set_tests_properties({name} PROPERTIES LABELS "{labels}")')
        if domain:
            lines.append(f"set_tests_properties({name} PROPERTIES JCE_TEST_DOMAIN {domain})")
        return "\n".join(lines) + "\n"

    def inventory(self, build, suite=None):
        command = ["ctest", "--test-dir", str(build), "-C", "Release", "--show-only=json-v1"]
        if suite:
            command += ["-L", f"^domain:{suite}$"]
        result = subprocess.run(command, capture_output=True, text=True, timeout=15)

        def fail_inventory(reason, detail):
            self.fail(json.dumps({"failure": "ctest_inventory", "reason": reason,
                "command": command, "returncode": result.returncode,
                "stdout": result.stdout, "stderr": result.stderr, "detail": detail},
                ensure_ascii=False))

        if result.returncode != 0:
            fail_inventory("nonzero_exit", "CTest inventory command failed")
        try:
            document = json.loads(result.stdout)
        except (json.JSONDecodeError, TypeError) as error:
            fail_inventory("invalid_json", str(error))
        if not isinstance(document, dict) or not isinstance(document.get("tests"), list):
            fail_inventory("invalid_shape", "Expected a JSON object containing a tests array")
        return document["tests"]

    def test_source_owner_covers_raw_scripts_cxx_variants_and_early_return(self):
        build, result = self.fixture([
            ("tests/scripting/cpp", self.registration("cxx_engine", "integration", "unit;cxx")),
            ("tests/scripting/python", self.registration("raw_script", "contract") +
             self.registration("script_variant", "integration") + "return()\n"),
            ("tests/editor", self.registration("editor_model", labels="unit;cxx;editor")),
            ("tests/consumers/fixture", self.registration("consumer_rule")),
        ])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(self.inventory(build)), 5)
        self.assertEqual({test["name"] for test in self.inventory(build, "engine")},
                         {"cxx_engine", "raw_script", "script_variant"})
        self.assertEqual({test["name"] for test in self.inventory(build, "editor")},
                         {"editor_model"})
        self.assertEqual({test["name"] for test in self.inventory(build, "consumer")},
                         {"consumer_rule"})
        for test in self.inventory(build):
            labels = next(item["value"] for item in test["properties"] if item["name"] == "LABELS")
            self.assertIn("unit", labels)  # existing selectors remain compatible
            self.assertEqual(sum(label.startswith("domain:") for label in labels), 1)
            self.assertEqual(sum(label.startswith("kind:") for label in labels), 1)
            if test["name"] == "cxx_engine":
                self.assertNotIn("editor", labels)
                self.assertIn("kind:integration", labels)

    @staticmethod
    def provider(name):
        return (f"add_custom_target({name}\n"
                ' COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/witness"\n'
                f' COMMAND "${{CMAKE_COMMAND}}" -E touch "${{CMAKE_BINARY_DIR}}/witness/{name}"\n'
                " VERBATIM)\n")

    def test_actual_aggregate_builds_only_domain_inputs_and_preserves_complete_inventory(self):
        providers = ("engine_input", "editor_input", "consumer_input", "external_shared_runner")
        # Providers are declared outside any classified test directory. The
        # consumer call site, not target SOURCE_DIR, selects their domain(s).
        root_code = "".join(self.provider(name) for name in providers)
        consumer = ('jce_register_test_directory()\n'
                    f'add_test(NAME consumer_unavailable COMMAND "{Path(sys.executable).as_posix()}" '
                    '-c "raise SystemExit(77)")\n'
                    'jce_set_test_kind(consumer_unavailable integration)\n'
                    'set_tests_properties(consumer_unavailable PROPERTIES SKIP_RETURN_CODE 77)\n'
                    'jce_test_build_dependencies(consumer_input)\n')
        build, result = self.fixture([
            ("tests/scripting/python", self.registration("raw_script", "contract") +
             self.registration("script_variant", "integration") +
             "jce_test_build_dependencies(engine_input external_shared_runner)\nreturn()\n"),
            ("tests/editor", self.registration("editor_rule") +
             "jce_test_build_dependencies(editor_input external_shared_runner)\n"),
            ("tests/consumers/fixture", consumer),
        ], root_code)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        before = self.inventory(build)
        self.assertEqual(len(before), 4)
        for suite, expected in (
            ("engine", {"engine_input", "external_shared_runner"}),
            ("editor", {"editor_input", "external_shared_runner"}),
            ("consumer", {"consumer_input"}),
            ("all", set(providers)),
        ):
            with self.subTest(suite=suite):
                witness = build / "witness"
                if witness.exists():
                    for item in witness.iterdir():
                        item.unlink()  # Only files authored by this owned temporary fixture.
                target = "jce_tests" if suite == "all" else "jce_tests_" + suite
                result = subprocess.run(["cmake", "--build", str(build), "--target", target],
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual({p.name for p in witness.iterdir()}, expected)
                self.assertEqual(self.inventory(build), before)
        result = subprocess.run(["ctest", "--test-dir", str(build), "-C", "Release",
                                 "-L", "^domain:consumer$", "--output-on-failure"],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Skipped", result.stdout)  # Unavailable stays SKIP, not fabricated PASS.

    def test_actual_closure_rejects_removed_all_domain_wires_and_unclassified_input(self):
        metadata = METADATA.read_text(encoding="utf-8")
        code = self.registration("engine_rule") + "jce_test_build_dependencies(engine_input)\n"
        for wire, diagnostic in (
            ('add_dependencies(jce_tests ${ARGN})', "all-test closure differs"),
            ('add_dependencies(jce_tests_${_domain} ${ARGN})', "engine test build inputs are not wired"),
        ):
            with self.subTest(wire=wire):
                self.assertEqual(metadata.count(wire), 1)
                _, result = self.fixture([("tests/os/core", code)], self.provider("engine_input"),
                                         metadata.replace(wire, "", 1))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(diagnostic, result.stdout + result.stderr)
        _, result = self.fixture([("tests/os/core", self.registration("engine_rule") +
                                  "add_dependencies(jce_tests engine_input)\n")],
                                 self.provider("engine_input"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("all-test closure differs", result.stdout + result.stderr)
        # A domain without native inputs is valid: script registrations persist.
        build, result = self.fixture([("tests/consumers/fixture", self.registration("script_only"))])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run(["cmake", "--build", str(build), "--target", "jce_tests_consumer"],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(self.inventory(build, "consumer")), 1)

    def test_missing_kind_rejects_raw_registration(self):
        _, result = self.fixture([("tests/renderer", self.registration("missing_kind", kind=None))])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing a valid JCE_TEST_KIND", result.stdout + result.stderr)

    def test_missing_directory_registration_rejects_even_labelled_test(self):
        code = self.registration("missing_registration").replace("jce_register_test_directory()\n", "")
        _, result = self.fixture([("tests/renderer", code)])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("has no domain/kind registration", result.stdout + result.stderr)

    def test_same_name_in_another_directory_cannot_hide_missing_registration(self):
        missing = self.registration("same_name").replace("jce_register_test_directory()\n", "")
        _, result = self.fixture([
            ("tests/scripting/cpp", self.registration("same_name")),
            ("tests/editor", missing),
        ])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("has no domain/kind registration", result.stdout + result.stderr)

    def test_wrong_domain_declaration_and_labels_are_rejected(self):
        for domain, labels in (("editor", "unit"), (None, "unit;editor"),
                               (None, "unit;domain:editor")):
            with self.subTest(domain=domain, labels=labels):
                _, result = self.fixture([("tests/scripting/cpp",
                                          self.registration("wrong_domain", domain=domain, labels=labels))])
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("wrong domain", result.stdout + result.stderr)

    def test_wrong_kind_and_unknown_owner_are_rejected(self):
        _, result = self.fixture([("tests/renderer",
                                  self.registration("wrong_kind", labels="unit;kind:graphical"))])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("wrong kind", result.stdout + result.stderr)
        _, result = self.fixture([("tests/unowned", self.registration("unknown_owner"))])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("has no domain owner", result.stdout + result.stderr)

    def test_invalid_kind_rejects_before_directory_finalization(self):
        _, result = self.fixture([("tests/renderer", self.registration("invalid_kind", kind="gpu"))])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid kind", result.stdout + result.stderr)


class TestSuiteCLI(unittest.TestCase):
    def driver(self):
        spec = importlib.util.spec_from_file_location("jce_domain_cli", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        return driver

    def test_parser_default_all_and_explicit_domains(self):
        parser = self.driver().build_parser()
        self.assertEqual(parser.parse_args(["test"]).suite, "all")
        for domain in ("all", "engine", "editor", "consumer"):
            self.assertEqual(parser.parse_args(["test", "--suite", domain]).suite, domain)
        with self.assertRaises(SystemExit):
            parser.parse_args(["test", "--suite", "cxx"])

    def test_ai_dispatch_parser_only_selects_an_explicit_request(self):
        parser = self.driver().build_parser()
        self.assertIsNone(parser.parse_args(["test"]).ai_dispatch)
        for value in ("on", "off"):
            self.assertEqual(parser.parse_args(["test", "--configure", "--ai-dispatch", value]).ai_dispatch,
                             value)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            parser.parse_args(["test", "--configure", "--ai-dispatch", "auto"])
        self.assertEqual(error.exception.code, 2)

    def test_ai_dispatch_refuses_without_configure_before_cache_or_any_mutation(self):
        driver = self.driver()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            marker = root / "CMakeUserPresets.json"
            marker.write_bytes(b"KEEP_USER_PRESET")
            for value in ("on", "off", "invalid"):
                with self.subTest(value=value):
                    args = driver.build_parser().parse_args(["test"])
                    args.ai_dispatch = value
                    args.configure = value == "invalid"
                    with patch.object(driver, "ROOT", root), patch.object(driver, "DRY_RUN", False), \
                            patch.object(driver, "_cache_scope") as ownership, \
                            patch.object(driver, "resolve_target") as resolve, \
                            patch.object(driver, "conan_install") as install, \
                            patch.object(driver, "run") as command, \
                            contextlib.redirect_stderr(io.StringIO()) as stderr:
                        # Invoke the decorated command to cover pre-ownership refusal.
                        with self.assertRaises(SystemExit) as error:
                            driver.cmd_test(args)
                    self.assertEqual(error.exception.code, 1)
                    self.assertIn("--ai-dispatch", stderr.getvalue())
                    ownership.assert_not_called(); resolve.assert_not_called()
                    install.assert_not_called(); command.assert_not_called()
                    self.assertEqual(marker.read_bytes(), b"KEEP_USER_PRESET")
                    self.assertFalse((root / "build").exists())

    def test_ai_dispatch_configure_preserves_cache_default_and_selected_sdk(self):
        driver = self.driver()
        for value in (None, "on", "off"):
            for with_sdk in (False, True):
                with self.subTest(value=value, with_sdk=with_sdk), tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    cache = root / "native/CMakeCache.txt"
                    cache.parent.mkdir()
                    prior = b"JCE_ENABLE_AI_DISPATCH:BOOL=OFF\n"
                    cache.write_bytes(prior)
                    sdk = root / "renamed SDK"
                    options = ["test", "--configure", "--variant", "debug", "--suite", "consumer",
                               "--filter", "^fixture$"]
                    if value is not None:
                        options += ["--ai-dispatch", value]
                    if with_sdk:
                        options += ["--sdk", str(sdk), "--sdk-variant", "dist",
                                    "--sdk-delivery-profile", "embedded"]
                    args = driver.build_parser().parse_args(options)
                    commands = []
                    target = {"arch": "aarch64"}
                    with patch.object(driver, "ROOT", root), patch.object(driver, "DRY_RUN", True), \
                            patch.object(driver, "resolve_target", return_value=target), \
                            patch.object(driver, "msvc_env", return_value={}), \
                            patch.object(driver, "preset_binary_dir", return_value=cache.parent), \
                            patch.object(driver, "preset_name", return_value="fixture-debug"), \
                            patch.object(driver, "conan_install") as install, \
                            patch.object(driver, "run", side_effect=lambda command, **kw: commands.append(command)):
                        driver.cmd_test(args)
                    install.assert_called_once_with(target, "Debug", {})
                    configure = commands[0]
                    selections = [part for part in configure if part.startswith("-DJCE_ENABLE_AI_DISPATCH=")]
                    self.assertEqual(selections, [] if value is None else ["-DJCE_ENABLE_AI_DISPATCH=" + value.upper()])
                    self.assertIn("-DJCE_BUILD_VARIANT=debug", configure)
                    if with_sdk:
                        for flag in (f"-DJCE_TEST_SDK_DIR={sdk.resolve().as_posix()}",
                                     "-DJCE_TEST_SDK_ARCH=aarch64", "-DJCE_TEST_SDK_VARIANT=dist",
                                     "-DJCE_TEST_SDK_DELIVERY=embedded"):
                            self.assertIn(flag, configure)
                    else:
                        self.assertFalse(any(part.startswith("-DJCE_TEST_SDK_") for part in configure))
                    self.assertEqual(commands[1][-2:], ["--target", "jce_tests_consumer"])
                    self.assertEqual(commands[2][0], "ctest")
                    self.assertEqual(commands[2][commands[2].index("-L") + 1], "^domain:consumer$")
                    self.assertEqual(len(commands), 3)  # Subset never dispatches the cooker.
                    self.assertEqual(cache.read_bytes(), prior)

    def test_installed_sdk_selection_is_explicit_and_reaches_same_configure(self):
        driver = self.driver()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk = root / "renamed SDK"
            args = driver.build_parser().parse_args(["test", "--configure", "--variant", "debug",
                "--suite", "consumer", "--sdk", str(sdk), "--sdk-variant", "dist",
                "--sdk-delivery-profile", "embedded", "--filter", "^test_jce_sdk_consumer_"])
            commands = []
            target = {"arch": "aarch64"}
            with patch.object(driver, "ROOT", root), patch.object(driver, "DRY_RUN", True), \
                    patch.object(driver, "resolve_target", return_value=target), \
                    patch.object(driver, "msvc_env", return_value={}), \
                    patch.object(driver, "preset_binary_dir", return_value=root / "native"), \
                    patch.object(driver, "preset_name", return_value="fixture-debug"), \
                    patch.object(driver, "conan_install") as install, \
                    patch.object(driver, "run", side_effect=lambda command, **kw: commands.append(command)):
                driver.cmd_test.__wrapped__(args)
            install.assert_called_once_with(target, "Debug", {})
            configure = commands[0]
            self.assertIn(f"-DJCE_TEST_SDK_DIR={sdk.resolve().as_posix()}", configure)
            self.assertIn("-DJCE_TEST_SDK_ARCH=aarch64", configure)
            self.assertIn("-DJCE_TEST_SDK_VARIANT=dist", configure)
            self.assertIn("-DJCE_TEST_SDK_DELIVERY=embedded", configure)
            self.assertIn("-DJCE_BUILD_VARIANT=debug", configure)
            self.assertEqual(commands[-1][commands[-1].index("-L")+1], "^domain:consumer$")

    def test_sdk_flags_without_explicit_selection_fail_before_commands(self):
        driver = self.driver()
        for flags in (["--sdk", "fixture"], ["--sdk-variant", "dist"],
                      ["--configure", "--sdk-delivery-profile", "embedded"]):
            with self.subTest(flags=flags), patch.object(driver, "run") as commands:
                args = driver.build_parser().parse_args(["test", *flags])
                with self.assertRaises(SystemExit):
                    driver.cmd_test.__wrapped__(args)
                commands.assert_not_called()

    def test_suite_selection_preserves_build_closure_filter_repeat_without_cooker(self):
        driver = self.driver()
        for suite in (None, "all", "engine", "editor", "consumer"):
            with self.subTest(suite=suite), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                args = SimpleNamespace(arch=None, configure=False, jobs=1,
                    target=None, filter="script|variant", repeat_until_fail=2)
                if suite is not None:
                    args.suite = suite
                commands = []
                with patch.object(driver, "ROOT", root), patch.object(driver, "DRY_RUN", True), \
                        patch.object(driver, "HOST", "linux"), \
                        patch.object(driver, "resolve_target", return_value={}), \
                        patch.object(driver, "msvc_env", return_value={}), \
                        patch.object(driver, "preset_binary_dir", return_value=root / "native"), \
                        patch.object(driver, "run", side_effect=lambda command, **kw: commands.append(command)), \
                        patch.object(driver.subprocess, "run", return_value=SimpleNamespace(returncode=0)):
                    driver.cmd_test.__wrapped__(args)
                self.assertEqual(commands[0][-2:], ["--target", "jce_tests" if suite in (None, "all") else "jce_tests_" + suite])
                ctest = next(command for command in commands if command[0] == "ctest")
                self.assertIn("--no-tests=error", ctest)
                self.assertEqual(ctest[ctest.index("-R") + 1], "script|variant")
                self.assertEqual(ctest[ctest.index("--repeat") + 1], "until-fail:2")
                if suite in (None, "all"):
                    self.assertNotIn("-L", ctest)
                else:
                    self.assertEqual(ctest[ctest.index("-L") + 1], f"^domain:{suite}$")
                colour = [command for command in commands if any("verify_colour_space_chain.py" in str(arg)
                                                                for arg in command)]
                self.assertEqual(colour, [])

    def test_only_full_all_selection_dispatches_independent_colour_acceptance(self):
        driver = self.driver()
        cases = [
            ([], True),
            (["--suite", "all"], True),
            (["--repeat-until-fail", "2"], True),
            (["--suite", "engine"], False),
            (["--suite", "editor"], False),
            (["--suite", "consumer"], False),
            (["--filter", ".*"], False),
            (["--rerun-failed"], False),
            (["--target", "jce_tests", "--filter", ".*"], False),
            (["--variant", "debug", "--target", "test_jce_editor_headless_build_driver",
              "--target", "test_jce_engine_quit", "--filter",
              "^(test_jce_editor_headless_build_driver|test_jce_engine_quit)$"], False),
        ]
        for options, full in cases:
            with self.subTest(options=options), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                build = root / "native"
                build.mkdir()
                (build / "CMakeCache.txt").touch()
                args = driver.build_parser().parse_args(["test", "--jobs", "1", *options])
                commands = []
                with patch.object(driver, "ROOT", root), patch.object(driver, "DRY_RUN", False), \
                        patch.object(driver, "HOST", "linux"), \
                        patch.object(driver, "resolve_target", return_value={}), \
                        patch.object(driver, "msvc_env", return_value={}), \
                        patch.object(driver, "preset_binary_dir", return_value=build), \
                        patch.object(driver, "run", side_effect=lambda command, **kw: commands.append(command)), \
                        patch.object(driver.subprocess, "run", return_value=SimpleNamespace(returncode=0)) as probe:
                    driver.cmd_test.__wrapped__(args)
                self.assertEqual(commands[0], ["cmake", "--build", str(build),
                    "--target", *(args.target or ["jce_tests" if args.suite == "all" else "jce_tests_" + args.suite])])
                self.assertEqual(commands[1][:3], ["ctest", "--test-dir", str(build)])
                self.assertIn("--no-tests=error", commands[1])
                if args.filter:
                    self.assertEqual(commands[1][commands[1].index("-R") + 1], args.filter)
                self.assertEqual(len(commands), 4 if full else 2)
                self.assertEqual(probe.call_count, 1 if full else 0)
                if full:
                    self.assertEqual(commands[2], ["cmake", "--build", str(build), "--target", "jce_cook"])
                    self.assertEqual(commands[3], [str(Path(driver.sys.executable)),
                        str(root / "tools/verify_colour_space_chain.py"), "--cook",
                        str(build / "tools/jce_cook")])

    def test_failed_selected_ctest_never_dispatches_cooker_acceptance(self):
        driver = self.driver()
        commands = []
        def dispatch(command, **kwargs):
            commands.append(command)
            if command[0] == "ctest":
                raise SystemExit(8)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = driver.build_parser().parse_args(["test", "--filter", "^failed$"])
            with patch.object(driver, "ROOT", root), patch.object(driver, "DRY_RUN", True), \
                    patch.object(driver, "HOST", "linux"), \
                    patch.object(driver, "resolve_target", return_value={}), \
                    patch.object(driver, "msvc_env", return_value={}), \
                    patch.object(driver, "preset_binary_dir", return_value=root / "native"), \
                    patch.object(driver, "run", side_effect=dispatch), \
                    patch.object(driver.subprocess, "run") as probe:
                with self.assertRaises(SystemExit) as error:
                    driver.cmd_test.__wrapped__(args)
                self.assertEqual(error.exception.code, 8)
                probe.assert_not_called()
        self.assertEqual(len(commands), 2)
        self.assertEqual(commands[1][0], "ctest")

    def test_invalid_suite_rejected_before_resolving_or_building(self):
        driver = self.driver()
        with patch.object(driver, "resolve_target") as resolve, patch.object(driver, "run") as run:
            with self.assertRaises(SystemExit):
                driver.cmd_test.__wrapped__(SimpleNamespace(suite="cxx"))
            resolve.assert_not_called()
            run.assert_not_called()


class TestInventoryDiagnostics(unittest.TestCase):
    """Typed process ports exercise the actual inventory boundary, without CMake."""
    def inventory_result(self, stdout, stderr, suite="consumer"):
        build = Path("inventory diagnostic fixture")
        command = ["ctest", "--test-dir", str(build), "-C", "Release", "--show-only=json-v1"]
        if suite:
            command += ["-L", f"^domain:{suite}$"]
        completed = subprocess.CompletedProcess(command, 0, stdout=stdout, stderr=stderr)
        with patch.object(subprocess, "run", return_value=completed) as process:
            try:
                result = TestDomainRegistration.inventory(self, build, suite)
            finally:
                process.assert_called_once_with(command, capture_output=True, text=True, timeout=15)
        return result

    def test_rc_zero_invalid_inventory_is_an_ordinary_structured_failure(self):
        for label, stdout, reason in (
            ("empty", "", "invalid_json"),
            ("malformed", "{broken", "invalid_json"),
            ("wrong_top", "[]", "invalid_shape"),
            ("missing_tests", "{}", "invalid_shape"),
            ("wrong_tests", '{"tests": {}}', "invalid_shape"),
        ):
            with self.subTest(payload=label):
                stderr = "original ctest diagnostic\nsecond line"
                try:
                    self.inventory_result(stdout, stderr)
                except AssertionError as error:
                    try:
                        diagnostic = json.loads(str(error))
                    except (json.JSONDecodeError, TypeError):
                        self.fail("Inventory must fail with a structured process diagnostic")
                    self.assertEqual(diagnostic["failure"], "ctest_inventory")
                    self.assertEqual(diagnostic["reason"], reason)
                    self.assertEqual(diagnostic["returncode"], 0)
                    self.assertEqual(diagnostic["stdout"], stdout)
                    self.assertEqual(diagnostic["stderr"], stderr)
                    self.assertEqual(diagnostic["command"], ["ctest", "--test-dir",
                        "inventory diagnostic fixture", "-C", "Release", "--show-only=json-v1",
                        "-L", "^domain:consumer$"])
                    self.assertIsInstance(diagnostic["detail"], str)
                except Exception as error:
                    self.fail("Inventory must raise ordinary assertion, got " + type(error).__name__)
                else:
                    self.fail("Invalid inventory was returned instead of an ordinary failure")

    def test_normal_inventory_list_is_returned_unchanged_including_empty(self):
        for tests in ([], [{"name": "consumer_rule", "properties": [
                {"name": "LABELS", "value": ["unit", "domain:consumer", "kind:unit"]}]}]):
            with self.subTest(tests=tests):
                self.assertEqual(self.inventory_result(json.dumps({"tests": tests}), ""), tests)


if __name__ == "__main__":
    unittest.main()
