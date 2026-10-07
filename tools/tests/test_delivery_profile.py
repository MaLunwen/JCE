"""CPU delivery admission and shared static configure controls; no native build."""
import importlib.util
import json
import subprocess
import shutil
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT=Path(__file__).resolve().parents[2]


def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


policy=load('delivery_policy_test',ROOT/'tools/build/delivery.py')
standalone=load('delivery_recipe_test',ROOT/'tools/build/standalone.py')


class DeliveryControls(unittest.TestCase):
    def test_selected_closure_and_declared_required_languages(self):
        self.assertEqual(policy.admit({},['scenes/main.scene.json','scripts/main.lua'],'embedded',['lua']),['lua'])
        for manifest,addresses,available in (({'required_languages':['python']},[],['lua','python']),
                ({},['scripts/main.py'],['lua','python']),
                ({},['scripts/game.jcec'],['lua','c']),
                ({'required_languages':['cpp']},[],['lua','cpp']),
                ({'required_languages':['js']},[],['lua']),
                ({'required_languages':'lua'},[],['lua'])):
            with self.assertRaises(ValueError):policy.admit(manifest,addresses,'embedded',available)
        self.assertEqual(policy.admit({'required_languages':['python']},[], 'authoring',['python']),['python'])
        with self.assertRaises(ValueError):policy.admit({'required_languages':['python']},[],'authoring',['lua'])

    def test_official_script_extensions_and_typed_unknown_fail_closed(self):
        self.assertEqual(policy.admit({},['game.jcejs'],'embedded',['js']),['js'])
        self.assertEqual(policy.admit({},['authoring.js','assembly.dll'],'embedded',[]),[])
        for path in ('game.jcejs','game.class'):
            with self.assertRaises(ValueError):policy.admit({},[path],'embedded',[])
        typed={'path':'opaque.jceasset','type':'script','representation':'js.source'}
        self.assertEqual(policy.admit({},['opaque.jceasset'],'embedded',['js'],[typed]),['js'])
        for asset in ({**typed,'representation':'binary.raw'},
                      {**typed,'path':'game.lua'}):
            with self.assertRaises(ValueError):policy.admit({},[asset['path']],'embedded',['js','lua'],[asset])
        self.assertEqual(policy.admit({},['data.dll'],'embedded',[],[
            {'path':'data.dll','type':'binary','representation':'binary.raw'}]),[])

    def test_project_uses_public_graph_not_unused_authoring_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); sdk=root/'sdk'; project=root/'project'
            resources=project/'resources'; resources.mkdir(parents=True)
            (resources/'main.scene.json').write_text('{}')
            (resources/'unused.py').write_text('print("authoring only")')
            (sdk/'bin').mkdir(parents=True)
            (sdk/'bin/jce_bundle_pack.exe').touch()
            manifest={'startup_scene':'main.scene.json'}
            driver=SimpleNamespace(HOST='windows',ROOT=root,DRY_RUN=False,
                                   sdk_shipped_script_backends=lambda sdk:[],run=Mock())
            def resolve(command,**kwargs):
                output=Path(command[command.index('--out')+1])
                (output/'bundle_catalog.json').write_text(json.dumps({'bundles':{}}))
                (output/'bundle_graph.json').write_text(json.dumps({'assets':[{'address':'main.scene.json'}]}))
            driver.run.side_effect=resolve
            args=SimpleNamespace(delivery_profile='embedded',standalone=False)
            self.assertEqual(policy.admit_project(driver,args,project,manifest,sdk),[])
            self.assertIn('--single-file',driver.run.call_args.args[0])
            def unsupported(command,**kwargs):
                output=Path(command[command.index('--out')+1])
                (output/'bundle_catalog.json').write_text(json.dumps({'bundles':{}}))
                (output/'bundle_graph.json').write_text(json.dumps({'assets':[{'address':'selected.py'}]}))
            driver.run.side_effect=unsupported
            with self.assertRaises(ValueError):policy.admit_project(driver,args,project,manifest,sdk)
            (resources/'main.scene.json').unlink()
            driver.run.reset_mock()
            with self.assertRaises(ValueError):policy.admit_project(driver,args,project,manifest,sdk)
            driver.run.assert_not_called()

    def test_smoke_shared_api_profile_contract_preserves_authoring(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk=Path(directory); stamp=sdk/'delivery-profile.json'
            self.assertTrue(policy.smoke_requires_shared_api(sdk,['c'],True))
            stamp.write_text(json.dumps({'profile':'authoring'}))
            self.assertTrue(policy.smoke_requires_shared_api(sdk,['c'],True))
            stamp.write_text(json.dumps({'profile':'embedded','external_vm_allowed':False}))
            self.assertFalse(policy.smoke_requires_shared_api(sdk,['c','cpp','js'],True))
            with self.assertRaises(ValueError):policy.smoke_requires_shared_api(sdk,['python'],True)
            stamp.write_text(json.dumps({'profile':'embedded','external_vm_allowed':True}))
            with self.assertRaises(ValueError):policy.smoke_requires_shared_api(sdk,['c'],True)

    def test_embedded_sdk_lock_scope_contains_both_nested_smoke_resources(self):
        driver=load('delivery_lock_driver_test',ROOT/'tools/build/jce.py')
        args=SimpleNamespace(arch='x64',variant='dist',standalone=False,
                             delivery_profile='embedded',smoke=True)
        target=driver.resolve_target(args.arch)
        prefix=driver.sdk_install_dir(target,args.variant)
        prefix=prefix.with_name(prefix.name+'-single')
        resources=driver._standalone_sdk_cache_paths({'args':args})
        nested={'t':target,'sdk':prefix,'variant':args.variant}
        for scripting in (False,True):
            self.assertTrue(set(driver._smoke_cache_paths(nested,scripting)) <= set(resources))
        args.smoke=False
        self.assertNotIn(driver._smoke_cache_paths(nested)[0],
                         driver._standalone_sdk_cache_paths({'args':args}))

    def test_wasm_embedded_reuses_web_sdk_route_and_canonical_prefix(self):
        driver=load('delivery_web_driver_test',ROOT/'tools/build/jce.py')
        target={'host':driver.HOST,'arch':'wasm32','key':'wasm','emscripten':True}
        args=SimpleNamespace(arch='wasm',delivery_profile='embedded',standalone=False,
            variant='dist',clean=False,no_debug=True,smoke=False,sdk=None)
        with patch.object(driver,'resolve_target',return_value=target), \
             patch.object(driver,'_sdk_one') as sdk, \
             patch.object(driver,'_sdk_standalone') as native:
            driver.cmd_sdk(args)
        native.assert_not_called()
        self.assertEqual(sdk.call_args.args[-1],'embedded')
        self.assertEqual(driver.sdk_install_dir(target,'dist').name,'wasm-dist')

    def test_html_audit_inline_and_external_negative_controls(self):
        with tempfile.TemporaryDirectory() as directory:
            html=Path(directory)/'game.html'
            inline='<html><script>var wasmBinary="data:application/wasm;base64,AGFzbQ=="; var vpath="/game_assets.pak";</script></html>'
            html.write_text(inline)
            self.assertEqual(policy.audit_html(html)['offline_browser_relocation'],'NOT VERIFIED')
            for bad in (inline+'<script src="game.js"></script>',
                        inline+'<img src="texture.png">',
                        '<script>var wasmBinaryFile="game.wasm";</script>',
                        '<html><script>console.log(1)</script></html>'):
                html.write_text(bad)
                with self.assertRaises(ValueError):policy.audit_html(html)

    def test_shared_configure_static_flags_are_platform_scoped(self):
        for host in ('windows','linux','macos'):
            with tempfile.TemporaryDirectory() as directory:
                root=Path(directory)
                driver=SimpleNamespace(ROOT=root,HOST=host,DRY_RUN=True,
                    die=Mock(side_effect=ValueError),sync_conan_hooks=Mock(),
                    export_conan_recipes=Mock(),find_vs_install=Mock(return_value=None),
                    host_build_profile=lambda:'native',bgfx_graphics_conan_args=lambda env:[],codec_overrides=lambda args:[],run=Mock())
                target={'host':host,'cross':False,'key':host+'-x64','arch':'x64','profile':'native'}
                standalone._configure(driver,SimpleNamespace(variant='dist',clean=False),target,{})
                commands=[call.args[0] for call in driver.run.call_args_list]
                conan=next(command for command in commands if command[0]=='conan')
                self.assertIn('*:shared=False',conan)
                self.assertEqual('compiler.runtime=static' in conan,host=='windows')
                configure=next(command for command in commands if command[0]=='cmake')
                self.assertIn('-DJCE_DELIVERY_PROFILE=embedded',configure)
                self.assertIn('-DJCE_ENABLE_PROFILING=OFF',configure)
                self.assertIn('-DJCE_BUILD_SCRIPT_VM_PYTHON=OFF',configure)
                self.assertEqual(driver.find_vs_install.called,host=='windows')

    def test_actual_cmake_profile_forces_embedded_and_honors_authoring_choice(self):
        cmake=shutil.which('cmake')
        if not cmake:self.skipTest('CMake unavailable')
        root=ROOT/'build/tests/delivery-profile'; root.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as directory:
            script=Path(directory)/'policy.cmake'
            include=(ROOT/'cmake/JCEDeliveryProfile.cmake').as_posix()
            for value, java, expected in (('embedded','ON','OFF'),('authoring','OFF','OFF'),('typo','ON',None)):
                script.write_text('set(JCE_DELIVERY_PROFILE "'+value+'" CACHE STRING "")\n'+
                    'set(JCE_BUILD_SCRIPT_JAVA '+java+' CACHE BOOL "")\n'+
                    'include("'+include+'")\nmessage(STATUS "ACTUAL_JAVA=${JCE_BUILD_SCRIPT_JAVA}")\n')
                result=subprocess.run([cmake,'-P',str(script)],capture_output=True,text=True)
                if expected is None:self.assertNotEqual(result.returncode,0)
                else:
                    self.assertEqual(result.returncode,0,result.stderr)
                    self.assertIn('ACTUAL_JAVA='+expected,result.stdout)

    def test_actual_cmake_sdk_admission_and_deferred_target_link(self):
        cmake=shutil.which('cmake')
        if not cmake:self.skipTest('CMake unavailable')
        root=ROOT/'build/tests/delivery-profile'; root.mkdir(parents=True,exist_ok=True)
        template=(ROOT/'cmake/JCEConfig.cmake.in').read_text(encoding='utf-8')
        tail=template[template.index('# Shipping admission is checked'):].replace('@JCE_DELIVERY_PROFILE@','embedded')
        for available,target,expected in (('js',True,True),('',True,False),('js',False,False)):
            with tempfile.TemporaryDirectory(dir=root) as directory:
                directory=Path(directory)
                content=('cmake_minimum_required(VERSION 3.20)\nproject(DeliveryFixture NONE)\n'+
                    'set(JCE_DELIVERY_PROFILE embedded)\nset(JCE_SCRIPT_LANGUAGES "'+available+'")\n'+
                    'set(JCE_REQUIRED_SCRIPT_LANGUAGES js)\nset(JCE_REQUIRED_SCRIPT_TARGET consumer)\n'+
                    'function(jce_script_enable target)\nfile(WRITE "${CMAKE_BINARY_DIR}/linked.txt" "${target};${ARGN}")\nendfunction()\n'+tail+('\nadd_custom_target(consumer)\n' if target else ''))
                (directory/'CMakeLists.txt').write_text(content,encoding='utf-8')
                result=subprocess.run([cmake,'-S',str(directory),'-B',str(directory/'configured')],capture_output=True,text=True)
                self.assertEqual(result.returncode==0,expected,result.stderr)
                if expected:self.assertEqual((directory/'configured/linked.txt').read_text(),'consumer;LANGUAGES;js;REQUIRED;NO_STAGE')

    def test_authoring_flags_preserve_explicit_java_off(self):
        flags=policy.cmake_flags('authoring',java=False,restore_authoring=True)
        self.assertIn('-DJCE_BUILD_SCRIPT_JAVA=OFF',flags)
        self.assertIn('-DJCE_BUILD_SCRIPT_VM_PYTHON=ON',flags)
        self.assertEqual(policy.cmake_flags('authoring'),['-DJCE_DELIVERY_PROFILE=authoring'])
        self.assertEqual(policy.profile(SimpleNamespace(standalone=True,delivery_profile='authoring')),'embedded')
        with self.assertRaises(ValueError):policy.cmake_flags('typo')


if __name__=='__main__':unittest.main()
