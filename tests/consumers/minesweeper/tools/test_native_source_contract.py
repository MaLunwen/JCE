"""Real consumer metadata + existing driver admission; no compiler/runtime claim."""
import copy
import hashlib
import json
from pathlib import Path
import re
import sys
import unittest
import xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT))
from tools.build import native_registry, delivery
EXT={'.lua':'lua','.jcejs':'js','.py':'python','.java':'java','.jcec':'c','.jcecpp':'cpp','.cs':'csharp'}
def inputs(name):
 base=ROOT/'tests/consumers'/name;manifest=json.loads((base/'jce_project.json').read_text())
 body=json.loads((base/manifest['source_assets']/manifest['startup_scene']).read_text());body=body.get('scene',body)
 paths=sorted({c['scriptPath'] for e in body['entities'] for c in e['components'] if c['type']=='Script'})
 # Derived typed IDs are explicit CPU fixtures, not an asserted actual PAK BOM.
 typed=[]
 for path in paths:
  suffix=Path(path).suffix
  if suffix not in ('.jcec','.jcecpp'):continue
  raw=(base/manifest['source_assets']/path).read_bytes();digest=hashlib.sha256(raw).hexdigest()
  typed.append({'path':path.lower(),'type':'script','representation':'c.class-ref' if suffix=='.jcec' else 'cpp.class-ref','asset_id':digest[:16],'content_id':digest[16:32],'size':len(raw),'dependencies':[]})
 return base,manifest,paths,typed
class NativeSourceContracts(unittest.TestCase):
 def test_actual_scene_paths_and_required_languages_match(self):
  for name in ['minesweeper','snake_seven']:
   base,manifest,paths,_=inputs(name)
   self.assertEqual(set(manifest['required_languages']),{EXT[Path(p).suffix] for p in paths})
   for path in paths:self.assertTrue((base/manifest['source_assets']/path).is_file(),path)
 def test_actual_native_bindings_feed_existing_plan_and_cmake_contract(self):
  for name in ['minesweeper','snake_seven']:
   _,manifest,_,typed=inputs(name);plan=native_registry.plan(manifest,typed,manifest['target'])
   self.assertEqual(plan['state'],'PENDING_STATIC_REGISTRY_EXECUTION')
   self.assertEqual(plan['bindings'],manifest['native_script_refs'])
   self.assertEqual(plan['factories'],manifest['native_script_factories'])
   args=native_registry.cmake_args(plan)
   self.assertIn('-DJCE_REQUIRED_SCRIPT_TARGET='+manifest['target'],args)
   self.assertIn('-DJCE_REQUIRED_NATIVE_FACTORIES='+';'.join(manifest['native_script_factories']),args)
 def test_missing_or_conflicting_bindings_are_not_admitted(self):
  for name in ['minesweeper','snake_seven']:
   _,manifest,_,typed=inputs(name)
   for mutate in ['refs','factory','representation']:
    m=copy.deepcopy(manifest);assets=copy.deepcopy(typed)
    if mutate=='refs':m['native_script_refs']={}
    if mutate=='factory':m['native_script_factories']=[]
    if mutate=='representation':assets[0]['representation']='lua.source'
    with self.assertRaises(ValueError):native_registry.plan(m,assets,m['target'])
 def test_actual_current_sdk_admission_keeps_full_seven_vm_obligation(self):
  sdk=ROOT/'dist/sdk/win32-x86_64-dist-single/lib/cmake/JCE/JCEScripting.cmake'
  if not sdk.is_file():self.skipTest('explicit installed SDK unavailable')
  match=re.search(r'set\(JCE_SCRIPT_COMPILED_LANGUAGES "([^"]*)"\)',sdk.read_text())
  self.assertIsNotNone(match);available={'lua'}|set(filter(None,match.group(1).split(';')))
  _,m,p,a=inputs('minesweeper');plan=native_registry.plan(m,a,m['target'])
  delivery.admit(m,p,'embedded',available,typed_assets=a,native_plan=plan)
  _,m,p,a=inputs('snake_seven');plan=native_registry.plan(m,a,m['target'])
  if set(m['required_languages'])<=available:
   delivery.admit(m,p,'authoring',available,typed_assets=a,native_plan=plan)
  else:
   with self.assertRaises(ValueError):delivery.admit(m,p,'authoring',available,typed_assets=a,native_plan=plan)
  with self.assertRaises(ValueError):delivery.admit(m,p,'embedded',available,typed_assets=a,native_plan=plan)
 def test_managed_source_and_output_have_single_canonical_owner(self):
  base=ROOT/'tests/consumers/snake_seven';tree=ET.parse(base/'csharp/SnakeScripts.csproj')
  self.assertEqual([e.attrib['Include'] for e in tree.iter('Compile')],['../resources/scripts/Hud.cs'])
  record=json.loads((base/'csharp/SOURCE_MAP.json').read_text())
  self.assertEqual(hashlib.sha256((base/record['active_source']).read_bytes()).hexdigest(),record['sha256_at_migration'])
  self.assertEqual((base/record['active_source']).read_bytes(),(base/record['preserved_inactive_source']).read_bytes())
  manifest=json.loads((base/'jce_project.json').read_text());self.assertEqual(manifest['script_assemblies'],['csharp/build/SnakeScripts.dll'])
 def test_entry_shim_and_cmake_have_one_declared_factory_publication(self):
  for name in ['minesweeper','snake_seven']:
   base,m,_,_=inputs(name);main=(base/'src/main.c').read_text();cmake=(base/'CMakeLists.txt').read_text()
   self.assertEqual(main.count('jce_script_register_linked_languages(NULL)'),1)
   self.assertNotIn('jce_script_vm_cpp_add_module(',main)
   self.assertIn('NATIVE_FACTORIES '+' '.join(m['native_script_factories']),cmake)
   self.assertIn('REQUIRED',cmake)
   self.assertIn('RUNTIME_OUTPUT_DIRECTORY_${_cfg}',cmake)
class VerifyGameFailureEvidence(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  import importlib.util
  spec=importlib.util.spec_from_file_location('mines_verify_game_failure_owner',Path(__file__).with_name('verify_game.py'))
  cls.owner=importlib.util.module_from_spec(spec);spec.loader.exec_module(cls.owner)
 def setUp(self):
  import tempfile
  self.temp=tempfile.TemporaryDirectory(prefix='mines-verify-failure-')
  self.addCleanup(self.temp.cleanup)
  self.root=Path(self.temp.name);self.exe=self.root/'selected game.exe'
  self.exe.write_bytes(b'EXPLICIT FILE; subprocess is always a counted port')
  self.owner.GAME=self.exe;self.owner.OUT=self.root/'evidence with spaces'
 def evidence(self,name):
  folder=self.owner.OUT
  receipt=json.loads((folder/(name+'-artifact.json')).read_text(encoding='utf-8'))
  raw=(folder/(name+'.log')).read_bytes()
  self.assertEqual(receipt['log_sha256'],hashlib.sha256(raw).hexdigest())
  self.assertEqual(receipt['executable'],str(self.exe))
  return receipt,raw.decode('utf-8')
 def test_timeout_keeps_available_output_and_identity_before_reraising(self):
  from unittest.mock import patch
  import subprocess
  profile=self.root/'isolated profile'
  failure=subprocess.TimeoutExpired([str(self.exe)],0.125,output=b'partial stdout\n',stderr=b'partial stderr\n')
  with patch.object(self.owner.subprocess,'run',side_effect=failure) as child:
   with self.assertRaises(subprocess.TimeoutExpired):
    self.owner.run('timed',timeout=0.125,extra={'JCE_EDITOR_PROFILE_DIR':str(profile)})
  child.assert_called_once()
  self.assertEqual(child.call_args.args[0],[str(self.exe)])
  self.assertEqual(child.call_args.kwargs['cwd'],self.exe.parent)
  self.assertEqual(child.call_args.kwargs['env']['JCE_EDITOR_PROFILE_DIR'],str(profile))
  self.assertEqual(child.call_args.kwargs['env'].get('HOME'),self.owner.os.environ.get('HOME'))
  receipt,log=self.evidence('timed')
  self.assertEqual(receipt['status'],'TIMEOUT');self.assertIsNone(receipt['exit'])
  self.assertEqual(receipt['timeout_s'],0.125);self.assertTrue(receipt['identity_stable'])
  self.assertEqual(receipt['hash_before_status'],'AVAILABLE')
  self.assertEqual(receipt['hash_after_status'],'AVAILABLE')
  self.assertIn('partial stdout',log);self.assertIn('partial stderr',log)
 def test_launch_oserror_is_persisted_without_inventing_child_exit(self):
  from unittest.mock import patch
  failure=OSError(5,'counted launch refusal')
  with patch.object(self.owner.subprocess,'run',side_effect=failure) as child:
   with self.assertRaises(OSError):self.owner.run('launch')
  child.assert_called_once()
  receipt,log=self.evidence('launch')
  self.assertEqual(receipt['status'],'LAUNCH_FAILED');self.assertIsNone(receipt['exit'])
  self.assertEqual(receipt['failure_stage'],'launch');self.assertEqual(receipt['failure_errno'],5)
  self.assertTrue(receipt['identity_stable']);self.assertIn('counted launch refusal',log)
  self.assertFalse(receipt['stdout_available']);self.assertFalse(receipt['stderr_available'])
 def test_real_before_and_after_read_failures_keep_receipts_and_refuse_pass(self):
  from unittest.mock import patch
  import subprocess
  original=self.exe.read_bytes()
  for stage in ('before','after'):
   with self.subTest(stage=stage):
    self.exe.write_bytes(original)
    if stage=='before':self.exe.unlink()
    def child_port(*args,**kwargs):
     self.exe.unlink()
     return subprocess.CompletedProcess(args[0],0,'available child stdout\n','')
    with patch.object(self.owner.subprocess,'run',side_effect=child_port) as child:
     with self.assertRaises(FileNotFoundError):self.owner.run('read-'+stage)
    self.assertEqual(child.call_count,0 if stage=='before' else 1)
    receipt,log=self.evidence('read-'+stage)
    self.assertEqual(receipt['status'],'ARTIFACT_READ_FAILED_'+stage.upper())
    self.assertFalse(receipt['identity_stable']);self.assertEqual(receipt['hash_after_status'],'UNAVAILABLE')
    self.assertTrue(receipt['hash_after_error']);self.assertEqual(receipt['failure_stage'],'hash_'+stage)
    if stage=='before':
     self.assertEqual(receipt['hash_before_status'],'UNAVAILABLE');self.assertIsNone(receipt['exit'])
    else:
     self.assertEqual(receipt['hash_before_status'],'AVAILABLE');self.assertEqual(receipt['exit'],0)
     self.assertIn('available child stdout',log)

if __name__=='__main__':unittest.main()
