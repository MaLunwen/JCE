"""Test host-toolchain propagation without invoking a compiler."""
import importlib.util
import contextlib
import io
import hashlib
import json
import os
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import Mock, patch
ROOT = Path(__file__).resolve().parents[2]


class BuildDriverTests(unittest.TestCase):
    def driver(self, name):
        spec = importlib.util.spec_from_file_location(name, ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        return driver

    def test_checksums_collects_only_canonical_desktop_targets(self):
        driver = self.driver("jce_checksums_canonical_desktop")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            driver.ROOT = root
            desktop = root / "build/desktop"
            for key in ("windows-x64", "windows-x64-standalone", "windows-x64-xp"):
                candidate = desktop / key / "dist/jce_editor.exe"
                candidate.parent.mkdir(parents=True)
                candidate.write_text(key, encoding="ascii")

            driver.cmd_checksums(SimpleNamespace(dir=None, out=None))

            stage = root / "dist/release"
            self.assertEqual({path.name for path in stage.iterdir()},
                             {"SHA256SUMS", "jce_editor-windows-x64.exe"})
            self.assertEqual((stage / "jce_editor-windows-x64.exe").read_text(encoding="ascii"),
                             "windows-x64")

    def test_web_registry_proof_revalidates_existing_artifact_without_rebuild(self):
        driver = self.driver("jce_proof_existing_artifact")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk = root / "sdk"
            config = sdk / "lib/cmake/JCE/JCEConfig.cmake"
            config.parent.mkdir(parents=True)
            config.write_text("fixture")
            project = root / "project"
            output = project / "build/windows-wasm32-dist/App.html"
            output.parent.mkdir(parents=True)
            output.write_text("unchanged artifact")
            args = SimpleNamespace(project=str(project), arch="wasm", sdk=sdk,
                variant="dist", target=None, exe=None, registry_proof="actual-receipt.json",
                delivery_profile="embedded", clean=False, configure_only=False)
            target = {"host": driver.HOST, "arch": "wasm32", "emscripten": True}
            # Host-dependent directory uses the actual target descriptor.
            if driver.HOST != "windows":
                moved = project / f"build/{driver.HOST}-wasm32-dist/App.html"
                moved.parent.mkdir(parents=True)
                output.rename(moved); output = moved
            delivery = Mock()
            delivery.profile.return_value = "embedded"
            digest = hashlib.sha256(output.read_bytes()).hexdigest()
            with patch.dict(driver.sys.modules, {driver.__name__: driver}), \
                    patch.object(driver, "read_manifest", return_value={"target": "App"}), \
                    patch.object(driver, "resolve_target", return_value=target), \
                    patch.object(driver, "resolve_sdk", return_value=sdk), \
                    patch.object(driver, "_confirmed_app_sdk_tools", return_value={}), \
                    patch.object(driver, "_delivery_module", return_value=delivery), \
                    patch.object(driver, "_dist_prebuilt_assets") as repack, \
                    patch.object(driver, "run") as run:
                driver.cmd_app.__wrapped__(args)
                delivery.audit_application.assert_called_once_with(driver, args, output)
                run.assert_not_called(); repack.assert_not_called()
                self.assertEqual(hashlib.sha256(output.read_bytes()).hexdigest(), digest)
                for option in ("clean", "configure_only"):
                    setattr(args, option, True)
                    with self.assertRaises(SystemExit): driver.cmd_app.__wrapped__(args)
                    setattr(args, option, False)
                output.unlink()
                with self.assertRaises(SystemExit): driver.cmd_app.__wrapped__(args)
                run.assert_not_called(); repack.assert_not_called()

    def dist_fixture(self, root):
        project = root / "project"
        resources = project / "resources"
        scene = resources / "scenes/start.scene.json"
        scene.parent.mkdir(parents=True)
        scene.write_bytes(b'{"entities":[]}')
        cooked = resources / "_cooked"
        cooked.mkdir()
        (cooked / "unused.bin").write_bytes(b"must not be implicitly packed")
        packer, resolver = root / "jce_pak.exe", root / "jce_bundle_pack.exe"
        packer.write_bytes(b"packer port"); resolver.write_bytes(b"public graph port")
        return project, {"source_assets": "resources", "startup_scene": "scenes/start.scene.json"}, {
            "jce_pak": packer, "jce_bundle_pack": resolver}

    def dist_tool_port(self, command, **kwargs):
        """Compiler-free outputs: tests consumption, never claims real archive bytes."""
        if '--cook-assets' in command:
            output = Path(command[command.index('--out') + 1])
            output.mkdir(parents=True, exist_ok=True)
            startup = 'scenes/start.scene.json'
            payload = b'PUBLIC GRAPH PORT, not a JPAK'
            (output / 'admission.jbundle').write_bytes(payload)
            assets = [dict(path=startup, asset_id='0000000000000001', content_id='0000000000000101',
                           size=15, dependencies=[dict(address='dynamic/season.bin', asset_id='0000000000000002')],
                           type='scene', representation='json'),
                      dict(path='dynamic/season.bin', asset_id='0000000000000002', content_id='0000000000000102',
                           size=4, dependencies=[], type='binary', representation='raw')]
            metadata = dict(contract=dict(name='jce.bundle', major=1), id='admission', scene_path=startup,
                            archive_size=len(payload), content_hash='0000000000001234', assets=assets)
            (output / 'admission.jbundle.json').write_text(json.dumps(metadata), encoding='utf-8')
            graph = dict(contract=dict(name='jce.bundle.graph', major=1),
                         assets=[dict(asset, address=asset['path']) for asset in assets])
            (output / 'bundle_graph.json').write_text(json.dumps(graph), encoding='utf-8')
            return
        pak = Path(command[command.index('--pak-file') + 1])
        archive = Path(command[command.index('--selected-archive') + 1])
        meta = json.loads(archive.with_suffix(archive.suffix + '.json').read_text(encoding='utf-8'))
        rows = [dict(path=asset['path'], source=str(archive), selected=True,
                     size=asset['size'], content_id=asset['content_id']) for asset in meta['assets']]
        for index, value in enumerate(command[:-1]):
            if value != '--resource-dir': continue
            root = Path(command[index + 1])
            for file in root.rglob('*'):
                if file.is_file(): rows.append(dict(path=file.relative_to(root).as_posix(), source=str(file),
                                                   selected=False, size=file.stat().st_size, content_id='0000000000000000'))
        pak.write_bytes(b'AUTHENTICATED PACK PORT, not real crypto')
        if '--obj-file' in command:
            Path(command[command.index('--obj-file') + 1]).write_bytes(b'COFF PORT')
        count = len(rows)
        header = {name: True for name in ('header_verified', 'encrypted', 'secure_index', 'authenticated', 'auth_verified')}
        report = dict(schema='jce.pakbom.v1', file=str(pak), file_size=pak.stat().st_size, sources_verified=True,
                      selected_source=dict(path=str(archive), size=archive.stat().st_size,
                                           content_id=meta['content_hash'], imported_count=len(meta['assets'])),
                      sources=rows, header=header,
                      totals=dict(entries=count, verify_ran=True, verified_count=count, corrupt_count=0,
                                  skipped_count=0, encrypted_count=count, authenticated_count=count))
        Path(command[command.index('--json') + 1]).write_text(json.dumps(report), encoding='utf-8')

    def test_dist_web_embeds_authenticated_archive_without_assembly_wrapper(self):
        driver = self.driver("jce_dist_archive_target")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project, manifest, tools = self.dist_fixture(root)
            for web in (True, False):
                commands = []
                gen = project / "build/jce_generated"
                def tool(command, **kwargs):
                    commands.append(command)
                    self.dist_tool_port(command, **kwargs)
                with patch.object(driver, "_project_pak_key", return_value=bytes(32)), \
                        patch.object(driver, "_write_pak_key_shares_c") as key_source, \
                        patch.object(driver, "run", side_effect=tool), patch.object(driver, "cmd_cook") as whole_cook:
                    flags = driver._dist_prebuilt_assets(project,
                        {"emscripten": web, "arch": "x64", "host": "windows"}, project / "build",
                        manifest, {}, root / "sdk", sdk_tools=tools)
                self.assertEqual(len(commands), 2)
                whole_cook.assert_not_called()
                graph_command, command = commands
                self.assertIn('--cook-assets', graph_command)
                self.assertEqual(graph_command[graph_command.index('--target')+1], 'web' if web else 'windows')
                self.assertEqual(command[0], str(tools['jce_pak']))
                self.assertIn('--encrypt-key-file', command)
                self.assertIn('--selected-archive', command)
                self.assertIn('--json', command)
                self.assertIn(str(gen / 'runtime_boot'), command)
                self.assertNotIn(str(project / 'resources/_cooked'), command)
                self.assertIn('-DJCE_PROJECT_STAGE_LOOSE_ASSETS=OFF', flags)
                receipt = json.loads((gen / 'project_assets.receipt.json').read_text(encoding='utf-8'))
                self.assertEqual(receipt['selected_count'], 2)
                self.assertEqual(receipt['runtime_parity'], 'NOT VERIFIED')
                self.assertIn('safe-superset', receipt['stock_assets'])
                self.assertEqual(receipt['pak_sha256'], hashlib.sha256((gen / 'project_assets.pak').read_bytes()).hexdigest())
                key_source.assert_called_once_with(gen / 'jce_pak_key.c', bytes(32))
                if web:
                    self.assertIn(f'-DJCE_PROJECT_PREBUILT_ASSETS_PAK={gen / "project_assets.pak"}', flags)
                    self.assertEqual(command[command.index('--obj-format')+1], 'none')
                    self.assertNotIn('--obj-file', command)
                else:
                    self.assertIn(f'-DJCE_PROJECT_PREBUILT_ASSETS_OBJ={gen / "project_assets.obj"}', flags)
                    self.assertEqual(command[command.index('--obj-format')+1], 'coff')
                    self.assertIn('--obj-file', command)

    def test_dist_rejects_stale_graph_outputs_before_packer(self):
        driver = self.driver('jce_dist_missing_graph')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project, manifest, tools = self.dist_fixture(root)
            graph = project / 'build/jce_generated/content_graph'
            graph.mkdir(parents=True)
            for name in ('admission.jbundle', 'admission.jbundle.json', 'bundle_graph.json'):
                (graph / name).write_bytes(b'stale')
            with patch.object(driver, 'run') as run, patch.object(driver, '_project_pak_key') as key:
                with self.assertRaises(SystemExit):
                    driver._dist_prebuilt_assets(project, {'arch':'x64','host':'windows'}, project/'build',
                                                manifest, {}, root/'sdk', sdk_tools=tools)
                self.assertEqual(run.call_count, 1)
                key.assert_not_called()
                self.assertFalse((graph/'admission.jbundle').exists())

    def test_dist_rejects_unverified_or_changed_pack_proof(self):
        driver = self.driver('jce_dist_bad_bom')
        controls = ('authentication', 'deep_verify', 'selected_content', 'selected_closure', 'selected_identity', 'changed_input')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project, manifest, tools = self.dist_fixture(root)
            for control in controls:
                with self.subTest(control=control):
                    def tool(command, **kwargs):
                        self.dist_tool_port(command, **kwargs)
                        if '--json' not in command: return
                        path = Path(command[command.index('--json') + 1])
                        report = json.loads(path.read_text(encoding='utf-8'))
                        if control == 'authentication': report['header']['auth_verified'] = False
                        if control == 'deep_verify': report['totals']['verified_count'] -= 1
                        if control == 'selected_content': report['sources'][0]['content_id'] = 'ffffffffffffffff'
                        if control == 'selected_closure': report['sources'][0]['path'] = 'unused.bin'
                        if control == 'selected_identity': report['selected_source']['content_id'] = 'ffffffffffffffff'
                        if control == 'changed_input':
                            Path(command[command.index('--selected-archive') + 1]).write_bytes(b'changed graph')
                        path.write_text(json.dumps(report), encoding='utf-8')
                    with patch.object(driver, '_project_pak_key', return_value=bytes(32)), \
                            patch.object(driver, '_write_pak_key_shares_c') as shares, \
                            patch.object(driver, 'run', side_effect=tool):
                        with self.assertRaises(SystemExit):
                            driver._dist_prebuilt_assets(project, {'arch':'x64','host':'windows'}, project/'build',
                                                        manifest, {}, root/'sdk', sdk_tools=tools)
                        shares.assert_not_called()
                        self.assertFalse((project/'build/jce_generated/project_assets.receipt.json').exists())

    def test_dist_missing_sdk_resolver_fails_before_outputs(self):
        driver = self.driver('jce_dist_missing_resolver')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project, manifest, tools = self.dist_fixture(root)
            del tools['jce_bundle_pack']
            with patch.object(driver, 'run') as run:
                with self.assertRaises(SystemExit):
                    driver._dist_prebuilt_assets(project, {'arch':'x64'}, project/'build', manifest,
                                                {}, root/'sdk', sdk_tools=tools)
                run.assert_not_called()

    def test_web_smoke_single_html_and_split_artifacts(self):
        driver = self.driver("jce_web_smoke_artifacts")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = {"emscripten": True, "cross": True}
            self.assertEqual(driver._smoke_target_artifacts(target, root, "fixture"),
                             (root / "fixture.js", root / "fixture.wasm"))
            html = root / "fixture.html"
            html.write_text('<html><script>var wasmBinary="data:application/wasm;base64,AA==";</script></html>')
            self.assertEqual(driver._smoke_target_artifacts(target, root, "fixture"), (html, html))
            for invalid in ('<script src="fixture.js"></script>',
                            '<script>var file="fixture.wasm";</script>',
                            '<html><script>console.log("no runtime")</script></html>'):
                html.write_text(invalid)
                with self.assertRaises(SystemExit):
                    driver._smoke_target_artifacts(target, root, "fixture")
            # A malformed HTML must not fall back to stale split products.
            (root / "fixture.js").write_text('stale')
            (root / "fixture.wasm").write_bytes(b"stale")
            with self.assertRaises(SystemExit):
                driver._smoke_target_artifacts(target, root, "fixture")
            self.assertEqual(driver._smoke_target_artifacts({}, root, "fixture")[0].suffix,
                             ".exe" if driver.HOST == "windows" else "")

    def test_explicit_test_targets_and_default_commands(self):
        driver = self.driver("jce_selected_test_targets")
        for suite, selected, regex, expected in (
                ("all", None, None, ["jce_tests"]),
                ("engine", None, None, ["jce_tests_engine"]),
                ("editor", None, "fixture", ["jce_tests_editor"]),
                ("consumer", None, None, ["jce_tests_consumer"]),
                ("editor", ["test_fixture", "test_other"], "^test_(fixture|other)$",
                 ["test_fixture", "test_other"])):
            with self.subTest(selected=selected), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                args = SimpleNamespace(suite=suite, target=selected, filter=regex, arch=None,
                                       configure=False, jobs=1)
                commands = []
                with patch.object(driver, "ROOT", root), patch.object(driver, "DRY_RUN", True), \
                        patch.object(driver, "HOST", "linux"), \
                        patch.object(driver, "resolve_target", return_value={}), \
                        patch.object(driver, "msvc_env", return_value={}), \
                        patch.object(driver, "preset_binary_dir", return_value=root / "native"), \
                        patch.object(driver, "run", side_effect=lambda cmd, **kw: commands.append(cmd)), \
                        patch.object(driver.subprocess, "run", return_value=SimpleNamespace(returncode=0)):
                    driver.cmd_test.__wrapped__(args)
                self.assertEqual(commands[0], ["cmake", "--build", str(root / "native"),
                                               "--target", *expected])
                ctest = next(cmd for cmd in commands if cmd[0] == "ctest")
                if regex:
                    self.assertEqual(ctest[ctest.index("-R")+1], regex)
                else:
                    self.assertNotIn("-R", ctest)
                self.assertIn("--no-tests=error", ctest)
                if suite != "all":
                    self.assertEqual(ctest[ctest.index("-L")+1], "^domain:" + suite + "$")
                else:
                    self.assertNotIn("-L", ctest)

    def test_explicit_test_targets_require_filter_and_valid_names(self):
        driver = self.driver("jce_invalid_test_targets")
        for target, regex in ((["test_fixture"], None), (["test_fixture"], ""),
                (["bad;target"], "video"), (["bad target"], "video"),
                (["--clean-first"], "video"), (["bad/target"], "video"),
                (["bad\\target"], "video"), (["bad\nname"], "video")):
            with self.subTest(target=target), patch.object(driver, "resolve_target") as resolve:
                with self.assertRaises(SystemExit):
                    driver.cmd_test.__wrapped__(SimpleNamespace(target=target, filter=regex))
                resolve.assert_not_called()

    def test_editor_artifact_directory_and_default_reset_reuse_tree(self):
        driver = self.driver("jce_editor_artifact_test")
        target = {"key": "fixture", "host": driver.HOST, "cross": False}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            authoring = root / "build/desktop/fixture/authoring"
            tree = authoring / "obj/release"
            selected = root / "artifacts with spaces"
            for requested in (selected, None):
                args = SimpleNamespace(arch=None, variant="release", clean=False,
                                       artifact_dir=requested, standalone=False, delivery_profile="authoring")
                commands = []
                with patch.object(driver, "ROOT", root), patch.object(driver, "DRY_RUN", True), \
                        patch.object(driver, "resolve_target", return_value=target), \
                        patch.object(driver, "msvc_env", return_value={}), \
                        patch.object(driver, "conan_install"), \
                        patch.object(driver, "preset_name", return_value="fixture"), \
                        patch.object(driver, "run", side_effect=lambda cmd, **kw: commands.append(cmd)):
                    returned = driver.cmd_editor.__wrapped__(args)
                expected = selected if requested else authoring / "release"
                self.assertEqual(returned, expected / driver._host_exe("jce_editor"))
                self.assertIn("-DJCE_EDITOR_ARTIFACT_DIR=" + expected.as_posix(), commands[0])
                self.assertEqual(commands[0][commands[0].index("-B") + 1], str(tree))
                self.assertEqual(commands[1][2:4], [str(tree), "--target"])

    def test_sdk_build_requests_only_the_target_toolchains_stl_bridge(self):
        driver = self.driver("jce_sdk_native_targets_test")
        class CapturedBuild(Exception):
            pass
        for host, web in (("linux", False), ("darwin", False),
                          ("windows", False), ("windows", True)):
            with self.subTest(host=host, web=web), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                target = {"host": host, "cross": False, "emscripten": web}
                commands = []
                def capture(command, **kwargs):
                    commands.append(command)
                    if "--build" in command and "jce_sdk_fat_lib" in command:
                        self.assertEqual(command[-2:], ["-j", "2"])
                        raise CapturedBuild()
                with patch.object(driver, "ROOT", root), \
                        patch.object(driver, "DRY_RUN", True), \
                        patch.object(driver, "conan_install"), \
                        patch.object(driver, "preset_name", return_value="fixture-preset"), \
                        patch.object(driver, "preset_binary_dir", return_value=root / "build"), \
                        patch.object(driver, "sdk_install_dir", return_value=root / "dist"), \
                        patch.object(driver, "toolchain_path", return_value=root / "fixture.cmake"), \
                        patch.object(driver, "msvc_env", return_value={"CMAKE_BUILD_PARALLEL_LEVEL": "2"}), \
                        patch.dict(driver.os.environ, {"CMAKE_BUILD_PARALLEL_LEVEL": "2"}), \
                        patch.object(driver, "run", side_effect=capture):
                    with self.assertRaises(CapturedBuild):
                        driver._sdk_one(target, "release", "Release", False)
                argv = commands[-1]
                self.assertIn("jce_scripting_sdk_artifacts", argv)
                self.assertEqual("jce_msvc_stl_shims" in argv, host == "windows" and not web)

    def test_dependency_policy_defaults_and_invalid_inputs(self):
        driver = self.driver("jce_package_policy_test")
        self.assertEqual(driver.system_package_conan_args({}), [
            "-c", "tools.system.package_manager:mode=check",
            "-c", "tools.system.package_manager:sudo=False"])
        explicit = driver.system_package_conan_args({
            "JCE_SYSTEM_PACKAGE_MODE": "install", "JCE_SYSTEM_PACKAGE_SUDO": "1"})
        self.assertIn("tools.system.package_manager:mode=install", explicit)
        self.assertIn("tools.system.package_manager:sudo=True", explicit)
        for env in ({"JCE_SYSTEM_PACKAGE_MODE": "skip"}, {"JCE_SYSTEM_PACKAGE_SUDO": "yes"}):
            with self.assertRaises(SystemExit):
                driver.system_package_conan_args(env)

    def test_posix_debug_conan_configuration_precedes_upstream_cmake(self):
        driver = self.driver("jce_posix_conan_test")
        with tempfile.TemporaryDirectory() as tmp:
            tc = Path(tmp) / "conan_toolchain.cmake"
            tc.touch()
            target = {"host": "linux", "profile": "linux-x64", "arch": "x86_64"}
            env = {"JCE_SYSTEM_PACKAGE_MODE": "install"}
            with patch.object(driver, "HOST", "linux"), patch.object(driver, "sync_conan_hooks"), patch.object(driver, "export_conan_recipes"), patch.object(driver, "toolchain_path", return_value=tc), patch.object(driver, "conan_dir", return_value=Path(tmp)), patch.object(driver, "host_build_profile", return_value="linux-x64"), patch.object(driver, "run") as run:
                driver.conan_install(target, "Debug", env)
            argv = run.call_args.args[0]
            self.assertIn("build_type=Debug", argv)
            self.assertNotIn("compiler.runtime_type=Debug", argv)
            self.assertEqual(run.call_args.kwargs["env"]["CMAKE_POLICY_VERSION_MINIMUM"], "3.5")
            self.assertNotIn("CMAKE_POLICY_VERSION_MINIMUM", env)

    def test_dependency_jobs_follow_the_build_environment_in_both_contexts(self):
        driver = self.driver("jce_dependency_jobs_test")
        for web in (False, True):
            with self.subTest(web=web), tempfile.TemporaryDirectory() as tmp:
                tc = Path(tmp) / "conan_toolchain.cmake"
                tc.touch()
                target = {"host": "linux", "profile": "linux-x64", "arch": "x86_64",
                          "emscripten": web}
                env = {"CMAKE_BUILD_PARALLEL_LEVEL": "2"}
                em = {"toolchain": "fixture.cmake", "cc": "emcc", "cxx": "em++"}
                with patch.dict(driver.os.environ, {"CMAKE_BUILD_PARALLEL_LEVEL": "99"}), \
                        patch.object(driver, "HOST", "linux"), \
                        patch.object(driver, "sync_conan_hooks"), \
                        patch.object(driver, "export_conan_recipes"), \
                        patch.object(driver, "toolchain_path", return_value=tc), \
                        patch.object(driver, "conan_dir", return_value=Path(tmp)), \
                        patch.object(driver, "host_build_profile", return_value="linux-x64"), \
                        patch.object(driver, "emscripten_paths", return_value=em), \
                        patch.object(driver, "web_toolchain_conan_args", return_value=["-pr:h", "conan/profiles/wasm", "-s:h", "compiler=clang", "-s:h", "compiler.version=23", "-c:h", "user.jce:web_toolchain_abi=" + "a" * 64]) as web_identity, \
                        patch.object(driver, "run") as run:
                    driver.conan_install(target, "Release", env)
                argv = run.call_args.args[0]
                self.assertIn("tools.build:jobs=2", argv)
                self.assertEqual(argv[argv.index("tools.build:jobs=2") - 1], "-c:a")
                self.assertNotIn("tools.build:jobs=99", argv)

    def test_conan_metadata_refreshes_graph_inputs_and_reuses_native_tree(self):
        driver = self.driver("jce_conan_fingerprint_test")
        for host, web in (("windows", False), ("linux", False),
                          ("darwin", False), ("linux", True)):
            with self.subTest(host=host, web=web), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                profiles = root / "conan/profiles"
                profiles.mkdir(parents=True)
                (root / "contracts").mkdir()
                recipe = root / "conanfile.py"
                recipe.write_text("# initial recipe\n")
                profile = profiles / "fixture"
                profile.write_text("include(base)\n")
                base = profiles / "base"
                base.write_text("# base profile\n")
                lock = root / "conan.lock"
                lock.write_text('{"version": "0.5"}')
                tc = root / "native/generators/conan_toolchain.cmake"
                tc.parent.mkdir(parents=True)
                tc.touch()
                sentinel = root / "native/keep-compiled-object.o"
                sentinel.write_bytes(b"cached native object")
                target = {"host": host, "profile": "fixture", "arch": "x86_64",
                          "emscripten": web}
                env = {"CMAKE_BUILD_PARALLEL_LEVEL": "2"}
                em = {"toolchain": "fixture.cmake", "cc": "emcc", "cxx": "em++"}
                commands = []
                def capture(command, **kwargs):
                    commands.append(command)
                with patch.object(driver, "ROOT", root), \
                        patch.object(driver, "HOST", host), \
                        patch.object(driver, "sync_conan_hooks"), \
                        patch.object(driver, "export_conan_recipes"), \
                        patch.object(driver, "find_vs_install", return_value=None), \
                        patch.object(driver, "toolchain_path", return_value=tc), \
                        patch.object(driver, "conan_dir", return_value=root / "native"), \
                        patch.object(driver, "host_build_profile", return_value="fixture"), \
                        patch.object(driver, "emscripten_paths", return_value=em), \
                        patch.object(driver, "web_toolchain_conan_args", return_value=["-pr:h", "conan/profiles/wasm", "-s:h", "compiler=clang", "-s:h", "compiler.version=23", "-c:h", "user.jce:web_toolchain_abi=" + "a" * 64]) as web_identity, \
                        patch.object(driver, "run", side_effect=capture):
                    driver.stamp_graphics_tier(tc, "stable")
                    driver.conan_install(target, "Release", env)
                    self.assertEqual(len(commands), 1)  # old tree: no fingerprint
                    driver.conan_install(target, "Release", env)
                    self.assertEqual(len(commands), 1)  # identical inputs reused
                    for path in (recipe, profile, base, lock):
                        path.write_text(path.read_text() + "\n# changed")
                        count = len(commands)
                        driver.conan_install(target, "Release", env)
                        self.assertEqual(len(commands), count + 1, str(path))
                        driver.conan_install(target, "Release", env)
                        self.assertEqual(len(commands), count + 1)
                    count = len(commands)
                    env["JCE_SYSTEM_PACKAGE_MODE"] = "report"
                    driver.conan_install(target, "Release", env)
                    self.assertEqual(len(commands), count + 1)
                    count = len(commands)
                    env["JCE_BGFX_CPU_PROFILER"] = "on"
                    driver.conan_install(target, "Release", env)
                    self.assertEqual(len(commands), count + 1)
                    self.assertIn("user.jce:bgfx_cpu_profiler=on", commands[-1])
                    driver.conan_install(target, "Release", env)
                    self.assertEqual(len(commands), count + 1)
                    env["JCE_BGFX_CPU_PROFILER"] = "off"
                    driver.conan_install(target, "Release", env)
                    self.assertEqual(len(commands), count + 2)
                    self.assertIn("user.jce:bgfx_cpu_profiler=off", commands[-1])
                    count = len(commands)
                    env["JCE_BGFX_DEBUG_ANNOTATION"] = "on"
                    driver.conan_install(target, "Release", env)
                    self.assertEqual(len(commands), count + 1)
                    self.assertIn("user.jce:bgfx_debug_annotation=on", commands[-1])
                    driver.conan_install(target, "Release", env)
                    self.assertEqual(len(commands), count + 1)
                    env["JCE_BGFX_DEBUG_ANNOTATION"] = "auto"
                    driver.conan_install(target, "Release", env)
                    self.assertEqual(len(commands), count + 2)
                    if web:
                        count = len(commands)
                        web_identity.return_value[-1] = "user.jce:web_toolchain_abi=" + "b" * 64
                        driver.conan_install(target, "Release", env)
                        self.assertEqual(len(commands), count + 1)
                        driver.conan_install(target, "Release", env)
                        self.assertEqual(len(commands), count + 1)
                    self.assertEqual(sentinel.read_bytes(), b"cached native object")
                    self.assertTrue(tc.is_file())

    def test_bgfx_cpu_profiler_shared_args_are_explicit_and_refuse_invalid_values(self):
        driver = self.driver("jce_bgfx_profiler_args_test")
        for env, expected in (({}, "off"), ({"JCE_BGFX_CPU_PROFILER": "off"}, "off"),
                              ({"JCE_BGFX_CPU_PROFILER": "on"}, "on")):
            with self.subTest(env=env):
                args = driver.bgfx_graphics_conan_args(env)
                self.assertIn("user.jce:bgfx_graphics_tier=stable", args)
                self.assertIn("user.jce:bgfx_cpu_profiler=" + expected, args)
        for value in ("", "1", "0", "true", "profile", "on;off"):
            with self.subTest(value=value), self.assertRaises(SystemExit):
                driver.bgfx_graphics_conan_args({"JCE_BGFX_CPU_PROFILER": value})

    def test_bgfx_annotation_shared_args_keep_request_and_refuse_invalid(self):
        driver = self.driver("jce_bgfx_annotation_args_test")
        for value in (None, "auto", "off", "on", " ON "):
            env = {} if value is None else {"JCE_BGFX_DEBUG_ANNOTATION": value}
            expected = "auto" if value is None else value.strip().lower()
            self.assertIn("user.jce:bgfx_debug_annotation=" + expected,
                          driver.bgfx_graphics_conan_args(env))
        for value in ("", "1", "0", "true", "debug", "on;off"):
            with self.subTest(value=value), self.assertRaises(SystemExit):
                driver.bgfx_graphics_conan_args({"JCE_BGFX_DEBUG_ANNOTATION": value})

    def test_bgfx_cpu_profiler_survives_clean_msvc_environment_activation(self):
        driver = self.driver("jce_bgfx_profiler_msvc_env_test")
        requested = {"SystemRoot": r"C:\Windows", "PATH": "old compiler path",
                     "JCE_BGFX_CPU_PROFILER": "on", "JCE_BGFX_DEBUG_ANNOTATION": "on",
                     "JCE_GRAPHICS_API_TIER": "modern"}
        def activate(command, **kwargs):
            self.assertEqual(kwargs["env"].get("JCE_BGFX_CPU_PROFILER"), "on")
            self.assertEqual(kwargs["env"].get("JCE_BGFX_DEBUG_ANNOTATION"), "on")
            self.assertEqual(kwargs["env"].get("JCE_GRAPHICS_API_TIER"), "modern")
            return "\n".join(f"{k}={v}" for k,v in {
                **kwargs["env"], "PATH": "new compiler path", "INCLUDE": "headers", "LIB": "libs"}.items())
        with patch.object(driver, "HOST", "windows"), \
                patch.object(driver, "DRY_RUN", False), \
                patch.dict(driver.os.environ, requested, clear=True), \
                patch.object(driver, "vcvars_arch", return_value="x64"), \
                patch.object(driver, "find_vcvars", return_value=Path("fixture/vcvarsall.bat")), \
                patch.object(driver, "_msvc_env_ready", side_effect=[False, True]), \
                patch.object(driver.shutil, "which", return_value=None), \
                patch.object(driver.subprocess, "check_output", side_effect=activate) as child:
            env = driver.msvc_env({"arch": "x86_64", "cross": False})
        self.assertEqual(child.call_count, 1)
        self.assertIn("user.jce:bgfx_cpu_profiler=on", driver.bgfx_graphics_conan_args(env))
        self.assertIn("user.jce:bgfx_debug_annotation=on", driver.bgfx_graphics_conan_args(env))

    def test_shaderc_overrides_survive_clean_msvc_environment_activation(self):
        names = ("JCE_SHADERC_EXECUTABLE", "BGFX_SHADERC")
        selections = ({}, {names[0]: str(ROOT / "host tools/release/shaderc.exe")},
                      {names[1]: str(ROOT / "host tools/alias/shaderc.exe")},
                      {names[0]: "", names[1]: str(ROOT / "fallback/shaderc.exe")})
        for overrides in selections:
            with self.subTest(overrides=overrides):
                driver = self.driver("jce_msvc_shaderc_clean_environment")
                requested = {"SystemRoot": r"C:\Windows", "PATH": "old compiler path",
                             "INCLUDE": "wrong headers", "LIB": "wrong libraries",
                             "VCINSTALLDIR": "wrong VS", "JCE_SHADERC_UNRELATED": "drop",
                             **overrides}

                def activate(command, **kwargs):
                    base = kwargs["env"]
                    for name in names:
                        self.assertEqual(name in base, name in overrides,
                                         "clean MSVC must retain shader tool override presence")
                        if name in overrides:
                            self.assertEqual(base[name], overrides[name],
                                             "clean MSVC must retain shader tool override value")
                    for name in ("INCLUDE", "LIB", "VCINSTALLDIR", "JCE_SHADERC_UNRELATED"):
                        self.assertNotIn(name, base)
                    self.assertNotIn("old compiler path", base["PATH"])
                    return "\n".join(f"{key}={value}" for key, value in {
                        **base, "PATH": "native compiler", "INCLUDE": "headers",
                        "LIB": "native libs"}.items())

                with patch.object(driver, "HOST", "windows"), \
                        patch.object(driver, "DRY_RUN", False), \
                        patch.dict(driver.os.environ, requested, clear=True), \
                        patch.object(driver, "vcvars_arch", return_value="x64"), \
                        patch.object(driver, "find_vcvars", return_value=Path("fixture/vcvarsall.bat")), \
                        patch.object(driver, "_msvc_env_ready", side_effect=[False, True]), \
                        patch.object(driver.shutil, "which", return_value=None), \
                        patch.object(driver.subprocess, "check_output", side_effect=activate) as child:
                    env = driver.msvc_env({"arch": "x86_64", "cross": False})
                self.assertEqual(child.call_count, 1)
                self.assertEqual({name: env[name] for name in names if name in env}, overrides)

    def test_shaderc_overrides_survive_ready_msvc_environment(self):
        names = ("JCE_SHADERC_EXECUTABLE", "BGFX_SHADERC")
        for overrides in ({}, {names[0]: str(ROOT / "host tools/release/shaderc.exe")},
                          {names[1]: str(ROOT / "host tools/alias/shaderc.exe")},
                          {names[0]: "", names[1]: str(ROOT / "fallback/shaderc.exe")}):
            with self.subTest(overrides=overrides):
                driver = self.driver("jce_msvc_shaderc_ready_environment")
                requested = {"PATH": "native compiler", "INCLUDE": "headers",
                             "LIB": "native libs", **overrides}
                with patch.object(driver, "HOST", "windows"), \
                        patch.dict(driver.os.environ, requested, clear=True), \
                        patch.object(driver, "vcvars_arch", return_value="x64"), \
                        patch.object(driver, "_msvc_env_ready", return_value=True), \
                        patch.object(driver, "find_vcvars") as find, \
                        patch.object(driver.subprocess, "check_output") as child:
                    env = driver.msvc_env({"arch": "x86_64", "cross": False})
                self.assertEqual(env, requested)
                find.assert_not_called()
                child.assert_not_called()

    def test_clean_msvc_environment_preserves_job_budget(self):
        for raw, expected in ((None, "8"), ("2", "2"), (" 3 ", "3"),
                              ("", "8"), ("0", "8"), ("invalid", "8")):
            with self.subTest(raw=raw):
                driver = self.driver("jce_msvc_job_budget")
                requested = {"SystemRoot": r"C:\Windows", "PATH": "old compiler path"}
                if raw is not None:
                    requested["CMAKE_BUILD_PARALLEL_LEVEL"] = raw

                def activate(command, **kwargs):
                    self.assertEqual(kwargs["env"].get("CMAKE_BUILD_PARALLEL_LEVEL"),
                                     raw, "clean MSVC must retain the requested job budget")
                    return "\n".join(f"{key}={value}" for key, value in {
                        **kwargs["env"], "PATH": "native compiler", "INCLUDE": "headers",
                        "LIB": "native libs"}.items())

                with patch.object(driver, "HOST", "windows"), \
                        patch.object(driver, "DRY_RUN", False), \
                        patch.dict(driver.os.environ, requested, clear=True), \
                        patch.object(driver, "vcvars_arch", return_value="x64"), \
                        patch.object(driver, "find_vcvars", return_value=Path("fixture/vcvarsall.bat")), \
                        patch.object(driver, "_msvc_env_ready", side_effect=[False, True]), \
                        patch.object(driver.shutil, "which", return_value=None), \
                        patch.object(driver.subprocess, "check_output", side_effect=activate) as child:
                    env = driver.msvc_env({"arch": "x86_64", "cross": False})
                self.assertEqual(child.call_count, 1)
                self.assertEqual(env.get("CMAKE_BUILD_PARALLEL_LEVEL"), raw)
                self.assertEqual(driver.build_jobs(env), expected)

    def test_failed_conan_refresh_cannot_leave_a_success_stamp(self):
        driver = self.driver("jce_conan_failed_fingerprint_test")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "conan").mkdir()
            recipe = root / "conanfile.py"
            recipe.write_text("# initial\n")
            tc = root / "conan_toolchain.cmake"
            tc.touch()
            target = {"host": "linux", "profile": "fixture", "arch": "x86_64", "key": "fixture"}
            with patch.object(driver, "ROOT", root), \
                    patch.object(driver, "HOST", "linux"), \
                    patch.object(driver, "sync_conan_hooks"), \
                    patch.object(driver, "export_conan_recipes"), \
                    patch.object(driver, "toolchain_path", return_value=tc), \
                    patch.object(driver, "conan_dir", return_value=root), \
                    patch.object(driver, "host_build_profile", return_value="fixture"), \
                    patch.object(driver, "run") as run:
                driver.conan_install(target, "Release", {})
                stamp = tc.parent / ".jce-conan-generation"
                self.assertTrue(stamp.is_file())
                recipe.write_text("# changed\n")
                run.side_effect = RuntimeError("install failed")
                with self.assertRaisesRegex(RuntimeError, "install failed"):
                    driver.conan_install(target, "Release", {})
                self.assertFalse(stamp.exists())
                run.side_effect = None
                driver.conan_install(target, "Release", {})
                self.assertTrue(stamp.is_file())

    def test_runtime_boot_settings_preserve_values_remove_stale_and_avoid_duplicates(self):
        driver = self.driver("jce_boot_settings_test")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project, cooked, boot = root / "project", root / "cooked", root / "boot"
            (project / ".jce").mkdir(parents=True)
            authored = project / ".jce/project-settings.json"
            output = boot / "settings/project-settings.json"
            authored.write_text('{"graphics":{"color_space":0}}', encoding="utf-8")
            driver._stage_runtime_boot_settings(project, cooked, boot)
            self.assertEqual(output.read_bytes(), authored.read_bytes())
            authored.unlink()
            driver._stage_runtime_boot_settings(project, cooked, boot)
            self.assertFalse(output.exists())
            authored.write_text("authored")
            (cooked / "settings").mkdir(parents=True)
            (cooked / "settings/project-settings.json").write_text("cooked")
            driver._stage_runtime_boot_settings(project, cooked, boot)
            self.assertFalse(output.exists())

    def test_runtime_boot_settings_sync_authored_pipeline_before_prebuilt_pack(self):
        driver = self.driver("jce_boot_authored_pipeline")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project, cooked, boot = root / "project", root / "cooked", root / "boot"
            source = project / "Settings/RenderPipeline.rp.json"
            canonical = cooked / "settings/render_pipeline.rp.json"
            source.parent.mkdir(parents=True)
            canonical.parent.mkdir(parents=True)
            source.write_bytes(b'{"post_quality":"mid","custom":7}\n')
            canonical.write_bytes(b'{"post_quality":"low"}\n')
            user = cooked / "user-data.bin"
            user.write_bytes(b"preserve independent cooked data")
            driver._stage_runtime_boot_settings(project, cooked, boot)
            self.assertEqual(canonical.read_bytes(), source.read_bytes())
            self.assertFalse((boot / "settings/render_pipeline.rp.json").exists())
            original_mtime = canonical.stat().st_mtime_ns
            os.utime(canonical, ns=(original_mtime - 1000000000, original_mtime - 1000000000))
            pinned_mtime = canonical.stat().st_mtime_ns
            driver._stage_runtime_boot_settings(project, cooked, boot)
            self.assertEqual(canonical.stat().st_mtime_ns, pinned_mtime)
            source.write_bytes(b'{"post_quality":"high","custom":9}\n')
            driver._stage_runtime_boot_settings(project, cooked, boot)
            self.assertEqual(canonical.read_bytes(), source.read_bytes())
            source.unlink()
            preserved = canonical.read_bytes()
            driver._stage_runtime_boot_settings(project, cooked, boot)
            self.assertEqual(canonical.read_bytes(), preserved)
            self.assertEqual(user.read_bytes(), b"preserve independent cooked data")

    def test_dist_pipeline_stage_reaches_both_actual_pack_commands(self):
        driver = self.driver("jce_dist_pipeline_stage_order")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project, manifest, tools = self.dist_fixture(root)
            source = project / "Settings/RenderPipeline.rp.json"
            canonical = project / "resources/_cooked/settings/render_pipeline.rp.json"
            source.parent.mkdir(parents=True)
            canonical.parent.mkdir(parents=True)
            source.write_bytes(b'{"post_quality":"mid"}\n')
            for web in (True, False):
                canonical.write_bytes(b'{"post_quality":"low"}\n')
                commands = []
                def observed_tool(command, **kwargs):
                    self.assertEqual(canonical.read_bytes(), source.read_bytes())
                    commands.append(command)
                    self.dist_tool_port(command, **kwargs)
                    if '--selected-archive' in command:
                        policy = project / "build/jce_generated/runtime_boot/settings/render_pipeline.rp.json"
                        self.assertEqual(policy.read_bytes(), source.read_bytes())
                        self.assertNotIn(str(project/'resources/_cooked'), command)
                with patch.object(driver, "_project_pak_key", return_value=bytes(32)), \
                        patch.object(driver, "_write_pak_key_shares_c"), \
                        patch.object(driver, "run", side_effect=observed_tool):
                    driver._dist_prebuilt_assets(project, {"emscripten":web,"arch":"x64","host":"windows"},
                        project / "build", manifest, {}, root / "sdk", sdk_tools=tools)
                self.assertEqual(len(commands), 2)

    def test_tests_can_configure_debug_and_repeat_selected_ctest_without_cooker(self):
        driver = self.driver("jce_fresh_test_driver_test")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = root / "build"
            build.mkdir()
            (build / "CMakeCache.txt").touch()
            env = {"JCE_TEST_TOOLCHAIN": "sentinel"}
            args = SimpleNamespace(arch="x64", variant="debug", configure=True, jobs=2,
                                   filter="^test_fixture$", repeat_until_fail=20)
            with patch.object(driver, "ROOT", root), patch.object(driver, "HOST", "linux"), patch.object(driver, "resolve_target", return_value={"key": "fixture"}), patch.object(driver, "preset_binary_dir", return_value=build) as binary, patch.object(driver, "preset_name", return_value="linux-x64-debug"), patch.object(driver, "msvc_env", return_value=env), patch.object(driver, "conan_install") as conan, patch.object(driver, "run") as run, patch.object(driver.subprocess, "run", return_value=SimpleNamespace(returncode=0)):
                driver.cmd_test(args)
            self.assertEqual(2, binary.call_count)
            binary.assert_called_with({"key": "fixture"}, "debug")
            conan.assert_called_once_with({"key": "fixture"}, "Debug", env)
            self.assertIn("-DJCE_BUILD_TESTS=ON", run.call_args_list[0].args[0])
            self.assertIn("--preset", run.call_args_list[0].args[0])
            self.assertEqual(run.call_count, 3)  # configure, selected build, selected CTest
            self.assertEqual(run.call_args.args[0][0], "ctest")
            ctest = next(call.args[0] for call in run.call_args_list if call.args[0][0] == "ctest")
            self.assertIn("--no-tests=error", ctest)
            self.assertEqual(ctest[ctest.index("-R") + 1], "^test_fixture$")
            self.assertEqual(ctest[ctest.index("--repeat") + 1], "until-fail:20")

    def test_editor_bundle_keeps_script_runtime_and_excludes_local_assets(self):
        spec = importlib.util.spec_from_file_location("jce_bundle_test", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "build/desktop/fixture/authoring/dist"
            src.mkdir(parents=True)
            exe = driver._host_exe("jce_editor")
            for name in (exe, "JceScript.dll", "JceScript.runtimeconfig.json", "libfixture.so.1"):
                (src / name).write_bytes(b"fixture")
            for name in ("jce_script_java_classes", "jce_script_python", "video", "fonts"):
                (src / name).mkdir()
                (src / name / "fixture").write_bytes(b"fixture")
            sdk = root / "sdk"
            (sdk / "lib/cmake/JCE").mkdir(parents=True)
            (sdk / "lib/cmake/JCE/JCEConfig.cmake").touch()
            for name in ("LICENSE", "THIRD_PARTY_LICENSES.md"):
                (root / name).write_bytes(b"notice")
            out = root / "package"
            args = SimpleNamespace(arch="x64", variant="dist", skip_build=True, out=str(out), delivery_profile="authoring")
            target = {"host": driver.HOST, "arch": "x86_64", "key": "fixture"}
            with patch.object(driver, "ROOT", root), \
                 patch.object(driver, "resolve_target", return_value=target), \
                 patch.object(driver, "sdk_install_dir", return_value=sdk), \
                 patch.object(driver, "git_short_sha", return_value="fixture"):
                driver.cmd_package_editor(args)
                for name in ("JceScript.runtimeconfig.json", "libfixture.so.1", "jce_script_java_classes/fixture", "jce_script_python/fixture", "LICENSE", "THIRD_PARTY_LICENSES.md"):
                    self.assertTrue((out / name).is_file(), name)
                self.assertFalse((out / "video").exists())
                self.assertFalse((out / "fonts").exists())
                runtime = root / "python-runtime"
                runtime.mkdir()
                originals = {"python312.dll": b"dll", "python312.zip": b"stdlib", "python312._pth": b"python312.zip\n.\n", "LICENSE.txt": b"upstream terms"}
                for name, data in originals.items():
                    (runtime / name).write_bytes(data)
                args.python_runtime = str(runtime)
                driver.cmd_package_editor(args)
                for name, data in originals.items():
                    self.assertEqual((out / name).read_bytes(), data)
                    self.assertEqual((runtime / name).read_bytes(), data)
                (runtime / "python312.zip").unlink()
                with self.assertRaises(SystemExit):
                    driver.cmd_package_editor(args)
                args.python_runtime = None
                (src / "JceScript.runtimeconfig.json").unlink()
                with self.assertRaises(SystemExit):
                    driver.cmd_package_editor(args)

    def test_legacy_toolchain_stamp_cannot_reuse_patched_dependencies(self):
        spec = importlib.util.spec_from_file_location("jce_stamp_test", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec); spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory() as tmp:
            toolchain = Path(tmp) / "conan_toolchain.cmake"
            toolchain.touch()
            driver.graphics_tier_stamp(toolchain).write_text("stable\n")
            self.assertFalse(driver.graphics_tier_matches(toolchain, "stable"))
            driver.stamp_graphics_tier(toolchain, "stable")
            self.assertTrue(driver.graphics_tier_matches(toolchain, "stable"))
            self.assertFalse(driver.graphics_tier_matches(toolchain, "modern"))

    def test_hook_retirement_preserves_bytes_without_cross_volume_rename(self):
        spec = importlib.util.spec_from_file_location("jce_hook_test", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repository"
            home = Path(tmp) / "conan-home"
            (root / "conan/hooks").mkdir(parents=True)
            (root / "contracts").mkdir()
            hooks = home / "extensions/hooks"
            hooks.mkdir(parents=True)
            name = "hook_owned_legacy.py"
            original = b"# retired first-party fixture\n"
            installed = hooks / name
            installed.write_bytes(original)
            policy = {"retired_hooks": {name: hashlib.sha256(original).hexdigest()}}
            (root / "contracts/conan-source-policy.json").write_text(json.dumps(policy))
            with patch.object(driver, "ROOT", root), patch.dict(os.environ, {"CONAN_HOME": str(home)}), patch.object(Path, "replace", side_effect=OSError("different disk drive")):
                driver.sync_conan_hooks()
                driver.sync_conan_hooks()
            archive = root / "docs/delivery/retired-installed-conan-hooks" / name
            self.assertEqual(archive.read_bytes(), original)
            self.assertFalse(installed.exists())

    def test_hook_sync_uses_explicit_home_without_touching_process_home(self):
        spec = importlib.util.spec_from_file_location("jce_hook_env_test", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repository"
            original = Path(tmp) / "original-home"
            overlay = Path(tmp) / "overlay-home"
            (root / "conan/hooks").mkdir(parents=True)
            (root / "contracts").mkdir()
            (root / "contracts/conan-source-policy.json").write_text('{"retired_hooks":{}}')
            with patch.object(driver, "ROOT", root), patch.dict(os.environ, {"CONAN_HOME": str(original)}):
                driver.sync_conan_hooks({"CONAN_HOME": str(overlay)})
                self.assertEqual(os.environ["CONAN_HOME"], str(original))
            self.assertTrue((overlay / "extensions/hooks").is_dir())
            self.assertFalse(original.exists())

    def test_failed_hook_archive_keeps_installed_original(self):
        spec = importlib.util.spec_from_file_location("jce_hook_failure_test", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repository"
            home = Path(tmp) / "conan-home"
            (root / "conan/hooks").mkdir(parents=True)
            (root / "contracts").mkdir()
            hooks = home / "extensions/hooks"
            hooks.mkdir(parents=True)
            name = "hook_owned_legacy.py"
            original = b"# retired first-party fixture\n"
            installed = hooks / name
            installed.write_bytes(original)
            policy = {"retired_hooks": {name: hashlib.sha256(original).hexdigest()}}
            (root / "contracts/conan-source-policy.json").write_text(json.dumps(policy))
            with patch.object(driver, "ROOT", root), patch.dict(os.environ, {"CONAN_HOME": str(home)}), patch.object(driver.shutil, "copy2", side_effect=OSError("archive write failed")):
                with self.assertRaises(OSError):
                    driver.sync_conan_hooks()
            self.assertEqual(installed.read_bytes(), original)

    def test_test_command_propagates_compiler_environment_to_every_process(self):
        spec = importlib.util.spec_from_file_location("jce_build_env_test", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory(prefix="jce-build-env-") as tmp:
            build = Path(tmp)
            (build / "CMakeCache.txt").touch()
            env = {"JCE_TEST_TOOLCHAIN": "sentinel"}
            calls = []
            def record(argv, **kwargs):
                calls.append((argv, kwargs))
            with patch.dict(os.environ, {"JCE_TEST_REAL_FONT": "1"}), patch.object(driver, "resolve_target", return_value={"key": "fixture"}), patch.object(driver, "preset_binary_dir", return_value=build), patch.object(driver, "msvc_env", return_value=env), patch.object(driver, "run", side_effect=record), patch.object(driver, "ctest_jobs", return_value="1"), patch.object(driver.subprocess, "run", return_value=SimpleNamespace(returncode=0)) as probe:
                driver.cmd_test(SimpleNamespace(arch="x64", jobs=1, rerun_failed=False))
            self.assertEqual(probe.call_args.args[0][-1], "import PIL, numpy")
            self.assertIs(probe.call_args.kwargs["env"], calls[1][1]["env"])
            self.assertEqual(len(calls), 4)
            self.assertTrue(any(call[0][0] == "ctest" for call in calls))
            self.assertIs(calls[0][1]["env"], env)
            self.assertIs(calls[2][1]["env"], env)
            self.assertEqual(env, {"JCE_TEST_TOOLCHAIN": "sentinel"})
            for argv, kwargs in (calls[1], calls[3]):
                self.assertEqual(kwargs["env"]["JCE_TEST_REAL_FONT"], "1", argv)
                self.assertEqual(kwargs["env"]["JCE_TEST_TOOLCHAIN"], "sentinel", argv)
                self.assertIs(kwargs["env"], calls[1][1]["env"], argv)

    def test_colour_verification_falls_back_to_configured_python(self):
        spec = importlib.util.spec_from_file_location("jce_python_fallback_test", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            python = build / "configured-python"
            python.touch()
            (build / "CMakeCache.txt").write_text("_Python3_EXECUTABLE:INTERNAL=%s\n" % python)
            with patch.object(driver, "resolve_target", return_value={"key": "fixture"}), patch.object(driver, "preset_binary_dir", return_value=build), patch.object(driver, "msvc_env", return_value={"key": "fixture"}), patch.object(driver, "run") as run, patch.object(driver, "ctest_jobs", return_value="1"), patch.object(driver.subprocess, "run", side_effect=[SimpleNamespace(returncode=1), SimpleNamespace(returncode=0)]) as probe:
                driver.cmd_test(SimpleNamespace(arch="x64", jobs=1, rerun_failed=False))
            self.assertEqual(probe.call_count, 2)
            self.assertEqual(probe.call_args.args[0][0], str(python))
            self.assertEqual(run.call_args.args[0][0], str(python))

    def test_colour_verification_reports_missing_dependencies(self):
        spec = importlib.util.spec_from_file_location("jce_python_missing_test", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            (build / "CMakeCache.txt").touch()
            with patch.object(driver, "resolve_target", return_value={"key": "fixture"}), patch.object(driver, "preset_binary_dir", return_value=build), patch.object(driver, "msvc_env", return_value={"key": "fixture"}), patch.object(driver, "run") as run, patch.object(driver, "ctest_jobs", return_value="1"), patch.object(driver.subprocess, "run", return_value=SimpleNamespace(returncode=1)):
                with self.assertRaisesRegex(SystemExit, "requires Pillow and numpy"):
                    driver.cmd_test(SimpleNamespace(arch="x64", jobs=1, rerun_failed=False))
            self.assertEqual(run.call_count, 3)


    def test_static_api_capability_requires_sdk_type_and_real_archive(self):
        driver = self.driver('static_api_driver')
        with tempfile.TemporaryDirectory() as temporary:
            sdk = Path(temporary)
            lib = sdk / 'lib/Release'
            lib.mkdir(parents=True)
            config = sdk / 'lib/cmake/JCE/JCEScripting.cmake'
            config.parent.mkdir(parents=True)
            for language in ('c', 'cpp', 'python'):
                roster = config.parent / 'backends' / language / 'jce_backend.cmake'
                roster.parent.mkdir(parents=True)
                roster.write_text('set(JCE_BACKEND_LANGUAGE "' + language + '")\n'
                                  'set(JCE_BACKEND_VM_TARGET "jce_script_vm_' + language + '")\n'
                                  'set(JCE_BACKEND_NEEDS_SCRIPT_API ON)\n')
                (lib / ('jce_script_vm_' + language + '.lib')).write_bytes(b'archive-fixture')
            (lib / 'jce_script_api.lib').write_bytes(b'archive-fixture')
            config.write_text('set(JCE_SCRIPT_API_STATIC 0)\n')
            self.assertEqual(driver.sdk_shipped_script_backends(sdk), [])
            config.write_text('set(JCE_SCRIPT_API_STATIC 1)\n')
            self.assertEqual(driver.sdk_shipped_script_backends(sdk), ['c', 'cpp'])
            self.assertEqual(driver.sdk_shipped_script_backends(sdk, {'emscripten': True}), ['c', 'cpp'])
            (lib / 'jce_script_api.lib').unlink()
            self.assertEqual(driver.sdk_shipped_script_backends(sdk), [])
            (lib / 'jce_script_api.dll').write_bytes(b'shared-fixture')
            config.write_text('set(JCE_SCRIPT_API_STATIC 0)\n')
            self.assertEqual(driver.sdk_shipped_script_backends(sdk), ['c', 'cpp', 'python'])


class HostToolConfigurationControls(unittest.TestCase):
    """Actual host-tools dispatch with counted commands/env/cache ownership.

    Temporary markers and observed argv prove only the command plan. No
    compiler, native asset tool, Editor or Web application is executed.
    """

    def driver(self, name):
        return BuildDriverTests.driver(self, name)

    def omit_owner_flag(self, driver, flag):
        import ast
        import inspect
        owner = getattr(driver.ensure_host_tools, "__wrapped__", driver.ensure_host_tools)
        tree = ast.parse(inspect.getsource(owner))
        hits = 0
        for node in ast.walk(tree):
            if not isinstance(node, ast.List):
                continue
            kept = []
            for item in node.elts:
                if isinstance(item, ast.Constant) and item.value == flag:
                    hits += 1
                else:
                    kept.append(item)
            node.elts = kept
        self.assertEqual(hits, 1, "negative requires one actual configure wire")
        # Whole actual owner, including its ordinary cache decorator and body.
        exec(compile(ast.fix_missing_locations(tree), owner.__code__.co_filename, "exec"),
             driver.__dict__)

    def consume_dispatch(self, driver, root, *, cross, existing_tools, stale_cache,
                         fail_configure=False, existing_toolchain=True, budget="2"):
        toolchain = root / "build/host-conan/build/Release/generators/conan_toolchain.cmake"
        toolchain.parent.mkdir(parents=True)
        if existing_toolchain:
            toolchain.write_text("owned generator marker", encoding="utf-8")
        tools = ("jce_pak", "jce_cook", "jce_bin2obj")
        if existing_tools:
            for name in tools:
                path = root / "build/host/tools" / (name + (".exe" if driver.HOST == "windows" else ""))
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"existing tool marker; not executed")
        host = {"host": driver.HOST, "arch": "x86_64", "cross": True}
        target = {"host": driver.HOST, "arch": "wasm32", "emscripten": True, "cross": True}
        native_env = {"HOST_TOOL_PORT": "native"}
        if budget is not None:
            native_env["CMAKE_BUILD_PARALLEL_LEVEL"] = budget
        expected_jobs = driver.build_jobs(native_env)
        caller_env = {"HOST_TOOL_PORT": "cross-target", "CMAKE_BUILD_PARALLEL_LEVEL": "9"}
        commands, ownership = [], []
        active = False
        configured = {"JCE_BUILD_EDITOR": "ON", "JCE_BUILD_TESTS": "ON"} if stale_cache else {}

        def resolve(arch):
            return target.copy() if cross and arch == "wasm" else host.copy()

        def environment(selection):
            if selection.get("emscripten"):
                return caller_env
            # The reusable owner explicitly clears the selected host's cross bit.
            if selection.get("cross") is False:
                return native_env
            return caller_env if cross else native_env

        @contextlib.contextmanager
        def cache_scope(paths):
            nonlocal active
            self.assertEqual(paths, [root / "build/host", root / "build/host-conan"])
            self.assertFalse(active)
            ownership.append("enter")
            active = True
            try:
                yield
            finally:
                active = False
                ownership.append("exit")

        def command(argv, **kwargs):
            self.assertTrue(active, "commands must retain the existing host cache owner")
            self.assertEqual(kwargs.get("env"), native_env)
            self.assertEqual(kwargs.get("cwd"), root)
            commands.append(list(argv))
            if "-S" in argv:
                for token in argv:
                    if token.startswith("-D") and "=" in token:
                        key, value = token[2:].split("=", 1)
                        configured[key] = value
                if fail_configure:
                    raise RuntimeError("owned configure port failure")

        with patch.object(driver, "ROOT", root), \
                patch.object(driver, "resolve_target", side_effect=resolve), \
                patch.object(driver, "msvc_env", side_effect=environment), \
                patch.object(driver, "_cache_scope", side_effect=cache_scope), \
                patch.object(driver, "sync_conan_hooks"), \
                patch.object(driver, "export_conan_recipes"), \
                patch.object(driver, "run", side_effect=command), \
                contextlib.redirect_stdout(io.StringIO()):
            args = driver.build_parser().parse_args(["host-tools", "--arch", "wasm" if cross else "x64"])
            if fail_configure:
                with self.assertRaisesRegex(RuntimeError, "owned configure port failure"):
                    args.func(args)
            else:
                args.func(args)
        self.assertEqual(ownership, ["enter", "exit"])
        self.assertFalse(active)
        self.assertEqual(len(commands), (1 if fail_configure else 2) + int(not existing_toolchain))
        if not existing_toolchain:
            install = commands[0]
            self.assertEqual(install[:3], ["conan", "install", "."])
            selected = "tools.build:jobs=" + expected_jobs
            self.assertEqual(install.count(selected), 1,
                             "host Conan install must retain the shared job budget")
            self.assertEqual(install[install.index(selected) - 1], "-c:a")
            self.assertFalse(any("compiler_executables" in value or "Emscripten.cmake" in value
                                 for value in install))
        configure = commands[int(not existing_toolchain)]
        self.assertEqual(configure[:8], ["cmake", "-S", ".", "-B", "build/host", "-G", "Ninja",
                                        "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain)])
        self.assertEqual(configured["CMAKE_BUILD_TYPE"], "Release")
        self.assertEqual(configured["JCE_DELIVERY_PROFILE"], "authoring")
        self.assertEqual(configured["JCE_ENABLE_SDK_INSTALL"], "ON")
        self.assertEqual(configured.get("JCE_BUILD_EDITOR"), "OFF",
                         "host-only configure must turn off the Editor")
        self.assertEqual(configured.get("JCE_BUILD_TESTS"), "OFF",
                         "host-only configure must turn off tests")
        if not fail_configure:
            self.assertEqual(commands[-1], ["cmake", "--build", "build/host", "--target",
                                           *tools, "-j", expected_jobs])

    def test_fresh_host_dependencies_and_build_share_job_budget(self):
        for budget in ("2", None, "invalid"):
            with self.subTest(budget=budget), tempfile.TemporaryDirectory() as temporary:
                driver = self.driver("jce_fresh_host_job_budget")
                self.consume_dispatch(driver, Path(temporary), cross=True,
                                      existing_tools=False, stale_cache=False,
                                      existing_toolchain=False, budget=budget)

    def test_native_fresh_and_stale_workspaces_disable_unrequested_consumers(self):
        for existing in (False, True):
            with self.subTest(existing_tools=existing), tempfile.TemporaryDirectory() as temporary:
                driver = self.driver("jce_host_scope_native")
                self.consume_dispatch(driver, Path(temporary), cross=False,
                                      existing_tools=existing, stale_cache=existing)

    def test_web_caller_still_refreshes_host_only_targets_with_native_environment(self):
        with tempfile.TemporaryDirectory() as temporary:
            driver = self.driver("jce_host_scope_web")
            self.consume_dispatch(driver, Path(temporary), cross=True,
                                  existing_tools=True, stale_cache=True)

    def test_either_missing_off_wire_fails_the_same_dispatch_admission(self):
        for flag, reason in (("-DJCE_BUILD_EDITOR=OFF", "host-only configure must turn off the Editor"),
                             ("-DJCE_BUILD_TESTS=OFF", "host-only configure must turn off tests")):
            with self.subTest(omitted=flag), tempfile.TemporaryDirectory() as temporary:
                driver = self.driver("jce_host_scope_omission")
                self.omit_owner_flag(driver, flag)
                with self.assertRaisesRegex(AssertionError, reason):
                    self.consume_dispatch(driver, Path(temporary), cross=True,
                                          existing_tools=True, stale_cache=True)

    def test_failed_configure_releases_cache_owner_without_dispatching_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            driver = self.driver("jce_host_scope_configure_failure")
            self.consume_dispatch(driver, Path(temporary), cross=True,
                                  existing_tools=False, stale_cache=False, fail_configure=True)


class ProjectPackageSafetyControls(unittest.TestCase):
    """Real temp paths and SDK admission; headless child is a counted port.

    These checks certify CLI scope/refusal, not native publishing or EXE ABI.
    The existing native package helper remains the sole publishing owner.
    """
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="jce-package-output-")
        self.addCleanup(temp.cleanup)
        self.workspace = Path(temp.name) / "workspace"
        self.project = self.workspace / "projects/fixture"
        self.project.mkdir(parents=True)
        self.manifest = {"name": "Fixture App", "target": "Probe", "exe": "Probe",
                         "version": "1.2.3"}
        self.write_manifest()
        self.sdk = self.workspace / "dist/sdk/relocated-sdk"
        config = self.sdk / "lib/cmake/JCE/JCEConfig.cmake"
        config.parent.mkdir(parents=True)
        config.write_text("CONFIG_PRESENCE_ONLY", encoding="utf-8")
        self.metadata = dict(schema=1, system="Windows", arch="x86_64", pointer_bits=64,
            config="Release", variant="dist", delivery="embedded", crt="MultiThreaded",
            archive="jce_engine_core.lib")
        self.metadata_path = self.sdk / "lib/Release/jce-sdk-target.json"
        self.metadata_path.parent.mkdir(parents=True)
        (self.metadata_path.parent / "jce_engine_core.lib").write_bytes(b"NOT_AN_ABI_ARCHIVE")
        self.write_metadata()
        self.editor = self.workspace / "build/editor-port.exe"
        self.editor.parent.mkdir(parents=True)
        self.editor.write_bytes(b"COUNTED_PROCESS_PORT JCE_HEADLESS_BUILD_PROJECT")
        spec = importlib.util.spec_from_file_location("jce_package_safety_control",
                                                     ROOT / "tools/build/jce.py")
        self.driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.driver)
        self.delivery = Mock()
        self.delivery.profile.return_value = "embedded"
        self.delivery.native_artifact_name.side_effect = self.driver._delivery_module().native_artifact_name
        self.delivery.admit_project.return_value = []
        patches = [patch.object(self.driver, "ROOT", self.workspace),
                   patch.object(self.driver, "HOST", "windows"),
                   patch.object(self.driver, "host_arch", return_value="x86_64"),
                   patch.dict(self.driver.sys.modules, {self.driver.__name__: self.driver}),
                   patch.object(self.driver, "_delivery_module", return_value=self.delivery),
                   patch.object(self.driver, "cmd_cook"), patch.object(self.driver, "cmd_app"),
                   patch.object(self.driver.shutil, "rmtree"),
                   patch.object(self.driver.subprocess, "run", side_effect=self.headless_port)]
        started = [p.start() for p in patches]
        for p in patches:
            self.addCleanup(p.stop)
        self.cook, self.app, self.remove, self.child = started[-4:]
        self.args = self.driver.build_parser().parse_args([
            "package", "game", str(self.project), "--arch", "x64", "--variant", "dist",
            "--sdk", str(self.sdk), "--editor", str(self.editor)])

    def write_manifest(self):
        (self.project / "jce_project.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def write_metadata(self):
        self.metadata_path.write_text(json.dumps(self.metadata), encoding="utf-8")

    def headless_port(self, argv, **kwargs):
        # No application runs or final output writes occur in this CPU port.
        self.assertEqual(argv, [str(self.editor)])
        result = Path(kwargs["env"]["JCE_HEADLESS_BUILD_RESULT"])
        result.write_text(json.dumps({"schema": "jce.editor.headless-build-result.v1",
            "state": "succeeded", "package_path": kwargs["env"]["JCE_HEADLESS_BUILD_OUT"]}),
            encoding="utf-8")
        return SimpleNamespace(returncode=0)

    def default_output(self):
        return self.project / "dist/apps/Fixture App/win32-x86_64-dist"

    def invoke(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.args.func(self.args)
        self.cook.assert_not_called()
        self.app.assert_not_called()
        self.remove.assert_not_called()

    def rejected(self, expected):
        before = self.child.call_count
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as result:
            self.invoke()
        self.assertEqual(result.exception.code, 1)
        self.assertIn(expected, stderr.getvalue())
        self.assertEqual(self.child.call_count, before)
        self.remove.assert_not_called()
        self.cook.assert_not_called()
        self.app.assert_not_called()

    def test_default_uses_shared_headless_owner_and_leaves_legacy_product(self):
        legacy = self.project / "dist/games/Fixture App-1.2.3-win32-x86_64"
        legacy.mkdir(parents=True)
        old = legacy / "player-note.txt"
        old.write_bytes(b"KEEP_LEGACY")
        self.invoke()
        self.child.assert_called_once()
        env = self.child.call_args.kwargs["env"]
        self.assertEqual(env["JCE_HEADLESS_BUILD_OUT"], str(self.default_output()))
        self.assertEqual(env["JCE_HEADLESS_BUILD_TARGET"], "Probe")
        self.assertEqual(env["JCE_HEADLESS_BUILD_EXE"], "Probe.exe")
        self.assertEqual(env["JCE_HEADLESS_BUILD_VARIANT"], "dist")
        self.assertEqual(old.read_bytes(), b"KEEP_LEGACY")
        self.assertFalse(self.default_output().exists())  # CPU port never publishes.

    def test_explicit_resolved_external_directory_and_dist_same_name_file(self):
        external = Path(self.project.parents[2]) / "external delivery"
        external.mkdir()
        self.args.out = str(external / "temporary/../final")
        self.invoke()
        self.assertEqual(self.child.call_args.kwargs["env"]["JCE_HEADLESS_BUILD_OUT"],
                         str(external / "final"))
        out = external / "old owned package"
        out.mkdir()
        old = out / "Probe.exe"
        old.write_bytes(b"KEEP_COMPLETE_OLD_APPLICATION")
        self.args.out = str(out)
        self.invoke()
        self.assertEqual(old.read_bytes(), b"KEEP_COMPLETE_OLD_APPLICATION")
        self.assertEqual(self.child.call_count, 2)

    def test_release_has_separate_layout_and_refuses_nonempty_directory(self):
        self.args.variant = "release"
        self.metadata["variant"] = "release"
        self.write_metadata()
        self.invoke()
        self.assertTrue(self.child.call_args.kwargs["env"]["JCE_HEADLESS_BUILD_OUT"].endswith(
            "win32-x86_64-release"))
        out = self.project / "dist/apps/Fixture App/win32-x86_64-release"
        out.mkdir(parents=True)
        old = out / "Probe.exe"
        old.write_bytes(b"KEEP_AUTHORING")
        self.rejected("unowned entry")
        self.assertEqual(old.read_bytes(), b"KEEP_AUTHORING")

    def test_roots_project_sources_build_sdk_and_workspace_are_refused(self):
        paths = [Path(self.project.anchor), self.project.parent, self.project,
                 self.project / "src", self.project / "build", self.project / "assets",
                 self.project / "resources/_cooked", self.sdk, self.sdk / "empty-output",
                 self.workspace / "engine", self.workspace / "build/final"]
        marker = self.project / "src/user-source.c"
        marker.parent.mkdir()
        marker.write_bytes(b"KEEP_USER_SOURCE")
        for output in paths:
            with self.subTest(output=output):
                self.args.out = str(output)
                self.rejected("package:")
        self.delivery.admit_project.assert_not_called()
        self.assertEqual(marker.read_bytes(), b"KEEP_USER_SOURCE")

    def test_nonowned_entries_and_file_destinations_are_kept(self):
        for leaf, directory in (("notes.txt", False), ("Old.exe", False),
                                ("resources", True), ("Probe.exe", True)):
            with self.subTest(leaf=leaf, directory=directory):
                out = self.workspace / "dist" / ("case-" + leaf)
                out.mkdir()
                entry = out / leaf
                if directory:
                    entry.mkdir()
                else:
                    entry.write_bytes(b"KEEP_UNKNOWN")
                self.args.out = str(out)
                self.rejected("unowned entry")
                self.assertTrue(entry.exists())
                if not directory:
                    self.assertEqual(entry.read_bytes(), b"KEEP_UNKNOWN")
        self.args.out = str(self.project / "jce_project.json")
        self.rejected("project source/build directory")
        self.assertEqual(json.loads((self.project / "jce_project.json").read_text()), self.manifest)
        file = self.workspace / "dist/final-file"
        file.write_bytes(b"KEEP_NOT_DIRECTORY")
        self.args.out = str(file)
        self.rejected("directory is a file")
        self.assertEqual(file.read_bytes(), b"KEEP_NOT_DIRECTORY")

    def test_unsafe_manifest_leafs_fail_before_any_child(self):
        for name in ("../outside", "nested/name", "CON", "trailing.", "bad\\name", "bad\nname"):
            with self.subTest(name=name):
                self.manifest["name"] = name
                self.write_manifest()
                self.rejected("unsafe filename component")
        self.delivery.admit_project.assert_not_called()

    def test_unsupported_compatibility_options_are_not_silently_ignored(self):
        for field, value, expected in (("name", "Different", "--name override"),
            ("version", "9.9.9", "--version override"), ("with_loose", True, "--with-loose")):
            with self.subTest(field=field):
                setattr(self.args, field, value)
                self.rejected(expected)
                setattr(self.args, field, False if field == "with_loose" else None)
        self.manifest["stage_loose"] = True
        self.write_manifest()
        self.rejected("stage_loose")
        self.delivery.admit_project.assert_not_called()

    def test_matching_name_version_and_clean_reach_existing_request(self):
        self.args.name = self.manifest["name"]
        self.args.version = self.manifest["version"]
        self.args.clean = True
        self.invoke()
        self.assertEqual(self.child.call_args.kwargs["env"]["JCE_HEADLESS_BUILD_CLEAN"], "1")

    def test_private_result_inside_output_cannot_delete_old_application(self):
        out = self.default_output()
        out.mkdir(parents=True)
        old = out / "Probe.exe"
        old.write_bytes(b"KEEP_EXE_NOT_RESULT")
        self.args.result = str(old)
        self.rejected("private result path must be outside")
        self.assertEqual(old.read_bytes(), b"KEEP_EXE_NOT_RESULT")

    def test_dry_run_does_not_create_output_or_delete_prior_result(self):
        result = self.workspace / "build/prior-result.json"
        result.write_bytes(b"KEEP_PRIOR_RESULT")
        self.args.result = str(result)
        with patch.object(self.driver, "DRY_RUN", True):
            self.invoke()
            self.args.result = str(self.workspace / "build/not-created/result.json")
            self.invoke()
        self.child.assert_not_called()
        self.assertEqual(result.read_bytes(), b"KEEP_PRIOR_RESULT")
        self.assertFalse(self.default_output().exists())
        self.assertFalse((self.workspace / "build/not-created").exists())

    def test_removing_actual_admission_wire_is_detected_by_negative_control(self):
        out = self.project / "src"
        out.mkdir()
        keep = out / "source.c"
        keep.write_bytes(b"KEEP_SOURCE")
        self.args.out = str(out)
        self.rejected("project source/build directory")
        with patch.object(self.driver, "_project_package_output", return_value=out):
            with self.assertRaisesRegex(AssertionError, "SystemExit not raised"):
                self.rejected("project source/build directory")
        self.child.assert_called_once()  # Mutant dispatches; it never publishes.
        self.assertEqual(keep.read_bytes(), b"KEEP_SOURCE")

    def test_headless_failure_discloses_current_log_path_without_masking_build_error(self):
        observed = {}
        def failed_headless(argv, **kwargs):
            self.headless_port(argv, **kwargs)
            result = Path(kwargs["env"]["JCE_HEADLESS_BUILD_RESULT"])
            path = result.parent / "current observed build.log"
            path.write_text("CMake Error: exact child diagnostic\n", encoding="utf-8")
            observed["path"] = path
            result.write_text(json.dumps({"schema": "jce.editor.headless-build-result.v1",
                "state": "failed", "stage": "compile", "exit_code": 1,
                "error": "tool exited with code 1", "log_path": str(path),
                "log_complete": True, "log_truncated": False, "log_incomplete": False}), encoding="utf-8")
            return SimpleNamespace(returncode=1)
        self.child.side_effect = failed_headless
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr), \
                self.assertRaises(SystemExit) as result:
            self.args.func(self.args)
        self.assertEqual(result.exception.code, 1)
        self.assertIn(str(observed["path"]), stdout.getvalue())
        self.assertIn("tool exited with code 1", stderr.getvalue())
        self.assertEqual(self.child.call_count, 1)
        self.cook.assert_not_called(); self.app.assert_not_called()

    def test_headless_unavailable_log_is_reported_separately_from_the_failed_build(self):
        def failed_headless(argv, **kwargs):
            self.headless_port(argv, **kwargs)
            result = Path(kwargs["env"]["JCE_HEADLESS_BUILD_RESULT"])
            result.write_text(json.dumps({"schema": "jce.editor.headless-build-result.v1",
                "state": "failed", "stage": "compile", "exit_code": 17,
                "error": "original compiler failure", "log_path": "",
                "log_status": "write_failed", "log_error": "host_atomic_write",
                "log_complete": False, "log_truncated": True, "log_incomplete": True}), encoding="utf-8")
            return SimpleNamespace(returncode=17)
        self.child.side_effect = failed_headless
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr), \
                self.assertRaises(SystemExit) as result:
            self.args.func(self.args)
        self.assertEqual(result.exception.code, 1)
        self.assertIn("build log write_failed", stdout.getvalue())
        self.assertIn("complete=false truncated=True incomplete=True", stdout.getvalue())
        self.assertIn("host_atomic_write", stdout.getvalue())
        self.assertIn("original compiler failure", stderr.getvalue())
        self.assertIn("exit_code=17", stderr.getvalue())
        self.assertEqual(self.child.call_count, 1)
        self.cook.assert_not_called(); self.app.assert_not_called()

    def test_sdk_mismatch_still_refuses_before_headless_dispatch(self):
        self.metadata["arch"] = "aarch64"
        self.write_metadata()
        self.rejected("arch mismatch")
        self.delivery.admit_project.assert_not_called()


class AgentBridgeDriverControls(unittest.TestCase):
    """Real CLI parser/command owner with counted compiler/environment ports.

    Temporary config/executable markers are dependency ports, never a native
    SDK/build witness. The optional old-wire mode fails these same assertions.
    """

    def setUp(self):
        import inspect
        self.driver = BuildDriverTests.driver(self, "jce_agent_bridge_driver_control")
        self.owner_source = inspect.getsource(self.driver.cmd_agent_bridge)
        self.temporary = tempfile.TemporaryDirectory(prefix="jce-agent-bridge-control-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.sdk = self.root / "selected SDK"
        self.env = {"PATH": "counted compiler environment"}
        self.commands = []
        root_patch = patch.object(self.driver, "ROOT", self.root)
        host_patch = patch.object(self.driver, "HOST", "windows")
        root_patch.start(); host_patch.start()
        self.addCleanup(root_patch.stop); self.addCleanup(host_patch.stop)
        self.environment = Mock(return_value=self.env)
        self.runner = Mock(side_effect=self.record_command)
        env_patch = patch.object(self.driver, "msvc_env", self.environment)
        run_patch = patch.object(self.driver, "run", self.runner)
        env_patch.start(); run_patch.start()
        self.addCleanup(env_patch.stop); self.addCleanup(run_patch.stop)
        self.old_mode = os.environ.get("JCE_TEST_AGENT_BRIDGE_OLD_PROJECT_FLAGS") == "1"

    def record_command(self, command, **kwargs):
        self.commands.append((command, kwargs))
        if not self.driver.DRY_RUN and command[:2] == ["cmake", "--build"]:
            self.exe.parent.mkdir(parents=True, exist_ok=True)
            self.exe.write_bytes(b"counted build output; not an executable")

    def restore_old_project_flags(self):
        # Mutate only the complete actual command function; retain parser,
        # SDK refusal, command order and the rest of the real driver.
        anchor = "*_delivery_module().cmake_flags(_delivery_module().profile(args))"
        self.assertEqual(self.owner_source.count(anchor), 1)
        old_wire = ('\n           "-DJCE_REQUIRED_SCRIPT_LANGUAGES="+";".join(required_languages),'
                    '\n           "-DJCE_REQUIRED_SCRIPT_TARGET="+(args.target or manifest.get("target") or project.name)')
        source = self.owner_source.replace(anchor, anchor + "," + old_wire)
        exec(compile(source, self.driver.__file__, "exec"), self.driver.__dict__)

    def observe(self, *, dry_run, variant="release", force_old=False):
        if force_old or self.old_mode:
            self.restore_old_project_flags()
        argv = (["--dry-run"] if dry_run else []) + [
            "agent-bridge", "--arch", "x64", "--sdk", str(self.sdk), "--variant", variant]
        args = self.driver.build_parser().parse_args(argv)
        self.assertFalse(hasattr(args, "target"))
        self.assertFalse(hasattr(args, "project"))
        self.driver.DRY_RUN = args.dry_run
        target = self.driver.resolve_target(args.arch)
        self.directory, self.exe = self.driver.agent_bridge_paths(target, variant)
        error = None
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            try:
                args.func(args)
            except BaseException as failure:
                error = failure
        return error, target

    def assert_dispatch(self, *, dry_run, variant="release", force_old=False):
        error, target = self.observe(dry_run=dry_run, variant=variant, force_old=force_old)
        self.assertIsNone(error, f"Agent bridge dispatch raised {type(error).__name__}: {error}")
        self.environment.assert_called_with(target)
        self.assertEqual(self.runner.call_count, 2)
        configure, build = self.commands
        command, _ = configure
        self.assertEqual(command[:3], ["cmake", "-S", str(self.root / "tools/jce_agent_bridge")])
        self.assertEqual(command[command.index("-B") + 1], str(self.directory))
        self.assertIn("-DJCE_DIR=" + str(self.sdk / "lib/cmake/JCE"), command)
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", command)
        self.assertIn("-DJCE_DELIVERY_PROFILE=embedded", command)
        self.assertFalse(any(value.startswith(("-DJCE_REQUIRED_SCRIPT_LANGUAGES=",
            "-DJCE_REQUIRED_SCRIPT_TARGET=")) for value in command))
        self.assertEqual(build[0], ["cmake", "--build", str(self.directory)])
        for _, kwargs in (configure, build):
            self.assertIs(kwargs["env"], self.env)
            self.assertEqual(kwargs["cwd"], self.root)

    def test_real_parser_dry_run_needs_no_project_or_sdk_files(self):
        for variant in ("release", "dist"):
            with self.subTest(variant=variant):
                self.commands.clear(); self.runner.reset_mock(); self.environment.reset_mock()
                self.assert_dispatch(dry_run=True, variant=variant)
                self.environment.assert_called_once()
                self.assertFalse(self.directory.exists())

    def test_actual_sdk_path_reaches_counted_configure_and_build(self):
        config = self.sdk / "lib/cmake/JCE/JCEConfig.cmake"
        config.parent.mkdir(parents=True)
        config.write_text("counted SDK config marker", encoding="utf-8")
        self.assert_dispatch(dry_run=False)
        self.assertTrue(self.exe.is_file())
        self.environment.assert_called_once()

    def test_missing_sdk_refuses_before_environment_or_commands(self):
        error, _ = self.observe(dry_run=False)
        self.assertIsInstance(error, SystemExit)
        self.assertNotEqual(error.code, 0)
        self.environment.assert_not_called()
        self.runner.assert_not_called()

    def test_exact_old_project_flags_fail_ordinary_dispatch_assertions(self):
        with self.assertRaisesRegex(AssertionError, "NameError.*required_languages"):
            self.assert_dispatch(dry_run=True, force_old=True)
        self.environment.assert_called_once()
        self.runner.assert_not_called()


if __name__ == "__main__":
    unittest.main()
