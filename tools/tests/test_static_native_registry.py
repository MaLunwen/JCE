"""Generated native registry CPU controls; not installed-SDK delivery proof."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.build import native_registry
CMAKE = shutil.which('cmake')
CC = shutil.which('gcc') or shutil.which('clang')


@unittest.skipUnless(CMAKE and CC, 'CMake/native C compiler subject unavailable')
class StaticRegistryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'build')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)

    def configure(self, factories='fixture_provider', required='fixture_provider',
                  target='probe', paths='scripts/Fixture.jcec', languages='c cpp', sha='a' * 64,
                  env_contract=False, registry_args=None, deferred=False,
                  deferred_languages='c', declaration_dir=False, declare=True,
                  deferred_factories=None, old_deferred=False):
        source = self.base / 'source'
        source.mkdir(exist_ok=True)
        (source / 'empty.c').write_text('int fixture;\n')
        text = f"""cmake_minimum_required(VERSION 3.20)
project(registry_probe LANGUAGES C)
add_library(probe STATIC empty.c)
add_library(jce_script_vm_c INTERFACE)
add_library(jce_script_vm_cpp INTERFACE)
add_library(jce_script_api INTERFACE)
set(JCE_REQUIRED_SCRIPT_TARGET [==[{target}]==])
set(JCE_REQUIRED_NATIVE_SCRIPT_PATHS [==[{paths}]==])
set(JCE_REQUIRED_NATIVE_FACTORIES [==[{required}]==])
set(JCE_NATIVE_SCRIPT_CLOSURE_SHA256 [==[{sha}]==])
include([==[{(ROOT / 'scripting/cmake/JCEScriptEnable.cmake').as_posix()}]==])
"""
        if deferred:
            template = (ROOT / 'cmake/JCEConfig.cmake.in').read_text(encoding='utf-8')
            tail = template[template.index('# Shipping admission is checked'):]
            if old_deferred:
                current = 'LANGUAGES ${_jce_delivery_languages} REQUIRED NO_STAGE ${_jce_factory_args})'
                self.assertEqual(tail.count(current), 1, 'deferred owner mutation must be unique')
                tail = tail.replace(current,
                                    'LANGUAGES ${JCE_REQUIRED_SCRIPT_LANGUAGES} REQUIRED NO_STAGE)')
            text += ('set(JCE_BUILD_VARIANT dist)\n'
                     'set(JCE_DELIVERY_PROFILE embedded)\nset(JCE_SDK_DELIVERY_PROFILE embedded)\n'
                     'set(JCE_SCRIPT_LANGUAGES c cpp)\n'
                     f'set(JCE_REQUIRED_SCRIPT_LANGUAGES {deferred_languages})\n' + tail + '\n')
        declaration = f'jce_script_enable(probe LANGUAGES {languages} NO_STAGE NATIVE_FACTORIES {factories})\n'
        if declare:
            if declaration_dir:
                declared = source / 'declared'
                declared.mkdir(exist_ok=True)
                (declared / 'CMakeLists.txt').write_text(declaration, encoding='utf-8', newline='\n')
                text += 'add_subdirectory(declared)\n'
            else:
                text += declaration
        if deferred_factories is not None:
            text += f'set(JCE_REQUIRED_NATIVE_FACTORIES [==[{deferred_factories}]==])\n'
        if deferred:
            # This runs after the real SDK deferred hook. Target properties and
            # the real SOURCES list, not a substitute enable port, are witnesses.
            text += """
function(registry_target_witness)
    get_target_property(sources probe SOURCES)
    set(registry_count 0)
    foreach(item IN LISTS sources)
        if(item MATCHES "/jce_script_register_linked[.]c$")
            math(EXPR registry_count "${registry_count} + 1")
        endif()
    endforeach()
    get_property(languages TARGET probe PROPERTY _JCE_SCRIPT_ENABLED_LANGUAGES)
    get_property(factories TARGET probe PROPERTY _JCE_SCRIPT_NATIVE_FACTORIES)
    get_property(shim TARGET probe PROPERTY _JCE_SCRIPT_SHIM_DIR)
    file(WRITE "${CMAKE_BINARY_DIR}/registry-witness.txt"
         "${registry_count}\n${languages}\n${factories}\n${shim}\n")
endfunction()
cmake_language(DEFER CALL registry_target_witness)
"""
        env = dict(os.environ)
        if env_contract:
            text = '\n'.join(line for line in text.splitlines() if not line.startswith('set(JCE_REQUIRED_') and not line.startswith('set(JCE_NATIVE_SCRIPT_')) + '\n'
            env.update(JCE_REQUIRED_SCRIPT_TARGET=target,
                       JCE_REQUIRED_NATIVE_SCRIPT_PATHS=paths,
                       JCE_REQUIRED_NATIVE_FACTORIES=required,
                       JCE_NATIVE_SCRIPT_CLOSURE_SHA256=sha)
        extra = []
        if registry_args is not None:
            text = '\n'.join(line for line in text.splitlines() if not line.startswith('set(JCE_REQUIRED_') and not line.startswith('set(JCE_NATIVE_SCRIPT_')) + '\n'
            for key in ('JCE_REQUIRED_SCRIPT_TARGET', 'JCE_REQUIRED_NATIVE_FACTORIES',
                        'JCE_REQUIRED_NATIVE_SCRIPT_PATHS', 'JCE_NATIVE_SCRIPT_CLOSURE_SHA256'):
                env.pop(key, None)
                extra.extend(['-U', key])
            extra.extend(registry_args)
        (source / 'CMakeLists.txt').write_text(text)
        result = subprocess.run([CMAKE, '-S', str(source), '-B', str(self.base / 'configured'),
                                 '-G', 'Ninja', '-DCMAKE_C_COMPILER=' + Path(CC).as_posix(), *extra],
                                capture_output=True, text=True, timeout=45, env=env)
        generated = self.base / 'configured'
        if declare and declaration_dir:
            generated /= 'declared'
        return result, generated / 'jce_script_enable/probe'

    def compile_probe(self, generated, omit_provider=False):
        include = self.base / 'headers'
        headers = {
            'jce/middleware/script/jce_script_vm.h':
                '#include <stdbool.h>\nint jce_script_vm_count(void); const char *jce_script_vm_language_at(int);'
                'int jce_script_vm_extension_count(void); const char *jce_script_vm_extension_at(int);'
                'const char *jce_script_vm_extension_language_at(int);const char *jce_script_vm_language_for_path(const char*);',
            'jce/os/core/jce_filesystem.h':
                '#include <stdbool.h>\n#include <stdint.h>\nbool jce_fs_host_get_base_path(char*,uint32_t);'
                'bool jce_fs_host_write_all(const char*,const void*,uint64_t);',
            'jce/os/core/jce_log.h': '#define LOG_INFO(...) ((void)0)\n#define LOG_ERROR(...) ((void)0)\n',
            'jce/script_vm/jce_script_vm_cpp.h':
                '#ifndef TEST_CPP_H\n#define TEST_CPP_H\n#include <stdbool.h>\n'
                'typedef struct JceCppModuleDesc {int value;} JceCppModuleDesc;'
                'bool jce_script_vm_cpp_register(void);bool jce_script_vm_c_register(void);'
                'void *jce_script_vm_cpp_add_module(const JceCppModuleDesc*);'
                'bool jce_script_vm_cpp_has_class(const char*);\n#endif\n',
            'jce/script_vm/jce_script_vm_c.h': '#include <jce/script_vm/jce_script_vm_cpp.h>\n',
        }
        for name, content in headers.items():
            path = include / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        harness = self.base / 'harness.c'
        harness.write_text("""#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <jce/script_vm/jce_script_vm_cpp.h>
#include "jce_script_register_linked.h"
static int added;
bool jce_script_vm_cpp_register(void){return true;}
bool jce_script_vm_c_register(void){return true;}
void *jce_script_vm_cpp_add_module(const JceCppModuleDesc *d){if(!d||getenv("FAIL_FACTORY"))return NULL;added++;return &added;}
bool jce_script_vm_cpp_has_class(const char *s){return added==1&&!getenv("FAIL_CLASS")&&strcmp(s,"scripts/Fixture.jcec")==0;}
const char *jce_script_vm_language_for_path(const char *s){(void)s;return "c";}
int jce_script_vm_count(void){return 0;}
const char *jce_script_vm_language_at(int x){(void)x;return "c";}
int jce_script_vm_extension_count(void){return 0;}
const char *jce_script_vm_extension_at(int x){(void)x;return "jcec";}
const char *jce_script_vm_extension_language_at(int x){(void)x;return "c";}
bool jce_fs_host_get_base_path(char *p,uint32_t n){(void)n;p[0]=0;return false;}
bool jce_fs_host_write_all(const char *p,const void *b,uint64_t n){FILE*f=fopen(p,"wb");if(!f)return false;int ok=fwrite(b,1,(size_t)n,f)==n;fclose(f);return ok;}
int main(void){return jce_script_register_linked_languages(NULL)<0?3:0;}
""" + ('' if omit_provider else 'const JceCppModuleDesc *fixture_provider(void){static const JceCppModuleDesc d={1};return &d;}\n'))
        exe = self.base / ('registry.exe' if os.name == 'nt' else 'registry')
        result = subprocess.run([CC, '-std=c99', '-I' + str(include), '-I' + str(generated),
                                 str(generated / 'jce_script_register_linked.c'), str(harness), '-o', str(exe)],
                                capture_output=True, text=True, timeout=30)
        return result, exe

    def test_real_generated_c_registry_reports_actual_resolution(self):
        result, generated = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result, exe = self.compile_probe(generated)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = self.base / 'report.json'
        env = dict(os.environ, JCE_NATIVE_REGISTRY_REPORT=str(report), JCE_NATIVE_REGISTRY_NONCE='b' * 64)
        for failure in (None, 'FAIL_FACTORY', 'FAIL_CLASS'):
            current = dict(env)
            if failure:
                current[failure] = '1'
            result = subprocess.run([str(exe)], env=current, capture_output=True, timeout=10)
            data = json.loads(report.read_text())
            self.assertEqual(result.returncode, 0 if failure is None else 3)
            self.assertEqual(data['ok'], failure is None)
            self.assertEqual(data['closure_sha256'], 'a' * 64)
            self.assertEqual(data['nonce'], 'b' * 64)
            self.assertEqual(data['selected_count'], 1)
            self.assertEqual(data['factory_count'], 1)
            self.assertEqual(data['registered_factory_count'], 0 if failure == 'FAIL_FACTORY' else 1)
            self.assertEqual(data['resolved_count'], 1 if failure is None else 0)
        env['JCE_NATIVE_REGISTRY_NONCE'] = 'invalid'
        self.assertEqual(subprocess.run([str(exe)], env=env, timeout=10).returncode, 3)

    def test_missing_factory_is_real_link_failure(self):
        result, generated = self.configure()
        self.assertEqual(result.returncode, 0, result.stderr)
        result, _ = self.compile_probe(generated, omit_provider=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('fixture_provider', result.stderr)

    def test_configure_rejects_missing_duplicate_invalid_and_unavailable(self):
        for options in ({'factories': ''}, {'factories': 'fixture_provider fixture_provider'},
                        {'factories': 'bad-symbol'}, {'paths': '../Fixture.jcec'},
                        {'sha': 'invalid'}, {'languages': 'cpp'}, {'required': 'other_provider'}, {'paths': ''}):
            with self.subTest(options=options):
                result, _ = self.configure(**options)
                self.assertNotEqual(result.returncode, 0, result.stdout)

    def test_headless_environment_contract_generates_same_registry(self):
        result, generated = self.configure(env_contract=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result, exe = self.compile_probe(generated)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(subprocess.run([str(exe)], timeout=10).returncode, 0)

    def test_actual_planner_to_cmake_to_generated_receipt_contract(self):
        manifest = {'target': 'stale_manifest_target', 'required_languages': ['c'],
                    'native_script_factories': ['fixture_provider'],
                    'native_script_refs': {'scripts/fixture.jcec': 'scripts/Fixture.jcec'}}
        typed = [{'path': 'scripts/fixture.jcec', 'asset_id': '0000000000000001',
                  'content_id': '0000000000000002', 'size': 1, 'dependencies': [],
                  'type': 'script', 'representation': 'c.class-ref'}]
        planned = native_registry.plan(manifest, typed, 'probe')
        actual_args = native_registry.cmake_args(planned)
        result, generated = self.configure(registry_args=actual_args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result, exe = self.compile_probe(generated)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = self.base / 'planner-receipt.json'
        nonce = 'c' * 64
        env = dict(os.environ, JCE_NATIVE_REGISTRY_REPORT=str(report), JCE_NATIVE_REGISTRY_NONCE=nonce)
        result = subprocess.run([str(exe)], env=env, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt = json.loads(report.read_text())
        self.assertEqual(receipt['target'], 'probe')
        self.assertEqual(receipt['closure_sha256'], planned['closure_sha256'])
        self.assertEqual(receipt['nonce'], nonce)
        self.assertEqual(receipt['selected_count'], len(planned['bindings']))
        self.assertEqual(receipt['resolved_count'], len(planned['bindings']))
        self.assertEqual(receipt['factory_count'], len(planned['factories']))
        self.assertEqual(receipt['registered_factory_count'], len(planned['factories']))
        self.assertIs(receipt['ok'], True)
        # Reproduce the old cross-module spelling mismatch. Required factories
        # without class paths must fail, not attest an empty selected closure.
        wrong_args = [value.replace('JCE_REQUIRED_NATIVE_SCRIPT_PATHS=',
                                    'JCE_NATIVE_SCRIPT_PATHS=') for value in actual_args]
        result, _ = self.configure(registry_args=wrong_args)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('missing class paths', result.stderr)

    def test_uppercase_native_extensions_keep_authored_class_case(self):
        authored = 'Scripts/FiXtUrE.JCEC'
        result, generated = self.configure(paths=authored)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        text = (generated / 'jce_script_register_linked.c').read_text()
        encoded = ''.join('\\%03o' % byte for byte in authored.encode('utf-8'))
        normalized = ''.join('\\%03o' % byte for byte in authored.lower().encode('utf-8'))
        self.assertIn('jce_script_vm_cpp_has_class("' + encoded + '")', text)
        self.assertNotIn('jce_script_vm_cpp_has_class("' + normalized + '")', text)
        result, _ = self.configure(paths='Scripts/Other.JCECPP')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_requirements_only_apply_to_selected_target(self):
        result, generated = self.configure(target='other_target', factories='', required='', sha='invalid')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        text = (generated / 'jce_script_register_linked.c').read_text()
        self.assertNotIn('jce_script_vm_cpp_has_class("', text)

    def test_actual_sdk_deferred_hook_keeps_factory_and_one_registry_source(self):
        result, generated = self.configure(deferred=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        witness = (self.base / 'configured/registry-witness.txt').read_text().splitlines()
        self.assertEqual(witness[:3], ['1', 'c;cpp', 'fixture_provider'])
        self.assertEqual(Path(witness[3]), generated)
        text = (generated / 'jce_script_register_linked.c').read_text()
        self.assertIn('extern const JceCppModuleDesc *fixture_provider(void);', text)
        result, exe = self.compile_probe(generated)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(subprocess.run([str(exe)], timeout=10).returncode, 0)

    def test_actual_sdk_deferred_hook_merges_languages_without_moving_child_registry(self):
        result, generated = self.configure(deferred=True, languages='c',
                                           deferred_languages='cpp', declaration_dir=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        witness = (self.base / 'configured/registry-witness.txt').read_text().splitlines()
        self.assertEqual(witness[:3], ['1', 'c;cpp', 'fixture_provider'])
        self.assertEqual(Path(witness[3]), generated)
        self.assertFalse((self.base / 'configured/jce_script_enable/probe').exists())
        self.assertIn('JCE_SCRIPT_LINKED_CPP', (generated / 'jce_script_register_linked.c').read_text())

    def test_actual_sdk_deferred_hook_cannot_synthesize_missing_factory_declaration(self):
        result, _ = self.configure(deferred=True, declare=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Selected native scripts require explicit factories', result.stderr)
        self.assertFalse((self.base / 'configured/registry-witness.txt').exists())

    def test_actual_sdk_deferred_hook_revalidates_factory_mismatch(self):
        result, _ = self.configure(deferred=True, deferred_factories='other_provider')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Selected native factories do not match target declaration', result.stderr)
        self.assertFalse((self.base / 'configured/registry-witness.txt').exists())

    def test_old_deferred_wire_has_ordinary_missing_factory_failure(self):
        result, _ = self.configure(deferred=True, old_deferred=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Selected native scripts require explicit factories', result.stderr)
        self.assertFalse((self.base / 'configured/registry-witness.txt').exists())


if __name__ == '__main__':
    if not CMAKE or not CC:
        print('SKIP: native compiler/CMake subject unavailable (exit 77)')
        raise SystemExit(77)
    unittest.main()
