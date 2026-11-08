import hashlib
import json
from pathlib import Path
import tempfile
import types
import unittest
import stage_portable_bundle as staging
from stage_portable_bundle import stage,export_command
class StageTests(unittest.TestCase):
 def write_empty_director_deps(self,source):
  sidecar=source/'scripts/es_director.lua.deps.json';sidecar.parent.mkdir(parents=True,exist_ok=True)
  sidecar.write_text(json.dumps({'contract':{'name':'jce.bundle.dependencies','major':1,'minor':0},'assets':[]}),encoding='utf-8')
 def portable_source(self,parent):
  source=parent/'source';source.mkdir()
  entry=source/'scenes/elemental_serenity.bundle.scene.json';entry.parent.mkdir()
  entry.write_text(json.dumps({'entities':[{'components':[{'type':'Script','scriptPath':'scripts/es_director_portable.lua'}]}]}),encoding='utf-8')
  originals=Path(__file__).resolve().parents[1]/'resources/assets/scripts'
  script=source/'scripts/es_director_portable.lua';script.parent.mkdir()
  script.write_bytes((originals/script.name).read_bytes())
  declaration=(originals/'es_director.lua.deps.json').read_bytes()
  (source/'scripts/es_director.lua.deps.json').write_bytes(declaration)
  # Tiny payloads test staging/addresses, not image/audio cooking or rendering.
  for rel in json.loads(declaration)['assets']:
   asset=source/rel;asset.parent.mkdir(parents=True,exist_ok=True);asset.write_bytes(('staged '+rel).encode('utf-8'))
  return source,declaration
 def assert_director_declaration(self,out,manifest,declaration,source_path):
  rel='scripts/es_director_portable.lua.deps.json';sidecar=out/rel
  self.assertTrue(sidecar.is_file(),'selected portable script lost its dynamic dependency declaration')
  self.assertEqual(sidecar.read_bytes(),declaration)
  expected=hashlib.sha256(declaration).hexdigest()
  self.assertEqual(manifest[rel]['source_path'],source_path)
  self.assertEqual(manifest[rel]['source_sha256'],expected)
  self.assertEqual(manifest[rel]['staged_sha256'],expected)
  self.assertEqual(manifest[rel]['bytes'],len(declaration))
 def test_excludes_fonts_unused_scripts_private_and_preserves_original(self):
  with tempfile.TemporaryDirectory() as d:
   source=Path(d)/'source';source.mkdir()
   for rel in ['fonts/unlicensed.ttf','scripts/es_director_portable.lua','scripts/native.jcec','scene_ai/private.txt','scenes/elemental_serenity.bundle.scene.json']:
    p=source/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('{}')
   self.write_empty_director_deps(source)
   roots={'assets':['fonts/unlicensed.ttf','render_settings.json']};(source/'bundle_roots.json').write_text(json.dumps(roots))
   out=Path(d)/'out';manifest=stage(source,out,10000)
   self.assertFalse((out/'fonts').exists());self.assertFalse((out/'scene_ai').exists());self.assertFalse((out/'scripts/native.jcec').exists())
   self.assertEqual(json.loads((source/'bundle_roots.json').read_text()),roots)
   self.assertEqual(json.loads((out/'bundle_roots.json').read_text())['assets'],['render_settings.json'])
   with self.assertRaises(ValueError):stage(source,Path(d)/'tiny',1)
 def test_mute_is_only_transformed_staging_content(self):
  with tempfile.TemporaryDirectory() as d:
   source=Path(d)/'source';(source/'scripts').mkdir(parents=True);(source/'scenes').mkdir()
   script='local MASTER = 0.55\nfunction script.on_start(self)\njce.play_sound("audio/sounds/ui_interactions/click.mp3", 0.25)\nend\n'
   (source/'scripts/es_director_portable.lua').write_text(script)
   self.write_empty_director_deps(source)
   scene={'entities':[{'components':[{'type':'AudioSource','volume':1}]}]}
   (source/'scenes/elemental_serenity.bundle.scene.json').write_text(json.dumps(scene))
   out=Path(d)/'out';stage(source,out,10000,True)
   self.assertIn('ES_QA_CONTENT_MUTE',(out/'scripts/es_director_portable.lua').read_text())
   self.assertNotIn('click.mp3", 0.25',(out/'scripts/es_director_portable.lua').read_text())
   self.assertEqual(json.loads((out/'scenes/elemental_serenity.bundle.scene.json').read_text())['entities'][0]['components'][0]['volume'],0)
   self.assertEqual((source/'scripts/es_director_portable.lua').read_text(),script)
 def test_selected_cli_path_is_absolute_and_exists_independent_of_cwd(self):
  with tempfile.TemporaryDirectory() as d:
   assets=Path(d)/'resources';selected=assets/'scenes/elemental_serenity.bundle.scene.json';selected.parent.mkdir(parents=True);selected.write_text('{}')
   cmd=export_command(Path(d)/'packer',assets,Path(d)/'out','windows',Path(d))
   self.assertEqual(Path(cmd[cmd.index('--scene')+1]),selected.resolve())
   self.assertTrue(Path(cmd[cmd.index('--scene')+1]).is_absolute())
   self.assertIn('--cook-assets',cmd);self.assertIn('--single-file',cmd)
   self.assertEqual(Path(cmd[cmd.index('--project')+1]),Path(d).resolve())
   selected.unlink()
   with self.assertRaises(ValueError):export_command(Path(d)/'packer',assets,Path(d)/'out','windows',Path(d))
 def test_actual_director_declaration_readdresses_dynamic_season_and_audio_assets(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);source,declaration=self.portable_source(root)
   before={p.relative_to(source).as_posix():p.read_bytes() for p in source.rglob('*') if p.is_file()}
   out=root/'out';manifest=stage(source,out,1000000)
   self.assert_director_declaration(out,manifest,declaration,'scripts/es_director.lua.deps.json')
   self.assertFalse((out/'scripts/es_director.lua.deps.json').exists())
   self.assertFalse((out/'scripts/es_director.lua').exists())
   for rel in json.loads(declaration)['assets']:
    self.assertTrue((out/rel).is_file());self.assertEqual((out/rel).read_bytes(),before[rel])
   self.assertEqual(before,{p.relative_to(source).as_posix():p.read_bytes() for p in source.rglob('*') if p.is_file()})
 def test_selected_own_sidecar_wins_without_rewriting_its_document(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);source,original=self.portable_source(root)
   own=json.loads(original);own['labels']=['portable-specific','runtime'];own['assets']=own['assets'][-1:]
   declared=(json.dumps(own,indent=3)+'\n').encode('utf-8')
   (source/'scripts/es_director_portable.lua.deps.json').write_bytes(declared)
   out=root/'out';manifest=stage(source,out,1000000)
   self.assert_director_declaration(out,manifest,declared,'scripts/es_director_portable.lua.deps.json')
   self.assertFalse((out/'scripts/es_director.lua.deps.json').exists())
 def test_only_selected_portable_script_sidecars_are_staged(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);source,_=self.portable_source(root)
   selected=source/'scripts/es_fireflies_portable.lua';selected.write_text('return {}',encoding='utf-8')
   (source/'scripts/es_fireflies_portable.lua.deps.json').write_bytes(b'{"contract":{"name":"jce.bundle.dependencies","major":1,"minor":0},"assets":[]}')
   for name in ['unused.lua.deps.json','orphan_portable.lua.deps.json','native.jcec.deps.json']:
    (source/'scripts'/name).write_bytes(b'{"not":"a selected declaration"}')
   out=root/'out';manifest=stage(source,out,1000000)
   self.assertIn('scripts/es_fireflies_portable.lua.deps.json',manifest)
   self.assertEqual((out/'scripts/es_fireflies_portable.lua.deps.json').read_bytes(),(source/'scripts/es_fireflies_portable.lua.deps.json').read_bytes())
   for name in ['unused.lua.deps.json','orphan_portable.lua.deps.json','native.jcec.deps.json']:
    self.assertFalse((out/'scripts'/name).exists())
 def test_missing_required_alias_reports_selected_and_source_before_output(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);source,_=self.portable_source(root)
   (source/'scripts/es_director.lua.deps.json').unlink();out=root/'out'
   with self.assertRaisesRegex(ValueError,r'selected_script=scripts/es_director_portable\.lua source_sidecar=scripts/es_director\.lua\.deps\.json'):
    stage(source,out,1000000)
   self.assertFalse(out.exists())
 def test_dependency_declaration_counts_toward_existing_staging_budget(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);source,declaration=self.portable_source(root)
   payload=sum(p.stat().st_size for p in source.rglob('*') if p.is_file())-len(declaration)
   out=root/'too-small'
   with self.assertRaisesRegex(ValueError,'bounded resource budget exceeded'):stage(source,out,payload)
   self.assertFalse(out.exists())
   manifest=stage(source,root/'exact-budget',payload+len(declaration))
   self.assert_director_declaration(root/'exact-budget',manifest,declaration,'scripts/es_director.lua.deps.json')
 def test_executed_old_drop_wire_fails_same_declaration_witness(self):
  owner=Path(staging.__file__).read_text(encoding='utf-8');wire='files.extend(_portable_dependency_files(source,files))'
  self.assertEqual(owner.count(wire),1)
  old=types.ModuleType('es_stage_old_drop_wire')
  exec(compile(owner.replace(wire,'files.extend([])',1),str(staging.__file__)+'::old-drop-wire','exec'),old.__dict__)
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);source,declaration=self.portable_source(root);out=root/'old-out'
   manifest=old.stage(source,out,1000000)
   self.assertTrue((out/'scripts/es_director_portable.lua').is_file())
   self.assertTrue((out/'textures/ground_baked2/rainy_night.png').is_file())
   with self.assertRaisesRegex(AssertionError,'lost its dynamic dependency declaration'):
    self.assert_director_declaration(out,manifest,declaration,'scripts/es_director.lua.deps.json')
 def test_capture_seeds_stage_public_params_only_and_preserve_normal_bytes(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);source,_=self.portable_source(root);rel='scenes/elemental_serenity.bundle.scene.json'
   original=json.loads((source/rel).read_text());director=original['entities'][0]['components'][0]
   keep={'name':'speed','kind':0,'number':1.5,'entity':0,'text':''}
   director['params']=[keep,{'name':'capture_seed_b','kind':0,'number':99}]
   original['entities'][0]['components'].append({'type':'AudioSource','volume':0.7})
   (source/rel).write_text(json.dumps({'scene':original},indent=1)+'\n',encoding='utf-8')
   before={p.relative_to(source).as_posix():p.read_bytes() for p in source.rglob('*') if p.is_file()}
   normal=stage(source,root/'normal',1000000)
   self.assertEqual((root/'normal'/rel).read_bytes(),before[rel])
   self.assertEqual(normal[rel]['source_sha256'],normal[rel]['staged_sha256'])
   manifest=stage(source,root/'capture',1000000,True,(0,16777215))
   components=json.loads((root/'capture'/rel).read_text())['scene']['entities'][0]['components']
   self.assertEqual(components[0]['params'],[keep,
    {'name':'capture_seed_a','kind':0,'number':0,'entity':0,'text':''},
    {'name':'capture_seed_b','kind':0,'number':16777215,'entity':0,'text':''}])
   self.assertEqual(components[1]['volume'],0)
   self.assertNotEqual(manifest[rel]['source_sha256'],manifest[rel]['staged_sha256'])
   self.assertEqual(before,{p.relative_to(source).as_posix():p.read_bytes() for p in source.rglob('*') if p.is_file()})
 def test_capture_seed_only_preserves_audio_and_script_bytes(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);source,_=self.portable_source(root);rel='scenes/elemental_serenity.bundle.scene.json'
   doc=json.loads((source/rel).read_text());doc['entities'][0]['components'].append({'type':'AudioSource','volume':0.7})
   (source/rel).write_text(json.dumps(doc),encoding='utf-8')
   stage(source,root/'out',1000000,capture_seeds=(317,91))
   self.assertEqual(json.loads((root/'out'/rel).read_text())['entities'][0]['components'][1]['volume'],0.7)
   self.assertEqual((root/'out/scripts/es_director_portable.lua').read_bytes(),(source/'scripts/es_director_portable.lua').read_bytes())
 def test_invalid_capture_seeds_reject_before_creating_output(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);source,_=self.portable_source(root)
   for seeds in [(),(1,),(1,2,3),(True,1),(1.0,2),('1',2),(-1,0),(0,16777216),'12']:
    with self.subTest(seeds=seeds):
     with self.assertRaisesRegex(ValueError,'capture seeds require two integers'):
      stage(source,root/'out',1000000,capture_seeds=seeds)
     self.assertFalse((root/'out').exists())
 def test_ambiguous_missing_duplicate_and_full_script_params_reject_before_output(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);source,_=self.portable_source(root);entry=source/'scenes/elemental_serenity.bundle.scene.json'
   director={'type':'Script','scriptPath':'scripts/es_director_portable.lua'}
   for components,expected in [([], 'exactly one'),([director,director], 'exactly one'),
    ([dict(director,params=[{'name':'x'},{'name':'x'}])], 'duplicate'),
    ([dict(director,params=[{'name':str(i)} for i in range(7)])], 'eight Script'),
    ([dict(director,params={})], 'invalid Director')]:
    entry.write_text(json.dumps({'entities':[{'components':components}]}),encoding='utf-8')
    with self.assertRaisesRegex(ValueError,expected):stage(source,root/'out',1000000,capture_seeds=(317,91))
    self.assertFalse((root/'out').exists())
 def test_executed_missing_seed_wire_fails_public_scene_witness(self):
  owner=Path(staging.__file__).read_text(encoding='utf-8');wire="director['params']=params"
  self.assertEqual(owner.count(wire),1)
  old=types.ModuleType('es_stage_missing_seed_wire')
  exec(compile(owner.replace(wire,'pass',1),str(staging.__file__)+'::missing-seed-wire','exec'),old.__dict__)
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);source,_=self.portable_source(root)
   old.stage(source,root/'out',1000000,capture_seeds=(317,91))
   params=json.loads((root/'out/scenes/elemental_serenity.bundle.scene.json').read_text())['entities'][0]['components'][0].get('params',[])
   with self.assertRaisesRegex(AssertionError,'capture seed parameters missing'):
    self.assertEqual([p['name'] for p in params],['capture_seed_a','capture_seed_b'],'capture seed parameters missing')
 def test_project_policy_hash_bom_and_omission_control(self):
  with tempfile.TemporaryDirectory() as d:
   project=Path(d);source=project/staging.PIPELINE_SOURCE
   self.assertEqual(staging.project_settings(project),{})
   source.parent.mkdir();data=b'{"enable_volumetric_fog":false,"enable_ssr":false}\n';source.write_bytes(data)
   (source.parent/'unused.json').write_bytes(b'{"private":"not a build root"}')
   inputs=staging.project_settings(project)
   self.assertEqual(inputs,{staging.PIPELINE_ADDRESS:{'source_path':staging.PIPELINE_SOURCE,'source_sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data)}})
   item={'path':staging.PIPELINE_ADDRESS,'size':len(data),'content_id':'0123456789abcdef'}
   witness=staging.verify_project_settings_bom(project,inputs,[{'assets':[item]}])
   self.assertEqual(witness[staging.PIPELINE_ADDRESS]['source_sha256'],hashlib.sha256(data).hexdigest())
   for entries in ([],[item,item],[dict(item,size=1)],[dict(item,content_id=None)]):
    with self.assertRaisesRegex(ValueError,'canonical project render pipeline'):
     staging.verify_project_settings_bom(project,inputs,[{'assets':entries}])
   source.write_bytes(b'{}')
   with self.assertRaisesRegex(ValueError,'changed during export'):
    staging.verify_project_settings_bom(project,inputs,[{'assets':[item]}])
 def test_present_invalid_policy_is_not_missing_default(self):
  with tempfile.TemporaryDirectory() as d:
   project=Path(d);source=project/staging.PIPELINE_SOURCE;source.mkdir(parents=True)
   with self.assertRaisesRegex(ValueError,'regular file'):staging.project_settings(project)
   source.rmdir()
   for data in (b'',b'[]',b'{',b' '*1048577):
    source.write_bytes(data)
    with self.assertRaises(ValueError):staging.project_settings(project)
 def test_executed_old_missing_project_argument_fails_command_witness(self):
  owner=Path(staging.__file__).read_text(encoding='utf-8');wire="'--project',str(project.resolve()),"
  self.assertEqual(owner.count(wire),1)
  old=types.ModuleType('es_stage_old_missing_project')
  exec(compile(owner.replace(wire,'',1),str(staging.__file__)+'::missing-project','exec'),old.__dict__)
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);assets=root/'resources';entry=assets/'scenes/elemental_serenity.bundle.scene.json';entry.parent.mkdir(parents=True);entry.write_bytes(b'{}')
   command=old.export_command(root/'packer',assets,root/'out','windows',root)
   with self.assertRaisesRegex(AssertionError,'project settings owner missing'):
    self.assertIn('--project',command,'project settings owner missing')
if __name__=='__main__':unittest.main()
