"""Real isolated CMake API linkage/profile controls, not a full SDK build."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CMAKE = shutil.which('cmake')
CC = shutil.which('gcc') or shutil.which('clang')


@unittest.skipUnless(CMAKE and CC, 'native CMake/compiler subject unavailable')
class StaticScriptAPITests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'build')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        source = self.base / 'source'
        source.mkdir()
        (source / 'dummy.c').write_text('int main(void){return 0;}\n')
        text = f"""cmake_minimum_required(VERSION 3.20)
project(script_api_matrix LANGUAGES C)
set(JCE_ENABLE_SDK_INSTALL ON)
include([==[{(ROOT / 'cmake/JCEDeliveryProfile.cmake').as_posix()}]==])
function(jce_target_warnings)
endfunction()
# Exercise the actual scripting root's API selection; unrelated backends are
# intentionally excluded from this isolated configuration (no vendor graph).
function(add_subdirectory directory)
  if(directory STREQUAL "c_abi")
    _add_subdirectory([==[{(ROOT / 'scripting/c_abi').as_posix()}]==] api)
  elseif(directory STREQUAL [==[{(ROOT / 'scripting').as_posix()}]==])
    _add_subdirectory("${{directory}}" script_layer)
  endif()
endfunction()
add_subdirectory([==[{(ROOT / 'scripting').as_posix()}]==])
add_executable(probe dummy.c)
add_library(jce_script_vm_cpp INTERFACE)
add_library(jce_script_vm_c INTERFACE)
include([==[{(ROOT / 'scripting/cmake/JCEScriptEnable.cmake').as_posix()}]==])
jce_script_enable(probe LANGUAGES c cpp)
if(TARGET jce_script_api)
  get_target_property(_type jce_script_api TYPE)
  get_target_property(_sources jce_script_api SOURCES)
  get_target_property(_link jce_script_api LINK_OPTIONS)
  install(TARGETS jce_script_api RUNTIME DESTINATION lib LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
else()
  set(_type NONE)
endif()
file(WRITE "${{CMAKE_BINARY_DIR}}/type.txt" "${{_type}}\n${{_sources}}\n${{_link}}\n")
"""
        (source / 'CMakeLists.txt').write_text(text)

    def configure(self, profile, static, shared):
        # Remove only the previous generated configuration to reject stale proof.
        (self.base / 'configured/script_layer/JCEScripting.cmake').unlink(missing_ok=True)
        return subprocess.run([CMAKE, '-S', str(self.base / 'source'), '-B', str(self.base / 'configured'),
                               '-G', 'Ninja', '-DCMAKE_C_COMPILER=' + Path(CC).as_posix(),
                               '-DJCE_DELIVERY_PROFILE=' + profile,
                               '-DJCE_BUILD_SCRIPT_API_STATIC=' + static,
                               '-DJCE_BUILD_SCRIPT_API_SHARED=' + shared],
                              capture_output=True, text=True, timeout=45)

    def test_embedded_static_shared_authoring_and_off_switch(self):
        for profile, static, shared, expected, side_loader in (
                ('embedded', 'OFF', 'ON', 'STATIC_LIBRARY', False),
                ('authoring', 'OFF', 'ON', 'SHARED_LIBRARY', True),
                ('authoring', 'OFF', 'OFF', 'NONE', False),
                ('authoring', 'ON', 'OFF', 'STATIC_LIBRARY', False)):
            with self.subTest(profile=profile, static=static, shared=shared):
                result = self.configure(profile, static, shared)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                metadata = (self.base / 'configured/type.txt').read_text()
                self.assertEqual(metadata.splitlines()[0], expected)
                config = self.base / 'configured/script_layer/JCEScripting.cmake'
                if expected == 'NONE':
                    self.assertFalse(config.exists())
                else:
                    actual_config = config.read_text()
                    expected_marker = '1' if expected == 'STATIC_LIBRARY' else '0'
                    self.assertIn('set(JCE_SCRIPT_API_STATIC ' + expected_marker + ')', actual_config)
                    self.assertNotIn('@JCE_SDK_SCRIPT_API_STATIC@', actual_config)
                shim = (self.base / 'configured/jce_script_enable/probe/jce_script_register_linked.c').read_text()
                self.assertIn('#define JCE_SE_NEEDS_SCRIPT_API   ' + ('1' if side_loader else '0'), shim)
                ninja = (self.base / 'configured/build.ninja').read_text()
                if expected != 'NONE':
                    self.assertIn('jce_script_api.gen.c', metadata)
                    self.assertIn('jce_script_api_core.c', metadata)
                    install = (self.base / 'configured/cmake_install.cmake').read_text()
                    self.assertIn('jce_script_api', install)
                if expected == 'STATIC_LIBRARY':
                    self.assertNotIn('--version-script', metadata)
                    self.assertNotIn('copy_if_different', ninja)
                elif side_loader:
                    self.assertIn('copy_if_different', ninja)

    def test_conflicting_authoring_linkage_fails(self):
        result = self.configure('authoring', 'ON', 'ON')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Select exactly one', result.stderr)


if __name__ == '__main__':
    if not CMAKE or not CC:
        print('SKIP: CMake/native compiler subject unavailable (exit 77)')
        raise SystemExit(77)
    unittest.main()
