"""CPU metadata/source closure; actual SDK Lua/GUI acceptance is separate."""
import ast
import copy
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path
import tempfile
import unittest
from es_mood_scene import append_mood_nodes, STATES
from stage_portable_bundle import stage, export_command
BASE = Path(__file__).resolve().parents[1]
ASSETS = BASE / 'resources/assets'
sys.path.insert(0, str(BASE.parents[2]))
from tools.build.delivery import admit
def scene(name): return json.loads((ASSETS/'scenes'/name).read_text(encoding='utf-8'))['scene']['entities']
class MoodSourceControls(unittest.TestCase):
 def test_default_entry_is_the_instantiated_public_sdk_template(self):
  template=(BASE.parents[2]/'engine/templates/empty/src/main.c').read_text(encoding='utf-8')
  expected=template.replace('@JCE_PROJECT_NAME@','Elemental Serenity').replace('@JCE_PROJECT_TARGET@','ElementalSerenity')
  self.assertEqual((BASE/'src/main_portable.c').read_text(encoding='utf-8'),expected)
 def test_portable_director_declares_the_existing_runtime_dependency_bytes(self):
  selected=(ASSETS/'scripts/es_director_portable.lua.deps.json').read_bytes()
  self.assertEqual(selected,(ASSETS/'scripts/es_director.lua.deps.json').read_bytes())
  declarations=json.loads(selected)['assets']
  self.assertEqual(len(set(declarations)),9)
  self.assertEqual({p for p in declarations if p.startswith('textures/ground_baked2/')},
   {'textures/ground_baked2/'+season+'_'+time+'.png'
    for season in ('spring','winter','autumn','rainy') for time in ('day','night')})
 @unittest.skipUnless(os.environ.get('JCE_ES_DEPENDENCY_PACKER'),
  'requires explicit public SDK JCE_ES_DEPENDENCY_PACKER; native closure unavailable')
 def test_actual_public_packer_resolves_all_portable_runtime_dependencies(self):
  packer=Path(os.environ['JCE_ES_DEPENDENCY_PACKER']).resolve()
  self.assertTrue(packer.is_file())
  scratch=BASE.parents[2]/'build/tests/es-default-dependency-closure'
  scratch.mkdir(parents=True,exist_ok=True)
  with tempfile.TemporaryDirectory(dir=scratch) as tmp:
   out=Path(tmp)/'bundles'
   result=subprocess.run(export_command(packer,ASSETS,out,'windows',BASE),
    capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=180)
   self.assertEqual(result.returncode,0,result.stdout+'\n'+result.stderr)
   graph=json.loads((out/'bundle_graph.json').read_text(encoding='utf-8'))
   addresses={item['address'] for item in graph['assets']}
   declared=json.loads((ASSETS/'scripts/es_director_portable.lua.deps.json').read_text(encoding='utf-8'))['assets']
   self.assertTrue(set(declared)<=addresses,sorted(set(declared)-addresses))
   # The sidecar is an authoring dependency declaration, not runtime content.
   self.assertNotIn('scripts/es_director_portable.lua.deps.json',addresses)
   self.assertNotIn('scripts/es_director.lua',addresses)
   print('ES_PUBLIC_DEPENDENCY_CLOSURE assets='+str(len(addresses))+
    ' declared_runtime='+str(len(declared))+' all_present=true')
 def test_ordinary_project_selects_actual_portable_lua_scene_and_embedded_closure(self):
  manifest=json.loads((BASE/'jce_project.json').read_text(encoding='utf-8'))
  self.assertEqual(manifest['startup_scene'],'scenes/elemental_serenity.bundle.scene.json')
  self.assertEqual(manifest['required_languages'],['lua'])
  for field in ('script_modules','native_script_factories','native_script_refs'):
   self.assertNotIn(field,manifest)
  selected=ASSETS/manifest['startup_scene']
  entities=json.loads(selected.read_text(encoding='utf-8'))['scene']['entities']
  paths=[c['scriptPath'] for e in entities for c in e['components'] if c['type']=='Script']
  self.assertEqual(len(paths),6)
  self.assertEqual(set(paths),{'scripts/es_director_portable.lua','scripts/es_fireflies_portable.lua',
   'scripts/es_campfire_portable.lua','scripts/es_flowers_portable.lua',
   'scripts/es_surfaces_portable.lua','scripts/es_mood_portable.lua'})
  for address in paths:
   resource=(ASSETS/address).resolve()
   self.assertTrue(resource.is_relative_to(ASSETS.resolve()))
   self.assertTrue(resource.is_file(),address)
  # Consume the real shipment policy with the actual bound addresses and only
  # Lua availability; this is policy admission, not a native link/run receipt.
  self.assertEqual(admit(manifest,paths,'embedded',['lua']),['lua'])
 def test_five_language_fixture_retains_original_manifest_and_refuses_lua_only_sdk(self):
  original=(BASE/'jce_project.multilang.json').read_bytes()
  self.assertEqual(hashlib.sha256(original).hexdigest(),
   'e5d53568ffb4ae465f51ef450146a5a58c7629831474ad5c617aa5c62507c91e')
  manifest=json.loads(original)
  self.assertEqual(manifest['startup_scene'],'scenes/elemental_serenity.scene.json')
  self.assertEqual(set(manifest['required_languages']),{'lua','python','java','cpp','c'})
  self.assertEqual(manifest['native_script_factories'],['es_flower_sway_module','es_prop_surface_module'])
  self.assertEqual(manifest['script_modules'],['native/bin/es_scripts.dll','native/bin/es_scripts_c.dll'])
  entities=json.loads((ASSETS/manifest['startup_scene']).read_text(encoding='utf-8'))['scene']['entities']
  paths=[c['scriptPath'] for e in entities for c in e['components'] if c['type']=='Script']
  languages={'.lua':'lua','.py':'python','.java':'java','.jcecpp':'cpp','.jcec':'c'}
  self.assertEqual({languages[Path(address).suffix] for address in paths},set(manifest['required_languages']))
  with self.assertRaisesRegex(ValueError,'unavailable installed SDK languages'):
   admit(manifest,paths,'authoring',['lua'])
 def test_restoring_original_default_is_an_ordinary_admission_failure(self):
  old=json.loads((BASE/'jce_project.multilang.json').read_text(encoding='utf-8'))
  # The preserved old default is a real negative input, not a synthetic parser
  # mutation: it cannot satisfy the accepted embedded Lua-only product.
  with self.assertRaisesRegex(ValueError,'required native script backend'):
   admit(old,[],'embedded',['lua'])
 def test_default_aa_is_spatial_fxaa_with_temporal_opt_in(self):
  for name in ['elemental_serenity.scene.json','elemental_serenity.bundle.scene.json']:
   with self.subTest(source=name):
    rendering=json.loads((ASSETS/'scenes'/name).read_text(encoding='utf-8'))['scene']['rendering']
    self.assertIs(rendering['postfx']['fxaa'],True)
  with self.subTest(source='gen_scene.py'):
   owner=ast.parse((BASE/'tools/gen_scene.py').read_text(encoding='utf-8'))
   rendering=next(node.value for node in owner.body if isinstance(node,ast.Assign)
                  and any(isinstance(target,ast.Name) and target.id=='rendering' for target in node.targets))
   postfx=next(value for key,value in zip(rendering.keys,rendering.values) if ast.literal_eval(key)=='postfx')
   self.assertIs(ast.literal_eval(postfx)['fxaa'],True)
  with self.subTest(source='Settings/RenderPipeline.rp.json'):
   policy=json.loads((BASE/'Settings/RenderPipeline.rp.json').read_text(encoding='utf-8'))
   self.assertIs(policy['enable_taa'],False)
 def test_authoring_and_bundle_have_one_primary_output_for_beauty_shot(self):
  for name in ['elemental_serenity.scene.json','elemental_serenity.bundle.scene.json']:
   entities=scene(name)
   cameras=[(e,c) for e in entities for c in e['components'] if c['type']=='Camera' and c.get('primary')]
   self.assertEqual(len(cameras),1)
   e,c=cameras[0]
   self.assertEqual(e['name'],'BeautyCam')
   v=next(c for c in e['components'] if c['type']=='VirtualCamera')
   self.assertEqual(c['fov'],v['fov'])
   self.assertLess(c['nearClip'],c['farClip'])
   self.assertTrue(v['active'])
 def test_primary_camera_clear_mode_preserves_sky_in_actual_outputs(self):
  for name in ['elemental_serenity.scene.json','elemental_serenity.bundle.scene.json']:
   with self.subTest(source=name):
    cameras=[c for e in scene(name) if e['name']=='BeautyCam' for c in e['components']
             if c['type']=='Camera' and c.get('primary')]
    self.assertEqual(len(cameras),1)
    self.assertEqual(cameras[0]['clearMode'],0)
  with self.subTest(source='gen_scene.py'):
   owner=ast.parse((BASE/'tools/gen_scene.py').read_text(encoding='utf-8'))
   appends=[node.value.args[0] for node in owner.body
            if isinstance(node,ast.Expr) and isinstance(node.value,ast.Call)
            and isinstance(node.value.func,ast.Attribute) and node.value.func.attr=='append'
            and isinstance(node.value.func.value,ast.Name) and node.value.func.value.id=='entities'
            and len(node.value.args)==1 and isinstance(node.value.args[0],ast.Call)]
   beauty=[call for call in appends if isinstance(call.func,ast.Name) and call.func.id=='ent'
           and isinstance(call.args[0],ast.Constant) and call.args[0].value=='BeautyCam']
   self.assertEqual(len(beauty),1)
   cameras=[ast.literal_eval(node) for node in beauty[0].args[1].elts
            if isinstance(node,ast.Dict) and any(isinstance(key,ast.Constant) and key.value=='type'
             and isinstance(value,ast.Constant) and value.value=='Camera'
             for key,value in zip(node.keys,node.values))]
   self.assertEqual(len(cameras),1)
   self.assertIs(cameras[0]['primary'],True)
   self.assertEqual(cameras[0]['clearMode'],0)
 def test_exact_authoring_builder_reproduces_current_nodes_without_changing_existing(self):
  current=scene('elemental_serenity.scene.json')
  original=[e for e in current if not e['name'].startswith('Mood')]
  before=copy.deepcopy(original);append_mood_nodes(original)
  self.assertEqual(original[:len(before)],before)
  self.assertEqual(original,current)
 def test_builder_rejects_duplicate_owner_or_missing_canvas(self):
  current=scene('elemental_serenity.scene.json')
  with self.assertRaisesRegex(ValueError,'already exist'):append_mood_nodes(copy.deepcopy(current))
  with self.assertRaisesRegex(ValueError,'unique'):append_mood_nodes([{'id':1,'name':'not-canvas'}])
 def test_same_serialized_protocol_and_buttons_in_both_entries(self):
  authored=scene('elemental_serenity.scene.json');portable=scene('elemental_serenity.bundle.scene.json')
  self.assertEqual([e for e in authored if e['name'].startswith('Mood')],[e for e in portable if e['name'].startswith('Mood')])
  for entities in [authored,portable]:
   self.assertEqual(len({e['id'] for e in entities}),len(entities))
   request=next(e for e in entities if e['name']=='MoodRequest')
   self.assertIn({'type':'Script','scriptPath':'scripts/es_mood_portable.lua'},request['components'])
   handlers=[c['onClickHandler'] for e in entities if e['name'].startswith('Mood') for c in e['components'] if c['type']=='UIButton']
   self.assertEqual(set(handlers),{'es_mood_next'}|{'es_mood_'+s for s in STATES})
 def test_defaults_are_automatic_and_optional_font_has_matching_pinned_notice(self):
  for name in ['elemental_serenity.scene.json','elemental_serenity.bundle.scene.json']:
   self.assertTrue(all(c.get('fontPath','')=='' for e in scene(name) for c in e['components'] if c['type']=='UIText'))
  roots=json.loads((ASSETS/'bundle_roots.json').read_text())
  self.assertNotIn('fonts/LXGWWenKai-Regular.ttf',roots['assets'])
  record=json.loads((ASSETS/'licenses/lxgw-local-provenance.json').read_text())
  self.assertEqual(record['upstream_release_binary_match'],'NOT_VERIFIED')
  self.assertEqual(hashlib.sha256((ASSETS/record['path']).read_bytes()).hexdigest(),record['sha256'])
  self.assertEqual(hashlib.sha256((ASSETS/record['upstream_notice']['path']).read_bytes()).hexdigest(),record['upstream_notice']['sha256'])
 def test_actual_portable_staging_keeps_shared_mood_and_public_contract_bytes(self):
  with tempfile.TemporaryDirectory() as tmp:
   source=Path(tmp)/'source'
   paths=['scripts/es_mood_portable.lua','scripts/es_director_portable.lua','scripts/es_director.lua.deps.json','scenes/elemental_serenity.bundle.scene.json','bundle_roots.json','scene_ai/elemental_catalog.json','scene_ai/bootstrap_recipes.json']
   for name in paths:
    dest=source/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes((ASSETS/name).read_bytes())
   manifest=stage(source,Path(tmp)/'out',1024*1024)
   for name in ['scripts/es_mood_portable.lua','scene_ai/elemental_catalog.json','scene_ai/bootstrap_recipes.json']:
    self.assertIn(name,manifest)
    self.assertEqual((Path(tmp)/'out'/name).read_bytes(),(ASSETS/name).read_bytes())
 def test_shared_script_and_director_callback_are_present_in_generator_owner(self):
  generated=(BASE/'tools/gen_director.py').read_text(encoding='utf-8')
  actual=(ASSETS/'scripts/es_director.lua').read_text(encoding='utf-8')
  portable=(ASSETS/'scripts/es_director_portable.lua').read_text(encoding='utf-8')
  start='-- The same message bridge in generated/native and portable directors.'
  def bridge(text):
   begin=text.index(start)
   # Compare the complete top-level bridge, excluding portable-only seed helpers.
   return text[begin:text.index('\nend\n',begin)+len('\nend\n')]
  self.assertEqual(bridge(generated),bridge(actual));self.assertEqual(bridge(actual),bridge(portable))
if __name__=='__main__':unittest.main()
