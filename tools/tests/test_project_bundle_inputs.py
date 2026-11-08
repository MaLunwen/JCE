"""Project bundle forwarding and incremental cache controls; no native build."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bundle_delivery_test', ROOT / 'tools/build/delivery.py')
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class BundleInputs(unittest.TestCase):
    def setUp(self):
        base = ROOT / 'build/tests/project-bundle-inputs'
        base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=base)
        self.root = Path(self.temp.name).resolve()
        assert self.root.is_relative_to(base.resolve())
        self.bundle = self.root / 'one.jbundle'
        self.bundle.write_bytes(b'argument fixture, not a validated archive')

    def tearDown(self):
        self.temp.cleanup()

    def test_empty_project_clears_previous_bundle_cache(self):
        self.assertEqual(policy.bundle_cmake_args(self.root, {}),
                         ['-U', 'JCE_PROJECT_BUNDLES', '-U', 'JCE_PROJECT_PREBUILT_BUNDLE_DIR'])

    def test_relative_input_resolves_once(self):
        args = policy.bundle_cmake_args(self.root, {'bundles': ['one.jbundle']})
        self.assertEqual(args[-1], '-DJCE_PROJECT_BUNDLES=' + self.bundle.as_posix())

    def test_explicit_absolute_external_input(self):
        self.assertEqual(policy.bundle_cmake_args(self.root / 'project',
                         {'bundles': [str(self.bundle)]})[-1],
                         '-DJCE_PROJECT_BUNDLES=' + self.bundle.as_posix())

    def test_order_is_preserved(self):
        other = self.root / 'two.jbundle'
        other.write_bytes(b'argument fixture')
        args = policy.bundle_cmake_args(self.root, {'bundles': ['two.jbundle', 'one.jbundle']})
        self.assertTrue(args[-1].endswith(other.as_posix() + ';' + self.bundle.as_posix()))

    def test_malformed_and_missing_inputs_fail(self):
        for bundles in (None, 'one.jbundle', [False], [''], ['missing.jbundle'], ['one.pak'], ['bad\n.jbundle']):
            with self.subTest(bundles=bundles), self.assertRaises(ValueError):
                policy.bundle_cmake_args(self.root, {'bundles': bundles})

    def test_duplicate_resolved_input_fails(self):
        with self.assertRaises(ValueError):
            policy.bundle_cmake_args(self.root, {'bundles': ['one.jbundle', './one.jbundle']})

    def test_unicode_spaces_and_semicolon_are_one_cmake_list_item(self):
        path = self.root / 'space \u4e2d\u6587;directory' / 'asset.jbundle'
        path.parent.mkdir()
        path.write_bytes(b'argument fixture')
        args = policy.bundle_cmake_args(self.root, {'bundles': [str(path)]})
        self.assertEqual(args[-1], '-DJCE_PROJECT_BUNDLES=' + path.as_posix().replace(';', '\\;'))

    def test_supported_driver_uses_shared_argument_owner(self):
        source = (ROOT / 'tools/build/jce.py').read_text(encoding='utf-8')
        self.assertIn('*_delivery_module().bundle_cmake_args(project, manifest)', source)


class NativeArtifactNames(unittest.TestCase):
    def test_windows_template_and_explicit_extension(self):
        self.assertEqual(policy.native_artifact_name('Project', 'windows'), 'Project.exe')
        self.assertEqual(policy.native_artifact_name('Project.exe', 'windows'), 'Project.exe')
        self.assertEqual(policy.native_artifact_name('Project.EXE', 'windows'), 'Project.EXE')

    def test_non_windows_name_is_unchanged(self):
        for host in ('linux', 'macos'):
            self.assertEqual(policy.native_artifact_name('Project', host), 'Project')

    def test_invalid_names_fail(self):
        for name in ('', None, False):
            with self.subTest(name=name), self.assertRaises(ValueError):
                policy.native_artifact_name(name, 'windows')


class DeclaredBundleAdmission(unittest.TestCase):
    """Actual Python owners with counted public CLI receipts, not archive/crypto proof."""
    def setUp(self):
        import json
        import sys
        import hashlib
        from types import SimpleNamespace
        from unittest.mock import Mock
        self.json, self.hashlib, self.Mock = json, hashlib, Mock
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.project, self.sdk = self.root / 'project', self.root / 'sdk'
        self.project.mkdir()
        (self.sdk / 'bin').mkdir(parents=True)
        self.tool = self.sdk / 'bin/jce_pak.exe'
        self.tool.write_bytes(b'counted CLI port, not an executable')
        self.resolver = self.sdk / 'bin/jce_bundle_pack.exe'
        self.resolver.write_bytes(b'counted graph port')
        self.tools = {'jce_pak': self.tool, 'jce_bundle_pack': self.resolver}
        self.reports, self.commands = {}, []
        self.sha = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
        self.driver = SimpleNamespace(HOST='windows', ROOT=self.root, DRY_RUN=False,
            _sha256=self.sha, sdk_shipped_script_backends=lambda sdk: [], run=self.run_port)
        self.args = SimpleNamespace(delivery_profile='embedded', standalone=False,
            variant='dist', target=None)
        self.target = {'host':'windows', 'arch':'x86_64', 'eng_arch':'x64', 'key':'windows-x64'}

    @staticmethod
    def asset(path, identifier=1, kind='scene', representation='json.utf8'):
        return {'path':path, 'asset_id':'%016x' % identifier,
                'content_id':'%016x' % (identifier+100), 'size':3,
                'type':kind, 'representation':representation, 'dependencies':[]}

    def bundle(self, name, scene='scenes/main.scene.json', assets=None, sidecar=False):
        path = self.project / (name + '.jbundle')
        path.write_bytes(('public archive protocol fixture ' + name).encode())
        if sidecar:
            Path(str(path)+'.json').write_text('sidecar protocol fixture')
        assets = assets if assets is not None else [self.asset(scene)]
        metadata = {'contract':{'name':'jce.bundle','major':1,'minor':1}, 'id':name,
            'target_profile':'windows','scene_path':scene,'depends_on':[],'assets':assets}
        self.reports[str(path)] = {'schema':'jce.pakbom.v1','file':str(path),
            'file_size':path.stat().st_size,'header':{'header_verified':True},
            'totals':{'entries':len(assets)+1,'verify_ran':True,'verified_count':len(assets)+1,
                      'corrupt_count':0,'skipped_count':0},
            'bundle':{'verified':True,'manifest':metadata,'content_id':'%016x' % (len(self.reports)+1000),
                      'sidecar':{'present':sidecar,'verified':sidecar,'compatible':sidecar}}}
        return path

    def run_port(self, command, **kwargs):
        self.commands.append(command)
        if '--inspect' in command:
            path = command[command.index('--inspect')+1]
            output = Path(command[command.index('--json')+1])
            output.write_text(self.json.dumps(self.reports[path]))
            return
        if '--scene' in command:
            output = Path(command[command.index('--out')+1]);output.mkdir(parents=True,exist_ok=True)
            root = Path(command[command.index('--resource-root')+1])
            scene = Path(command[command.index('--scene')+1]).relative_to(root).as_posix()
            assets=[self.asset(scene,99)]
            blob=b'cooked graph protocol, not JPAK'
            (output/'admission.jbundle').write_bytes(blob)
            metadata={'contract':{'name':'jce.bundle','major':1,'minor':1},'id':'admission',
                      'scene_path':scene,'archive_size':len(blob),'content_hash':'0000000000000099',
                      'assets':assets}
            (output/'admission.jbundle.json').write_text(self.json.dumps(metadata))
            (output/'bundle_graph.json').write_text(self.json.dumps({'contract':{'name':'jce.bundle.graph','major':1},
                'assets':[dict(asset,address=asset['path']) for asset in assets]}))
            return
        pak=Path(command[command.index('--pak-file')+1]);pak.write_bytes(b'authenticated pack protocol, not crypto')
        sources,rows=[],{}
        for index, option in enumerate(command[:-1]):
            if option=='--selected-archive':
                path=command[index+1]
                if path in self.reports:
                    report=self.reports[path];assets=report['bundle']['manifest']['assets']
                    content_id=report['bundle']['content_id']
                else:
                    metadata=self.json.loads(Path(path+'.json').read_text())
                    assets=metadata['assets'];content_id=metadata['content_hash']
                sources.append({'path':path,'size':Path(path).stat().st_size,
                                'content_id':content_id,'imported_count':len(assets)})
                for asset in assets:
                    rows.setdefault(asset['path'],{'path':asset['path'],'source':path,'selected':True,
                        'size':asset['size'],'content_id':asset['content_id']})
            elif option=='--resource-dir':
                for file in Path(command[index+1]).rglob('*'):
                    if file.is_file():
                        path=file.relative_to(Path(command[index+1])).as_posix()
                        rows.setdefault(path,{'path':path,'source':str(file),'selected':False,
                            'size':file.stat().st_size,'content_id':'0000000000000000'})
        if '--obj-file' in command:
            Path(command[command.index('--obj-file')+1]).write_bytes(b'object protocol')
        count=len(rows)
        report={'schema':'jce.pakbom.v1','file':str(pak),'file_size':pak.stat().st_size,
            'sources_verified':True,'selected_source':sources[0] if len(sources)==1 else None,
            'selected_sources':sources,'sources':list(rows.values()),
            'header':{key:True for key in ('header_verified','encrypted','secure_index','authenticated','auth_verified')},
            'totals':{'entries':count,'verify_ran':True,'verified_count':count,'corrupt_count':0,
                      'skipped_count':0,'encrypted_count':count,'authenticated_count':count}}
        Path(command[command.index('--json')+1]).write_text(self.json.dumps(report))

    def admit(self, bundles, startup=None, *, module=policy):
        manifest={'bundles':[path.name for path in bundles]}
        if startup is not None:manifest['startup_scene']=startup
        result=module.admit_project(self.driver,self.args,self.project,manifest,self.sdk,
                                   target=self.target,sdk_tools=self.tools)
        return manifest,result

    def build_driver(self):
        spec=importlib.util.spec_from_file_location('jce_declared_bundle_driver',ROOT/'tools/build/jce.py')
        driver=importlib.util.module_from_spec(spec);spec.loader.exec_module(driver)
        return driver

    def pack(self, manifest, *, run_port=None):
        from unittest.mock import patch
        driver=self.build_driver()
        with patch.object(driver,'run',side_effect=run_port or self.run_port), \
             patch.object(driver,'sdk_shipped_script_backends',return_value=[]), \
             patch.object(driver,'_project_pak_key',return_value=bytes(32)), \
             patch.object(driver,'_write_pak_key_shares_c') as shares:
            flags=driver._dist_prebuilt_assets(self.project,self.target,self.project/'build/dist',
                manifest,{},self.sdk,self.tools,bundle_admission=self.args._bundle_admission)
        return flags,shares

    def test_bundle_only_uses_internal_scene_with_optional_absent_sidecar(self):
        path=self.bundle('one')
        _,languages=self.admit([path])
        self.assertEqual(languages,[])
        self.assertEqual(self.args._bundle_admission['startup_scene'],'scenes/main.scene.json')
        self.assertFalse(self.args._bundle_admission['use_loose_scene'])
        self.assertIsNone(self.args._bundle_admission['bundles'][0]['sidecar_sha256'])
        self.assertEqual(len(self.commands),1)
        self.assertIn('--verify',self.commands[0])

    def test_order_and_explicit_selected_bundle_scene_follow_runtime_priority(self):
        shared=self.bundle('shared',scene=None,assets=[self.asset('shared/data.bin',7,'binary','binary.raw')])
        second=self.bundle('second','scenes/second.scene.json',[self.asset('scenes/second.scene.json',2)])
        first=self.bundle('first','scenes/first.scene.json',[self.asset('scenes/first.scene.json',3)])
        self.admit([shared,second,first])
        self.assertEqual(self.args._bundle_admission['startup_scene'],'scenes/second.scene.json')
        self.admit([shared,second,first],'scenes/first.scene.json')
        self.assertEqual(self.args._bundle_admission['startup_scene'],'scenes/first.scene.json')
        with self.assertRaisesRegex(ValueError,'unavailable'):
            self.admit([first],'scenes/missing.scene.json')

    def test_all_declared_typed_scripts_are_admitted_not_only_first_scene(self):
        first=self.bundle('first')
        script=self.asset('opaque.jceasset',8,'script','python.source')
        later=self.bundle('later','scenes/later.scene.json',[self.asset('scenes/later.scene.json',2),script])
        with self.assertRaisesRegex(ValueError,'cannot provide'):
            self.admit([first,later])
        self.reports[str(later)]['bundle']['manifest']['assets'][1]['representation']='lua.source'
        _,languages=self.admit([first,later])
        self.assertEqual(languages,['lua'])

    def test_equal_overlap_merges_but_identity_or_missing_bundle_dependency_is_refused(self):
        one=self.bundle('one')
        two=self.bundle('two','scenes/two.scene.json',[self.asset('scenes/two.scene.json',2),self.asset('scenes/main.scene.json')])
        self.admit([one,two])
        self.assertEqual(len(self.args._bundle_admission['assets']),2)
        self.assertEqual(len(self.args._bundle_admission['provenance']['scenes/main.scene.json']),2)
        metadata=self.reports[str(two)]['bundle']['manifest']
        metadata['assets'][1]['content_id']='ffffffffffffffff'
        with self.assertRaisesRegex(ValueError,'conflicting'):
            self.admit([one,two])
        metadata['assets'][1]=self.asset('scenes/main.scene.json')
        metadata['depends_on']=['not_declared']
        with self.assertRaisesRegex(ValueError,'dependency is missing'):
            self.admit([one,two])

    def test_bad_public_inspection_does_not_fall_back_to_sidecar_or_plain_archive(self):
        import copy
        one=self.bundle('one',sidecar=True)
        original=copy.deepcopy(self.reports[str(one)])
        controls=(lambda r:r.pop('bundle'),lambda r:r['bundle'].update(verified=False),
            lambda r:r['totals'].update(verified_count=0),lambda r:r['totals'].update(skipped_count=1),
            lambda r:r['header'].update(header_verified=False),lambda r:r['bundle']['sidecar'].update(compatible=False),
            lambda r:r['bundle']['sidecar'].update(present=False),lambda r:r['bundle']['manifest']['contract'].update(minor=0),
            lambda r:r['bundle']['manifest'].update(target_profile='web'),lambda r:r['bundle']['manifest']['assets'][0].update(size=-1),
            lambda r:r['bundle']['manifest']['assets'][0].update(dependencies=[{'address':'missing.lua','asset_id':'0000000000000004'}]),
            lambda r:r['bundle']['manifest']['assets'][0].update(type='script\0binary'),
            lambda r:r['bundle']['manifest']['assets'][0].update(representation='lua.source\0python.source'))
        for index,change in enumerate(controls):
            with self.subTest(control=index):
                self.reports[str(one)]=copy.deepcopy(original);change(self.reports[str(one)])
                with self.assertRaises(ValueError):self.admit([one])
        self.reports[str(one)]=original
        self.admit([one])

    def test_legacy_sidecar_needs_actual_reader_compatibility_and_is_not_hash_auth(self):
        one=self.bundle('one',sidecar=True)
        self.reports[str(one)]['bundle']['sidecar']['verified']=False
        manifest,_=self.admit([one])
        self.assertTrue(self.args._bundle_admission['bundles'][0]['sidecar_compatible'])
        self.assertFalse(self.args._bundle_admission['bundles'][0]['sidecar_verified'])
        self.pack(manifest)
        receipt=self.json.loads((self.project/'build/dist/jce_generated/project_assets.receipt.json').read_text())
        self.assertFalse(receipt['bundle_inputs'][0]['sidecar_verified'])
        self.reports[str(one)]['bundle']['sidecar']['compatible']=False
        with self.assertRaisesRegex(ValueError,'compatibility'):self.admit([one])
        del self.reports[str(one)]['bundle']['sidecar']['compatible']
        with self.assertRaises(ValueError):self.admit([one])

    def test_source_sidecar_presence_and_inspection_changes_invalidate_plan(self):
        one=self.bundle('one');self.admit([one])
        plan=self.args._bundle_admission
        Path(str(one)+'.json').write_text('appeared after inspection')
        with self.assertRaisesRegex(ValueError,'sidecar changed'):policy.verify_bundle_inputs(self.driver,plan)
        Path(str(one)+'.json').unlink()
        report=Path(plan['bundles'][0]['inspection_path']);report.write_text('{}')
        with self.assertRaisesRegex(ValueError,'inspection changed'):policy.verify_bundle_inputs(self.driver,plan)
        self.admit([one]);one.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'Bundle changed'):policy.verify_bundle_inputs(self.driver,self.args._bundle_admission)

    def test_duplicate_json_and_stale_sdk_without_bundle_fields_refuse(self):
        from unittest.mock import patch
        one=self.bundle('one')
        def duplicate(command,**kwargs):
            self.run_port(command,**kwargs)
            output=Path(command[command.index('--json')+1])
            text=output.read_text().replace('"verified": true','"verified": false, "verified": true',1)
            output.write_text(text)
        with patch.object(self.driver,'run',side_effect=duplicate),self.assertRaisesRegex(ValueError,'duplicate'):
            self.admit([one])
        self.driver.DRY_RUN=True
        with self.assertRaisesRegex(ValueError,'dry-run'):
            self.admit([one])

    def test_removed_actual_bundle_language_wire_would_admit_opaque_python(self):
        from types import ModuleType
        one=self.bundle('one',assets=[self.asset('scenes/main.scene.json'),self.asset('opaque.jceasset',8,'script','python.source')])
        with self.assertRaises(ValueError):self.admit([one])
        source=(ROOT/'tools/build/delivery.py').read_text(encoding='utf-8')
        wire="""    if plan:
        groups = [(source['path'], source['assets']) for source in plan['bundles']]
        if typed_assets:
            groups.insert(0, (output / 'admission.jbundle', typed_assets))
        typed_assets, _ = merge_selected_assets(groups)
        addresses = [asset['path'] for asset in typed_assets]
"""
        self.assertEqual(source.count(wire),1)
        mutant=ModuleType('jce_removed_declared_bundle_language_wire')
        mutant.__file__=str(ROOT/'tools/build/delivery.py')
        exec(compile(source.replace(wire,'',1),mutant.__file__,'exec'),mutant.__dict__)
        self.assertEqual(self.admit([one],module=mutant)[1],[])

    def test_inspection_command_keeps_exact_web_and_mobile_target(self):
        for target,category,exact in (({'host':'windows','emscripten':True},'web','web'),
                                     ({'host':'android'},'mobile','android'),
                                     ({'host':'ios'},'mobile','ios')):
            with self.subTest(exact=exact):
                self.target=target
                one=self.bundle('target-'+exact)
                self.reports[str(one)]['bundle']['manifest']['target_profile']=exact
                self.commands.clear()
                self.admit([one])
                command=self.commands[0]
                self.assertEqual(command[command.index('--bundle-target')+1],exact)
                self.assertEqual(command[command.index('--platform')+1],category)

    def test_dist_bundle_union_uses_one_authenticated_pak_boot_and_no_plain_embedding(self):
        one=self.bundle('one');two=self.bundle('two','scenes/two.scene.json',[self.asset('scenes/two.scene.json',2),self.asset('scenes/main.scene.json')])
        manifest,_=self.admit([one,two]);flags,shares=self.pack(manifest)
        pack=[command for command in self.commands if '--pak-file' in command][0]
        self.assertEqual(pack.count('--selected-archive'),2)
        self.assertEqual(pack[pack.index('--bundle-target')+1], 'windows')
        inspections=[command for command in self.commands if '--inspect' in command]
        self.assertEqual(len(inspections),2)
        for command in inspections:
            self.assertEqual(command[command.index('--bundle-target')+1], 'windows')
            self.assertEqual(command[command.index('--platform')+1], 'desktop')
        self.assertIn('--encrypt-key-file',pack)
        self.assertFalse(any('--cook-assets' in command for command in self.commands))
        self.assertIn('-DJCE_PROJECT_BUNDLES=',flags)
        gen=self.project/'build/dist/jce_generated'
        self.assertEqual(self.json.loads((gen/'runtime_boot/jce/runtime_boot.json').read_text())['startup_scene'],'scenes/main.scene.json')
        receipt=self.json.loads((gen/'project_assets.receipt.json').read_text())
        self.assertEqual(receipt['selected_count'],2)
        self.assertEqual(receipt['bundle_inputs'][0]['sidecar_sha256'],None)
        self.assertEqual(receipt['runtime_parity'],'NOT VERIFIED')
        shares.assert_called_once()

    def test_explicit_loose_scene_and_bundles_share_real_graph_and_union_proof_path(self):
        one=self.bundle('one')
        scene=self.project/'resources/scenes/loose.scene.json';scene.parent.mkdir(parents=True);scene.write_text('{}')
        manifest,_=self.admit([one],'scenes/loose.scene.json');flags,_=self.pack(manifest)
        self.assertTrue(self.args._bundle_admission['use_loose_scene'])
        pack=[command for command in self.commands if '--pak-file' in command][0]
        self.assertEqual(pack.count('--selected-archive'),2)
        selected=[pack[index+1] for index,value in enumerate(pack[:-1]) if value=='--selected-archive']
        self.assertEqual(Path(selected[0]).name,'admission.jbundle')
        self.assertEqual(Path(selected[0]).parent.name,'content_graph')
        self.assertEqual(selected[1],str(one))
        self.assertIn('-DJCE_PROJECT_BUNDLES=',flags)

    def app(self, manifest, *, configure_only=True, clean=False, after_link=None):
        import sys
        from types import SimpleNamespace
        from unittest.mock import patch
        driver=self.build_driver()
        (self.project/'jce_project.json').write_text(self.json.dumps(dict(manifest,target='BundleApp',exe='BundleApp')))
        config=self.sdk/'lib/cmake/JCE/JCEConfig.cmake';config.parent.mkdir(parents=True,exist_ok=True);config.touch()
        (self.sdk/'delivery-profile.json').write_text(self.json.dumps({'profile':'embedded'}))
        args=SimpleNamespace(project=str(self.project),arch='x64',variant='dist',
            delivery_profile='embedded',standalone=False,target=None,exe=None,registry_proof=None,
            configure_only=configure_only,clean=clean)
        target=dict(self.target,host=driver.HOST)
        def dispatch(command,**kwargs):
            if command[0]=='cmake':
                self.commands.append(command)
                if '--build' in command:
                    out=Path(command[command.index('--build')+1]);out.mkdir(parents=True,exist_ok=True)
                    (out/'BundleApp.exe').write_bytes(b'link protocol only, not a native artifact')
                    if after_link:after_link()
            else:self.run_port(command,**kwargs)
        with patch.dict(sys.modules,{driver.__name__:driver}), \
             patch.object(driver,'resolve_target',return_value=target), \
             patch.object(driver,'resolve_sdk',return_value=self.sdk), \
             patch.object(driver,'_confirmed_app_sdk_tools',return_value=self.tools), \
             patch.object(driver,'_delivery_module',return_value=policy), \
             patch.object(driver,'sdk_shipped_script_backends',return_value=[]), \
             patch.object(driver,'msvc_env',return_value={}), \
             patch.object(driver,'_project_pak_key',return_value=bytes(32)), \
             patch.object(driver,'_write_pak_key_shares_c'), \
             patch.object(driver,'run',side_effect=dispatch), \
             patch.object(policy,'audit_application') as audit:
            self.audit=audit
            driver.cmd_app.__wrapped__(args)
        return audit

    def test_actual_app_configure_consumes_bundle_plan_and_disables_plain_embedding(self):
        one=self.bundle('one')
        self.app({'bundles':[one.name]})
        command=next(command for command in self.commands if command[0]=='cmake')
        self.assertEqual(Path(command[command.index('-B') + 1]),
                         self.project / 'build/desktop/windows-x64/dist')
        selections=[value for value in command if value.startswith('-DJCE_PROJECT_BUNDLES=')]
        self.assertEqual(selections[-1],'-DJCE_PROJECT_BUNDLES=')
        self.assertTrue(any(value.startswith('-DJCE_PROJECT_PREBUILT_ASSETS_OBJ=') for value in command))
        self.assertTrue(any(value.startswith('-DJCE_PROJECT_PREBUILT_PAK_KEY_C=') for value in command))
        self.assertEqual(len([command for command in self.commands if '--inspect' in command]),1)
        self.audit.assert_not_called()

    def test_actual_app_unsupported_script_fails_before_clean_or_compile_and_changed_link_fails_before_acceptance(self):
        one=self.bundle('one',assets=[self.asset('scenes/main.scene.json'),self.asset('opaque.jceasset',8,'script','python.source')])
        prior=self.project/'build/windows-x86_64-dist/old-user-result';prior.parent.mkdir(parents=True);prior.write_bytes(b'preserve')
        with self.assertRaises(SystemExit):self.app({'bundles':[one.name]},clean=True)
        self.assertEqual(prior.read_bytes(),b'preserve')
        self.assertFalse(any(command[0]=='cmake' or '--pak-file' in command for command in self.commands))
        self.reports[str(one)]['bundle']['manifest']['assets'][1]['representation']='lua.source'
        with self.assertRaises(SystemExit):
            self.app({'bundles':[one.name]},configure_only=False,
                     after_link=lambda:Path(str(one)+'.json').write_text('appeared after linking'))
        self.assertTrue(any(command[:2]==['cmake','--build'] for command in self.commands))
        self.audit.assert_not_called()

    def test_final_proof_missing_source_auth_or_changed_input_cannot_publish_receipt(self):
        from unittest.mock import patch
        one=self.bundle('one')
        for control in ('source','auth','identity','changed'):
            self.admit([one]);manifest={'bundles':[one.name]}
            def bad(command,**kwargs):
                self.run_port(command,**kwargs)
                if '--pak-file' not in command:return
                output=Path(command[command.index('--json')+1]);report=self.json.loads(output.read_text())
                if control=='source':report['selected_sources']=[]
                if control=='auth':report['header']['auth_verified']=False
                if control=='identity':next(row for row in report['sources'] if row['selected'])['content_id']='ffffffffffffffff'
                if control=='changed':one.write_bytes(b'changed after pack')
                output.write_text(self.json.dumps(report))
            with self.subTest(control=control),self.assertRaises(SystemExit):self.pack(manifest,run_port=bad)
            self.assertFalse((self.project/'build/dist/jce_generated/project_assets.receipt.json').exists())
            if control=='changed':break


if __name__ == '__main__':
    unittest.main()
