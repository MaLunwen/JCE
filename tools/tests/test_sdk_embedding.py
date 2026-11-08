"""Exercise SDK embedding CMake recipes without a compiler or native build."""
import ast
import os
import struct
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SDKEmbeddingTests(unittest.TestCase):
    def test_incremental_cook_removes_only_generated_reserved_boot(self):
        result,trace=self.recipe(manifest=True,cook=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('app_cooked/jce/runtime_boot.json',trace)
        self.assertIn('-E;rm;-f;',trace)
        self.assertNotIn('-E;rm;-rf;',trace)
        # No generated project authority: leave ordinary cooked assets alone.
        result,trace=self.recipe(manifest=False,cook=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertNotIn('-E;rm;-f;',trace)

    def recipe(self, msvc=False, web=False, prebuilt=False, wrapper=True, manifest=False, remove_settings=False, cook=False, prebuilt_kind="ASM", missing_pak=False, repeat_web=False, installed=False, custom_root_position=None):
        cmake = shutil.which("cmake")
        ninja = shutil.which("ninja")
        if not cmake or not ninja:
            self.skipTest("CMake/Ninja required for compiler-free recipe test")
        scratch = ROOT / "build" / "sdk-embedding-tests"
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as tmp:
            root = Path(tmp)
            helper = ROOT / "cmake/JCESDKHelpers.cmake"
            if installed:
                sdk_cmake = root / "installed SDK with spaces/lib/cmake/JCE"
                sdk_cmake.mkdir(parents=True)
                for name in ("JCESDKHelpers.cmake", "JCESDKTarget.cmake"):
                    shutil.copyfile(ROOT / "cmake" / name, sdk_cmake / name)
                shutil.copyfile(ROOT / "engine/src/os/platform/jce_webgl_interop.js",
                                sdk_cmake / "jce_webgl_interop.js")
                helper = sdk_cmake / "JCESDKHelpers.cmake"
            (root / "resources").mkdir()
            (root / "resources" / "sample.txt").write_text("asset")
            if manifest:
                (root / "jce_project.json").write_text('{"startup_scene":"scenes/start.scene.json"}')
                (root / ".jce").mkdir()
                (root / ".jce/project-settings.json").write_text('{"graphics":{"color_space":0}}')
            (root / "fixture.jbundle").write_bytes(b"bundle")
            (root / "assets.S").write_text("/* prebuilt */\n")
            if not missing_pak:
                (root / "assets.pak").write_bytes(b"JPAK")
            custom_root = 'target_link_options(app PRIVATE "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$consumerStartup")'
            (root / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(EmbeddingRecipe LANGUAGES NONE)
add_custom_target(app)
set(MSVC {"ON" if msvc else "OFF"})
set(EMSCRIPTEN {"ON" if web else "OFF"})
set(CMAKE_C_COMPILER_ARCHITECTURE_ID x64)
set(JCE_PAK_EXECUTABLE fixture-packer)
set(JCE_COOK_EXECUTABLE fixture-cooker)
set(JCE_BIN2OBJ_EXECUTABLE {"fixture-wrapper" if wrapper else ""})
function(enable_language)
 file(APPEND "${{CMAKE_BINARY_DIR}}/trace.txt" "LANGUAGE|${{ARGV}}\\n")
endfunction()
include("{helper.as_posix()}")
function(jce_configure_application_target)
 file(APPEND "${{CMAKE_BINARY_DIR}}/trace.txt" "DIST_POLICY|${{ARGV}}\\n")
 _jce_configure_web_target(${{ARGV}})
endfunction()
foreach(command add_custom_command target_sources set_source_files_properties
                add_custom_target add_dependencies target_link_options)
 function(${{command}})
  file(APPEND "${{CMAKE_BINARY_DIR}}/trace.txt" "${{CMAKE_CURRENT_FUNCTION}}|${{ARGV}}\\n")
 endfunction()
endforeach()
{("set(JCE_PROJECT_PREBUILT_ASSETS_" + prebuilt_kind + " ${CMAKE_SOURCE_DIR}/assets." + ("pak" if prebuilt_kind == "PAK" else "S") + ")") if prebuilt else ""}
{custom_root if custom_root_position == 'before' else ''}
{"jce_add_pak" if cook else "jce_target_embed_pak"}(app RESOURCE_DIRS resources NO_ENGINE_RESOURCES)
{"" if web or prebuilt else "jce_target_embed_bundle(app BUNDLE fixture.jbundle SYMBOL bundle_fixture)"}
{"_jce_configure_web_target(app)" if repeat_web else ""}
{custom_root if custom_root_position == 'after' else ''}
''', encoding="utf-8", newline="\n")
            result = subprocess.run(
                [cmake, "-S", str(root), "-B", str(root / "out"), "-G", "Ninja"],
                capture_output=True, text=True)
            if remove_settings and result.returncode == 0:
                (root / ".jce/project-settings.json").unlink()
                result = subprocess.run(
                    [cmake, "-S", str(root), "-B", str(root / "out"), "-G", "Ninja"],
                    capture_output=True, text=True)
            trace = root / "out" / "trace.txt"
            text = trace.read_text() if trace.exists() else ""
            boot = root / "out/app_runtime_boot/jce/runtime_boot.json"
            if manifest and boot.is_file():
                text += "\nBOOT|" + boot.read_text()
                settings = root / "out/app_runtime_boot/settings/project-settings.json"
                if settings.is_file():
                    text += "\nSETTINGS|" + settings.read_text()
            return result, text

    def test_prebuilt_embedding_keeps_application_policy(self):
        # Early prebuilt returns must still apply GUI/shipping target policy.
        for cook in (False, True):
            result, trace = self.recipe(prebuilt=True, cook=cook)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("DIST_POLICY|app", trace)

    def test_web_prebuilt_paths_keep_single_html_policy(self):
        # Execute the actual shared Web policy before the prebuilt early return.
        for cook in (False, True):
            result, trace = self.recipe(web=True, prebuilt=True, cook=cook, prebuilt_kind="PAK")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(trace.count('-sSINGLE_FILE=1'), 1)
            self.assertEqual(trace.count('-sSINGLE_FILE_BINARY_ENCODE=0'), 1)
            self.assertEqual(trace.count('--embed-file'), 1)
            self.assertIn('assets.pak@/game_assets.pak', trace)
            self.assertIn('wasm_pak_stub.c', trace)
            self.assertNotIn('LANGUAGE|ASM', trace)
            self.assertNotIn('COMMAND;fixture-wrapper', trace)
            self.assertNotIn('COMMAND;fixture-packer', trace)

    def test_web_library_preserves_consumer_roots_in_source_and_installed_helpers(self):
        consumer = "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$consumerStartup"
        for installed in (False, True):
            for prebuilt in (False, True):
                for position in ("before", "after"):
                    with self.subTest(installed=installed, prebuilt=prebuilt, position=position):
                        result, trace = self.recipe(web=True, installed=installed,
                            prebuilt=prebuilt, prebuilt_kind="PAK", repeat_web=True,
                            custom_root_position=position)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertEqual(trace.count(consumer), 1)
                        self.assertEqual(trace.count('-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE='), 1)
                        self.assertEqual(trace.count('--js-library'), 1)
                        self.assertNotIn('$jceWebTerminalAckBootstrap', trace)
                        self.assertNotIn('INCLUDE_FULL_LIBRARY', trace)
                        self.assertNotIn('--pre-js', trace)
                        self.assertIn('jce_webgl_interop.js', trace)
                        if installed:
                            self.assertIn('installed SDK with spaces/lib/cmake/JCE/jce_webgl_interop.js', trace)
        result, trace = self.recipe(repeat_web=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=', trace)
        self.assertNotIn('--js-library', trace)

    def test_prebuilt_web_pak_rejects_missing_archive_and_native_host(self):
        for web, missing in ((True, True), (False, False)):
            result, _ = self.recipe(web=web, prebuilt=True,
                                    prebuilt_kind="PAK", missing_pak=missing)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Prebuilt Web PAK requires', result.stderr)

    def test_native_pak_and_bundle_share_wrapper_and_dependencies(self):
        result, trace = self.recipe()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("LANGUAGE|ASM", trace)
        self.assertEqual(trace.count("COMMAND;fixture-wrapper;--input"), 2)
        self.assertEqual(trace.count("--format;asm"), 2)
        self.assertIn("--symbol;assets_pak_data", trace)
        self.assertIn("--symbol;bundle_fixture", trace)
        self.assertNotIn("asm-incbin", trace)
        self.assertNotIn("--obj-format", trace)
        self.assertIn("DEPENDS;fixture-wrapper;", trace)
        self.assertIn("app_assets.pak.S;", trace)

    def test_msvc_keeps_coff_arch_and_external_object(self):
        result, trace = self.recipe(msvc=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("LANGUAGE|ASM", trace)
        self.assertEqual(trace.count("--format;coff;--arch;x64"), 2)
        self.assertEqual(trace.count("EXTERNAL_OBJECT;TRUE"), 2)

    def test_web_embeds_archive_in_single_output_without_assembly(self):
        result, trace = self.recipe(web=True, wrapper=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("LANGUAGE|ASM", trace)
        self.assertNotIn("COMMAND;fixture-wrapper", trace)
        self.assertIn("--embed-file", trace)
        self.assertNotIn("--preload-file", trace)
        self.assertIn("-sSINGLE_FILE=1", trace)
        self.assertIn("-sSINGLE_FILE_BINARY_ENCODE=0", trace)

    def test_direct_helper_embeds_startup_contract(self):
        result, trace = self.recipe(web=True, manifest=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("app_runtime_boot", trace)
        self.assertIn("scenes/start.scene.json", trace)
        self.assertIn('SETTINGS|{"graphics":{"color_space":0}}', trace)

    def test_removed_project_settings_do_not_survive_reconfigure(self):
        result, trace = self.recipe(web=True, manifest=True, remove_settings=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("BOOT|", trace)
        self.assertNotIn("SETTINGS|", trace)

    def test_prebuilt_assembly_activates_language_without_host_wrapper(self):
        result, trace = self.recipe(prebuilt=True, wrapper=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("LANGUAGE|ASM", trace)
        self.assertIn("assets.S", trace)
        self.assertNotIn("COMMAND;fixture-packer", trace)

    def test_missing_wrapper_fails_instead_of_silent_missing_symbols(self):
        result, _ = self.recipe(wrapper=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Binary embedding requires", result.stderr)

    def test_assembly_template_has_target_prefix_sections_and_size_t(self):
        # This is a source/template contract, not a cross-platform link test.
        source = (ROOT / "tools/jce_bin2obj.c").read_text()
        begin = source.index("static void generate_asm_incbin(")
        end = source.index("/* ---------------- io helpers", begin)
        body = source[begin:end]
        literals = "\n".join(line for line in body.splitlines()
                             if line.lstrip().startswith('"'))
        template = "".join(ast.literal_eval(token) for token in
                           re.findall(r'"(?:[^"\\]|\\.)*"', literals))
        self.assertIn("JCE_JOIN(__USER_LABEL_PREFIX__, name)", template)
        self.assertIn(".section __TEXT,__const", template)
        self.assertIn(".section .rodata", template)
        self.assertIn("#if __SIZEOF_POINTER__ == 8\n    .quad", template)
        self.assertIn("#elif __SIZEOF_POINTER__ == 4\n    .long", template)
        self.assertIn("#error Unsupported native pointer width", template)
        self.assertIn(".note.GNU-stack", template)

    def test_explicit_host_tool_real_coff_asm_and_invalid_inputs(self):
        tool = os.environ.get("JCE_BIN2OBJ_TEST_EXECUTABLE")
        if not tool:
            self.skipTest("Set JCE_BIN2OBJ_TEST_EXECUTABLE for actual host tool evidence")
        self.assertTrue(Path(tool).is_file(), tool)
        scratch = ROOT / "build" / "sdk-embedding-tests"
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as tmp:
            root = Path(tmp)
            payload = bytes(range(37))
            blob = root / "asset with spaces.bin"
            blob.write_bytes(payload)
            def wrap(output, fmt, symbol="assets_pak_data", arch="x64", path=blob):
                return subprocess.run(
                    [tool, "--input", str(path), "--symbol", symbol,
                     "--output", str(output), "--format", fmt, "--arch", arch],
                    capture_output=True, text=True)
            for arch, machine, width, prefix in (
                    ("x86", 0x014c, 4, "_"), ("x64", 0x8664, 8, ""),
                    ("arm", 0x01c4, 4, ""), ("arm64", 0xaa64, 8, "")):
                output = root / (arch + ".obj")
                result = wrap(output, "coff", arch=arch)
                self.assertEqual(result.returncode, 0, result.stderr)
                data = output.read_bytes()
                self.assertEqual(struct.unpack_from("<H", data)[0], machine)
                symoff, count = struct.unpack_from("<II", data, 8)
                strings = symoff + count * 18
                actual = []
                for index in range(count):
                    entry = symoff + index * 18
                    offset = struct.unpack_from("<I", data, entry + 4)[0]
                    end = data.index(b"\0", strings + offset)
                    actual.append(data[strings + offset:end].decode())
                self.assertEqual(actual, [prefix + "assets_pak_data",
                                          prefix + "assets_pak_data_size"])
                self.assertEqual(data[60:60 + len(payload)], payload)
                size_offset = (len(payload) + width - 1) & ~(width - 1)
                self.assertEqual(int.from_bytes(data[60 + size_offset:
                                                      60 + size_offset + width],
                                                "little"), len(payload))
            output = root / "payload.S"
            result = wrap(output, "asm")
            self.assertEqual(result.returncode, 0, result.stderr)
            source = output.read_text()
            self.assertIn(blob.as_posix(), source)
            self.assertIn("JCE_SYMBOL(assets_pak_data_size)", source)
            self.assertIn(".quad 37", source)
            self.assertIn(".long 37", source)
            self.assertIn("__USER_LABEL_PREFIX__", source)
            for symbol in ("9bad", "bad-symbol", "x" * 64):
                bad = root / "invalid.obj"
                result = wrap(bad, "coff", symbol=symbol)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(bad.exists())


if __name__ == "__main__":
    unittest.main()
