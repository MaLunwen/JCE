"""Bundle command CPU controls: public options, SDK availability and failures."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


bundle = load("bundle_command", ROOT/"tools/build/bundle.py")


def arguments(**overrides):
    values = dict(project=None, scenes_dir=None, resource_root=None, out=None,
        scene=None, target="auto", cook_assets=False, single_file=False,
        quiet=False, id=None, sdk=None)
    values.update(overrides)
    return SimpleNamespace(**values)


class BundleControls(unittest.TestCase):
    def test_private_cli_bridge_does_not_initialize_engine(self):
        bridge = (ROOT/"tools/cli/jce_tool_main_sdl.c").read_text()
        tool = (ROOT/"tools/jce_bundle_pack.c").read_text()
        cmake = (ROOT/"CMakeLists.txt").read_text()
        self.assertIn("#include <SDL3/SDL_main.h>", bridge)
        self.assertIn("return jce_tool_main(argc, argv);", bridge)
        for forbidden in ("SDL_Init(", "jce_engine_", "SDL_MAIN_USE_CALLBACKS", "_WIN32"):
            self.assertNotIn(forbidden, bridge)
        self.assertNotIn("SDL3/", tool)
        self.assertIn("int jce_tool_main(int argc, char **argv)", tool)
        self.assertIn("add_library(jce_tool_cli_bootstrap OBJECT", cmake)
        self.assertIn("jce_core jce_platform jce_tool_cli_bootstrap sdl::sdl", cmake)
        # Shared UTF-8 entry and path I/O are migrated together.
        for name in ("jce_pak.c", "jce_cook.c", "jce_bin2obj.c"):
            source = (ROOT/"tools"/name).read_text()
            self.assertIn("int jce_tool_main(", source)
            self.assertNotIn("FindFirstFileA", source)
            self.assertNotIn("FILE *f = fopen", source)

    def test_selected_scene_options(self):
        with tempfile.TemporaryDirectory() as temporary:
            scene = Path(temporary)/"a.scene.json"
            scene.write_text("{}")
            cmd = bundle.command(arguments(project=temporary, scene=[str(scene)],
                single_file=True, id="sample", cook_assets=True, target="android"), "packer")
            self.assertEqual(cmd[0], "packer")
            self.assertIn(str(scene.resolve()), cmd)
            self.assertIn("--cook-assets", cmd)
            self.assertEqual(cmd[cmd.index("--target")+1], "android")
            wrong = Path(temporary)/"not-a-scene.txt"
            wrong.write_text("{}")
            with self.assertRaisesRegex(ValueError, "selected scene unavailable"):
                bundle.command(arguments(project=temporary, scene=[str(wrong)]), "packer")

    def test_invalid_options_do_not_build(self):
        driver = SimpleNamespace(HOST="windows", ROOT=ROOT,
            ensure_host_tools=Mock(), run=Mock())
        for args in (arguments(), arguments(project="root", single_file=True),
                     arguments(project="root", id="invalid"),
                     arguments(project="root", scene=["missing.scene.json"])):
            with self.assertRaises(ValueError):
                bundle.execute(args, driver)
        driver.ensure_host_tools.assert_not_called()
        driver.run.assert_not_called()

    def test_sdk_missing_and_pack_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            driver = SimpleNamespace(HOST="windows", ROOT=ROOT,
                ensure_host_tools=Mock(), run=Mock())
            args = arguments(project=temporary, sdk=temporary)
            with self.assertRaisesRegex(ValueError, "SDK bundle tool unavailable"):
                bundle.execute(args, driver)
            exe = Path(temporary)/"bin/jce_bundle_pack.exe"
            exe.parent.mkdir(); exe.write_bytes(b"mock")
            driver.run.side_effect = RuntimeError("packer failed")
            with self.assertRaisesRegex(RuntimeError, "packer failed"):
                bundle.execute(args, driver)
            driver.ensure_host_tools.assert_not_called()
            self.assertEqual(driver.run.call_args.args[0][0], str(exe.resolve()))

    def test_host_cache_and_parser(self):
        driver = SimpleNamespace(HOST="linux", ROOT=ROOT,
            ensure_host_tools=Mock(return_value={"jce_bundle_pack": Path("host-tool")}), run=Mock())
        bundle.execute(arguments(project="root"), driver)
        driver.ensure_host_tools.assert_called_once_with({}, ("jce_bundle_pack",))
        actual = load("bundle_build_driver", ROOT/"tools/build/jce.py")
        parser = actual.build_parser()
        args = parser.parse_args(["bundle", "--project", "root", "--target", "web", "--cook-assets"])
        self.assertEqual(args.target, "web")
        self.assertTrue(args.cook_assets)
        with self.assertRaises(SystemExit):
            parser.parse_args(["bundle", "--project", "root", "--target", "typo"])



class HostBundleClosureTests(unittest.TestCase):
    def test_offline_targets_share_real_implementations_without_render_layers(self):
        root = (ROOT/"CMakeLists.txt").read_text(encoding="utf-8")
        shared = (ROOT/"cmake/JCEHostAssetSources.cmake").read_text(encoding="utf-8")
        self.assertIn("jce_host_asset_sources(jce_cook)", root)
        self.assertIn("jce_host_asset_sources(jce_bundle_pack)", root)
        begin = root.index("add_executable(jce_bundle_pack ")
        end = root.index("set_target_properties(jce_bundle_pack", begin)
        closure = root[begin:end]
        for layer in ("jce_resource", "jce_renderer", "jce_scene", "jce_audio"):
            self.assertNotIn(layer, closure)
        for source in ("jce_asset_cooker.c", "jce_bundle_mesh_convert.cpp",
                       "jce_image_decode.c", "jce_tex_encode.cpp"):
            self.assertIn(source, shared)
        for source in ("jce_bundle_pack.c", "jce_bundle_pack_cook.c",
                       "jce_bundle_contract_internal.c", "jce_asset_reader.c"):
            self.assertIn(source, closure)
        for dependency in ("assimp::assimp", "bgfx::bimg_encode", "miniaudio::miniaudio"):
            self.assertIn(dependency, shared)
        self.assertNotIn("bgfx::bgfx", shared)
        self.assertNotIn("jce_vendor_source(", shared)
        self.assertNotIn("JCE_STB_IMAGE_SOURCE_DIR", shared)
        late = root.index("foreach(_host_asset_target jce_cook jce_bundle_pack)")
        self.assertGreater(late, root.index("add_subdirectory(engine)"))
        admission = root[late:root.index("endforeach()", late)]
        self.assertIn('target_include_directories(${_host_asset_target} PRIVATE "${JCE_STB_IMAGE_SOURCE_DIR}")', admission)
        self.assertIn("add_dependencies(${_host_asset_target} jce_vendor_verify_stb_image)", admission)
        self.assertIn("cgltf::cgltf", shared)

    def test_cook_only_excludes_gpu_definitions_and_retains_cpu_paths(self):
        importer = (ROOT/"engine/src/resource/jce_model_importer.cpp").read_text(encoding="utf-8")
        # Select actual guarded source regions, rather than asserting a macro
        # exists somewhere unrelated to the GPU definitions.
        guard = "#ifndef JCE_MODEL_IMPORTER_COOK_ONLY"
        end = "#endif /* !JCE_MODEL_IMPORTER_COOK_ONLY */"
        blocks = []
        cursor = 0
        while True:
            begin = importer.find(guard, cursor)
            if begin < 0:
                break
            finish = importer.find(end, begin)
            if finish < 0:
                break
            blocks.append(importer[begin:finish])
            cursor = finish+len(end)
        for definition in ("static JceMesh *convert_all_meshes(",
                           "JceMesh *jce_model_importer_load_pak(",
                           "JceMesh *jce_model_importer_load_file("):
            self.assertTrue(any(definition in block for block in blocks), definition)
        for definition in ("bool jce_model_importer_load_parts_memory(",
                           "bool jce_model_importer_load_parts_file(",
                           "bool jce_model_importer_load_cpu_file(",
                           "bool jce_model_importer_extract_material(",
                           "bool jce_model_importer_inspect_file("):
            self.assertIn(definition, importer)
            self.assertFalse(any(definition in block for block in blocks), definition)
        cooker = (ROOT/"tools/jce_cook.c").read_text(encoding="utf-8")
        for definition in ("JceMesh *jce_mesh_create(", "const JcePakAsset *jce_pak_find(",
                           "size_t jce_pak_decompress(", "size_t jce_pak_decompress_ex("):
            self.assertNotIn(definition, cooker)
        shared = (ROOT/"cmake/JCEHostAssetSources.cmake").read_text(encoding="utf-8")
        for source in ("jce_pak_loader.c", "jce_archive_reader.c", "jce_archive_crypto.c"):
            self.assertIn(source, shared)
        self.assertIn("jce_core jce_platform", shared)

    def test_host_build_uses_shared_job_budget(self):
        from unittest.mock import patch
        driver = load("host_bundle_jobs", ROOT/"tools/build/jce.py")
        commands = []
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            toolchain = root/"build/host-conan/build/Release/generators/conan_toolchain.cmake"
            toolchain.parent.mkdir(parents=True)
            toolchain.write_text("", encoding="utf-8")
            with patch.object(driver, "ROOT", root), patch.object(driver, "host_tool", return_value=root/"tool"), \
                    patch.object(driver, "resolve_target", return_value={}), \
                    patch.object(driver, "msvc_env", return_value={"CMAKE_BUILD_PARALLEL_LEVEL": "2"}), \
                    patch.object(driver, "run", side_effect=lambda cmd, **kw: commands.append(cmd)):
                function = getattr(driver.ensure_host_tools, "__wrapped__", driver.ensure_host_tools)
                function({}, ("jce_bundle_pack",))
        build = next(command for command in commands if "--build" in command)
        self.assertEqual(build[build.index("-j")+1], "2")
        self.assertIn("jce_bundle_pack", build)

if __name__ == "__main__":
    unittest.main()
