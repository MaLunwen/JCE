import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from contextlib import redirect_stdout
from unittest import mock
import bundle_reopen_suite as suite
from bundle_reopen_suite import validate, validate_content_record, ROOT, sha256_file, POLICY_OWNERS
def add_owner_probes(before,after):
 # Serialized input controls only; real lifecycle evidence comes from runtime
 # snapshots, never from this authored parser fixture.
 for _,owner,script,probe in POLICY_OWNERS:
  for doc,updates in ((before,30),(after,200)):
   doc['entities'].append({'name':owner,'components':[{'type':'Script','scriptPath':'scripts/'+script}]})
   doc['entities'].append({'name':probe,'components':[{'type':'Transform','posX':1,'posY':updates,'posZ':5}]})

class PolicyWitnessTests(unittest.TestCase):
 def test_liveness_cannot_replace_rain_motion_or_mute(self):
  def entity(n,k,**v):return {'name':n,'components':[dict(type=k,**v)]}
  before={'entities':[entity('Ground','MeshRenderer',albedoTex='spring_night.png'),entity('flower_0','Transform',rotZ=0),entity('Firefly_0','Transform',posX=0),entity('CampfireLight','Light',intensity=4),entity('CampfireLight2','Light',intensity=2)]}
  after=copy.deepcopy(before);after['entities'][0]['components'][0]['albedoTex']='rainy_night.png';after['entities'][1]['components'][0]['rotZ']=1;after['entities'][2]['components'][0]['posX']=1
  for e in after['entities'][3:]:e['components'][0]['intensity']=0
  add_owner_probes(before,after)
  log='ES_QA_CONTENT_MUTE master_policy=0\nes_director online:\n'+'\n'.join('ES_PORTABLE '+r+' Lua started' for r in ('fireflies','campfire','flowers','surfaces'))
  image=(160,160,3,bytes(i%256 for i in range(160*160*3)))
  validate(log,before,after,image)
  for bad in [log.replace('master_policy=0','missing'),log+'] ERROR - fail']:
   with self.assertRaises(ValueError):validate(bad,before,after,image)
  with self.assertRaises(ValueError):validate(log,before,before,image)
  with self.assertRaises(ValueError):validate(log,before,after,(160,160,3,bytes(160*160*3)))

 def test_normal_content_preserves_runtime_policy_and_rejects_qa_mute(self):
  def entity(n,k,**v):return {'name':n,'components':[dict(type=k,**v)]}
  before={'entities':[entity('Ground','MeshRenderer',albedoTex='spring_night.png'),entity('flower_0','Transform',rotZ=0),entity('Firefly_0','Transform',posX=0),entity('CampfireLight','Light',intensity=4),entity('CampfireLight2','Light',intensity=2)]}
  after=copy.deepcopy(before);after['entities'][0]['components'][0]['albedoTex']='rainy_night.png';after['entities'][1]['components'][0]['rotZ']=1;after['entities'][2]['components'][0]['posX']=1
  for e in after['entities'][3:]:e['components'][0]['intensity']=0
  add_owner_probes(before,after)
  log='es_director online:\n'+'\n'.join('ES_PORTABLE '+r+' Lua started' for r in ('fireflies','campfire','flowers','surfaces'))
  image=(160,160,3,bytes(i%256 for i in range(160*160*3)))
  result=validate(log,before,after,image,qa_content_mute=False)
  self.assertEqual(result['content_policy'],'normal')
  self.assertIn('audio output not measured',result['scope'])
  for bad in [log+'\nES_QA_CONTENT_MUTE master_policy=0',log+'] ERROR - fail']:
   with self.assertRaises(ValueError):validate(bad,before,after,image,qa_content_mute=False)
  with self.assertRaises(ValueError):validate(log,before,before,image,qa_content_mute=False)
  with self.assertRaises(ValueError):validate(log,before,after,(160,160,3,bytes(160*160*3)),qa_content_mute=False)

 def policy_inputs(self):
  def entity(n,k,**v):return {'name':n,'components':[dict(type=k,**v)]}
  before={'entities':[entity('Ground','MeshRenderer',albedoTex='spring_night.png'),entity('flower_0','Transform',rotZ=0),entity('Firefly_0','Transform',posX=0),entity('CampfireLight','Light',intensity=4),entity('CampfireLight2','Light',intensity=2)]}
  after=copy.deepcopy(before);after['entities'][0]['components'][0]['albedoTex']='rainy_night.png';after['entities'][1]['components'][0]['rotZ']=1;after['entities'][2]['components'][0]['posX']=1
  for e in after['entities'][3:]:e['components'][0]['intensity']=0
  add_owner_probes(before,after)
  return before,after,(160,160,3,bytes(i%256 for i in range(160*160*3)))

 def test_actual_owner_probes_replace_unavailable_dist_boot_log_without_removing_lifecycle(self):
  before,after,image=self.policy_inputs()
  result=validate('',before,after,image,qa_content_mute=False)
  self.assertEqual(set(result['policy_lifecycle']),{r[0] for r in POLICY_OWNERS})
  self.assertTrue(all(v=='NOT_OBSERVED' for v in result['boot_log'].values()))
  self.assertIn('portable Lua policy roles',result['scope'])
  # Existing qa-muted admission is not relaxed by the normal-content channel.
  with self.assertRaises(ValueError):validate('',before,after,image)

 def test_log_boot_cannot_mask_missing_start_stalled_or_invalid_actual_probe(self):
  before,after,image=self.policy_inputs()
  log='es_director online:\n'+'\n'.join('ES_PORTABLE '+r+' Lua started' for r in ('fireflies','campfire','flowers','surfaces'))
  for _,_,_,probe in POLICY_OWNERS:
   for key,value in [('posX',0),('posX',2),('posY',30),('posY',1),('posZ',0),('posZ',6),('posY',True),('posY',float('nan')),('posY',float('inf')),('posY',200.5),('posY',16777216)]:
    with self.subTest(probe=probe,key=key,value=value):
     bad=copy.deepcopy(after)
     next(e for e in bad['entities'] if e['name']==probe)['components'][0][key]=value
     with self.assertRaises(ValueError):validate(log,before,bad,image,qa_content_mute=False)

 def test_wrong_owner_binding_missing_or_duplicate_probe_refuses_even_when_world_moves(self):
  before,after,image=self.policy_inputs()
  for _,owner,_,probe in POLICY_OWNERS:
   for mode in ('missing','duplicate','wrong-binding','missing-component'):
    with self.subTest(owner=owner,mode=mode):
     bad=copy.deepcopy(after)
     row=next(e for e in bad['entities'] if e['name']==probe)
     if mode=='missing':bad['entities'].remove(row)
     elif mode=='duplicate':bad['entities'].append(copy.deepcopy(row))
     elif mode=='missing-component':row['components']=[]
     else:next(e for e in bad['entities'] if e['name']==owner)['components'][0]['scriptPath']='scripts/unrelated_portable.lua'
     with self.assertRaises(ValueError):validate('',before,bad,image,qa_content_mute=False)

 def test_actual_catalog_record_rehashes_normal_and_rejects_stale_or_wrong_policy(self):
  import json
  import tempfile
  from pathlib import Path
  bundle=ROOT/'examples/elemental_serenity/elemental_serenity.jbundle'
  record_path=bundle.parent/'BUNDLE_PROVENANCE.json'
  if not bundle.is_file() or not record_path.is_file():self.skipTest('published ES catalog bytes/provenance unavailable')
  record=json.loads(record_path.read_text(encoding='utf-8'))
  self.assertEqual(validate_content_record(record,bundle,'normal'),sha256_file(bundle))
  for key,value in [('variant','qa'),('qa_muted',True),('file','other.jbundle'),('sha256','0'*64),('bytes',record['bytes']+1),('bytes',True)]:
   bad=dict(record);bad[key]=value
   with self.assertRaises(ValueError):validate_content_record(bad,bundle,'normal')
  with self.assertRaises(ValueError):validate_content_record(record,bundle,'qa-muted')
  with tempfile.TemporaryDirectory() as temporary:
   changed=Path(temporary)/bundle.name;changed.write_bytes(b'changed archive bytes')
   with self.assertRaises(ValueError):validate_content_record(record,changed,'normal')

 def test_muted_record_keeps_explicit_qa_admission(self):
  import tempfile
  from pathlib import Path
  with tempfile.TemporaryDirectory() as temporary:
   # Metadata admission only; this fixture makes no archive/renderer claim.
   bundle=Path(temporary)/'qa.jbundle';bundle.write_bytes(b'controlled admission bytes')
   record={'qa_content_mute':True,'outputs':{bundle.name:sha256_file(bundle)}}
   self.assertEqual(validate_content_record(record,bundle,'qa-muted'),sha256_file(bundle))
   for bad in [dict(record,qa_content_mute=False),dict(record,qa_content_mute='true'),dict(record,outputs={bundle.name:'0'*64})]:
    with self.assertRaises(ValueError):validate_content_record(bad,bundle,'qa-muted')
   with self.assertRaises(ValueError):validate_content_record(record,bundle,'normal')

def lifecycle_files(out,mutation=None):
 # These are explicitly authored CPU inputs to the real CSV/scene/stream
 # consumers. They do not stand in for an actual Editor or rendered image.
 before,after,image=PolicyWitnessTests().policy_inputs()
 # Authored CPU parser inputs only. The production runner reads independent
 # real scene600/800 files and never writes these expected runtime values.
 selected={'HudCanvas','MoodRequest','MoodPlan','MoodAck','MoodProvider','MoodStatus','MoodState6'}
 authored=json.loads(suite.MOOD_LAYOUT.read_text(encoding='utf-8'))['scene']['entities']
 for doc in (before,after):
  doc['entities'].extend(copy.deepcopy(e) for e in authored if e['name'] in selected)
 def position(doc,name,xyz):
  t=suite.component(doc,name,'Transform');t.update(zip(('posX','posY','posZ'),xyz))
 position(after,'MoodRequest',(0,-1,0));position(after,'MoodAck',(0,1,0))
 suite.component(after,'MoodStatus','UIText')['text']='READY | portable preset (native UNAVAILABLE)'
 suite.component(after,'Ground','MeshRenderer')['albedoTex']='textures/ground_baked2/rainy_night.png'
 for doc,text in ((before,'ELEMENTAL SERENITY'),(after,'')):
  doc['entities'].append({'name':'IntroTitle','components':[{'type':'UIText','text':text,'fontPath':''}]})
  doc['entities'].append({'name':'BeautyCam','components':[{'type':'Camera','primary':True}]})
  for i,entity in enumerate(doc['entities'],1):entity.update(id=i,parentId=0)
  canvas=next(e['id'] for e in doc['entities'] if e['name']=='HudCanvas')
  for e in doc['entities']:
   if e['name'] in ('MoodStatus','MoodState6'):e['parentId']=canvas
 before={'scene':before};during={'scene':after};restored=copy.deepcopy(before)
 for entity in restored['scene']['entities']:
  entity['id']+=1000
  if entity['parentId']:entity['parentId']+=1000
 if mutation=='restore':restored['scene']['entities'][0]['components'][0]['albedoTex']='different.png'
 if mutation=='intro':during['scene']['entities'][-2]['components'][0]['text']='ELEMENTAL SERENITY'
 if mutation=='camera':during['scene']['entities'][-1]['components']=[]
 early_after=copy.deepcopy(before)
 for _,_,_,probe in POLICY_OWNERS:
  next(e for e in early_after['scene']['entities'] if e['name']==probe)['components'][0]['posY']=200
 if mutation=='early-liveness':
  next(e for e in early_after['scene']['entities'] if e['name']=='EsLuaProbe')['components'][0]['posY']=30
 weather_before=copy.deepcopy(before)
 next(e for e in weather_before['scene']['entities'] if e['name']=='IntroTitle')['components'][0]['text']=''
 if mutation=='weather':during['scene']['entities'][0]['components'][0]['albedoTex']='spring_night.png'
 mood_after=copy.deepcopy(during)
 position(mood_after,'MoodRequest',(1,6,0));position(mood_after,'MoodAck',(1,6,1))
 suite.component(mood_after,'MoodStatus','UIText')['text']='APPLIED | portable preset (native UNAVAILABLE) | rainy_day'
 suite.component(mood_after,'Ground','MeshRenderer')['albedoTex']='textures/ground_baked2/rainy_day.png'
 if mutation=='mood-missing-ack':mood_after['scene']['entities']=[e for e in mood_after['scene']['entities'] if e['name']!='MoodAck']
 if mutation=='mood-wrong-seq':position(mood_after,'MoodAck',(2,6,1))
 if mutation=='mood-provider':position(mood_after,'MoodProvider',(1,0,0))
 if mutation=='mood-ground':suite.component(mood_after,'Ground','MeshRenderer')['albedoTex']='textures/ground_baked2/rainy_night.png'
 if mutation=='mood-status':suite.component(mood_after,'MoodStatus','UIText')['text']='REQUESTED | portable preset (native UNAVAILABLE) | 1'
 if mutation=='mood-script':suite.component(mood_after,'MoodRequest','Script')['scriptPath']='scripts/unrelated_portable.lua'
 for frame,doc in ((10,before),(50,before),(220,early_after),(400,weather_before),(600,during),(800,mood_after),(880,restored)):
  (out/f'scene-{frame}.json').write_text(json.dumps(doc),encoding='utf-8')
 phases={'ed_play_capture':20,'ed_play_create':20,'ed_play_teardown':820,'ed_play_restore':820}
 if mutation=='early-stop':phases.update(ed_play_teardown=700,ed_play_restore=700)
 lines=['frame_index,phase,ms']
 for frame in range(899 if mutation=='truncated' else 900):
  lines.append(f'{frame},frame_wall,1.0000')
  for name,at in phases.items():lines.append(f'{frame},{name},{0.125 if frame==at else 0:.4f}')
 (out/'frames.csv').write_text('\n'.join(lines)+'\n',encoding='utf-8')
 events=[]
 def service(name,identity):
  stamp=identity*1000
  events.extend([dict(ph='i',cat='task',pid=1,tid=1,ts=stamp,name=name,args=dict(task=identity,parent=0,kind=2)),
   dict(ph='B',cat='task',pid=1,tid=1,ts=stamp+1,name=name,args=dict(id=identity,parent=identity)),
   dict(ph='E',cat='task',pid=1,tid=1,ts=stamp+101,name=name,args=dict(id=identity,parent=identity,ns=100000,state=0))])
 for identity,name in enumerate(suite.LIFECYCLE_REQUIRED,1):
  if mutation=='missing-tick' and name=='runtime.tick':continue
  service(name,identity)
 if mutation=='duplicate-create':service('runtime.create',30)
 # Complete fixed-schema resource sample: unavailable observations are zero,
 # explicitly unknown, rather than manufactured resident/GPU measurements.
 from tools import resource_trace_observation as resource
 values={name:0 for name in resource.FLAGS+resource.SCALARS+resource.WIDE}
 values.update(schema=1,sample_ns=100000,owner_thread_id=1,**{'tagged.partial_scope':1})
 counters={name:values[name] for name in resource.FLAGS+resource.SCALARS}
 for name in resource.WIDE:
  counters[name+'.hi32']=values[name]>>32;counters[name+'.lo32']=values[name]&0xffffffff
 identity=50;stamp=50000
 service(resource.SERVICE,identity)
 end=events.pop()
 for index,(name,value) in enumerate(counters.items()):
  events.append(dict(ph='C',cat='core',pid=1,tid=1,ts=stamp+2+index,name='resource.'+name,args=dict(id=identity,parent=0,value=value)))
 events.append(end)
 # Actual public trace wire shape for one CPU submit boundary. This authored
 # fixture does not establish backend execution or GPU completion.
 from tools import renderer_submission_observation as submission
 identity=51;stamp=51000
 service(submission.SERVICE,identity)
 end=events.pop()
 for index,(name,value) in enumerate(zip(submission.FIELDS,(-1,0xffffffff,0,0))):
  events.append(dict(ph='C',cat='render',pid=1,tid=1,ts=stamp+2+index,name=submission.PREFIX+name,args=dict(id=identity,parent=identity,value=value)))
 events.append(end)
 if mutation=='unclosed':events.pop()
 if mutation=='missing-resource':events=[e for e in events if e['name']!=resource.SERVICE and not e['name'].startswith('resource.')]
 if mutation=='missing-submission':events=[e for e in events if e['name']!=submission.SERVICE and not e['name'].startswith(submission.PREFIX)]
 if mutation=='wrong-submission-thread':next(e for e in events if e['name']==submission.PREFIX+submission.FIELDS[0])['tid']=2
 metadata=dict(mode='continuous',consumer_mode='threaded',worker_available=True,write_ok=True,capture_complete=True,
  unexported=0,active_threads=0,active_work_items=0,active_waits=0,recorded=len(events),captured=len(events),
  overwritten_before_capture=0,cursor_lost=0,lost=0,capture_generation_stable=True)
 if mutation=='lost':metadata.update(cursor_lost=1,lost=1)
 if mutation=='resident':metadata['mode']='resident'
 wire=json.dumps(dict(jceTrace=metadata,traceEvents=events)).encode()
 (out/'lifecycle-trace.json').write_bytes(wire+(b' trailing' if mutation=='eof' else b''))
 return image

def controlled_main(module,out,mutation=None):
 exe=out.parent/'editor.exe';exe.write_bytes(b'CPU controlled executable identity only')
 bundle=out.parent/'controlled.jbundle';bundle.write_bytes(b'CPU controlled bundle identity only')
 record=out.parent/'provenance.json'
 record.write_text(json.dumps(dict(variant='normal',qa_muted=False,file=bundle.name,sha256=sha256_file(bundle),bytes=bundle.stat().st_size)))
 image=[]
 def process_port(env,log_path,**kwargs):
  if kwargs['frames']!=900 or env['JCE_MAX_SECONDS']!='0':raise AssertionError('wrong effective full-mode frame budget')
  image.append(lifecycle_files(out,mutation))
  return {'process':{'returncode':0,'timed_out':False}}
 args=['es-witness','--exe',str(exe),'--bundle',str(bundle),'--content-policy','normal','--content-record',str(record),'--out',str(out)]
 log='\n'.join(f'Game event fixture source=synthetic-virtual-game frame={f} type={k} accepted=1 owner=1 focused=0 hovered=0 captured=0'
  for f,k in ((398,'M'),(400,'B'),(402,'B'),(648,'M'),(650,'B'),(652,'B')) if mutation!='mood-input' or f<648)
 with mock.patch.object(sys,'argv',args),mock.patch.object(module,'run',side_effect=process_port),\
      mock.patch.object(module,'read_execution_log',return_value=log),\
      mock.patch.object(module,'assert_backend',return_value='d3d11'),\
      mock.patch.object(module,'read_png',side_effect=lambda path:image[0]),redirect_stdout(io.StringIO()):
  module.main()

class FullLifecycleTests(unittest.TestCase):
 def test_explicit_virtual_admission_cannot_be_replaced_by_scene_motion_or_old_event_logs(self):
  log='\n'.join(f'Game event fixture source=synthetic-virtual-game frame={f} type={k} accepted=1 owner=1 focused=0 hovered=0 captured=0'
   for f,k in ((398,'M'),(400,'B'),(402,'B'),(648,'M'),(650,'B'),(652,'B')))
  self.assertEqual(len(suite.validate_virtual_events(log)['rows']),6)
  for bad in ('',log.replace('accepted=1','accepted=0',1),log.replace('owner=1','owner=2',1),
              log.replace('synthetic-virtual-game','legacy'),log.rsplit('\n',1)[0],
              log.replace('frame=400','frame=80',1),log.replace('frame=650','frame=80',1)):
   with self.assertRaises(ValueError):suite.validate_virtual_events(bad)

 def test_actual_full_plan_extends_beyond_old_intro_and_stops_after_capture(self):
  env=suite.capture_environment(Path('normal.jbundle'),'d3d11',Path('reports/cpu-control'),'full-lifecycle')
  self.assertEqual(env['JCE_MAX_FRAMES'],'900');self.assertEqual(env['JCE_MAX_SECONDS'],'0')
  self.assertGreater(float(env['JCE_KPI_GAME_SHOTS'].split('|')[0]),11.1)
  self.assertLess(float(env['JCE_KPI_GAME_SHOTS'].split('|')[0]),int(env['JCE_DBG_PLAY_STOP_AT'])/60)
  self.assertEqual(env['JCE_DBG_SCENE_OBSERVE_AT'],'10,50,220,400,600,800,880')
  self.assertEqual(suite.capture_events('full-lifecycle').splitlines(),[
   '398 M 0.5 0.065277778 0 0','400 B 0.5 0.065277778 1 1','402 B 0.5 0.065277778 1 0',
   '648 M 0.850781250 0.797916667 0 0','650 B 0.850781250 0.797916667 1 1','652 B 0.850781250 0.797916667 1 0'])
  self.assertEqual(env['JCE_TRACE'],'1');self.assertEqual(env['JCE_RESOURCE_TRACE'],'1')
  self.assertEqual(env['JCE_TRACE_EXPORT'],'')
  old=suite.capture_environment(Path('qa.jbundle'),'d3d11',Path('reports/cpu-control'),'policy')
  self.assertEqual(old['JCE_MAX_FRAMES'],'360');self.assertEqual(old['JCE_MAX_SECONDS'],'0')
  self.assertNotIn('JCE_TRACE_STREAM',old)
  self.assertEqual(suite.capture_events('policy').splitlines(),[
   '78 M 0.5 0.065277778 0 0','80 B 0.5 0.065277778 1 1','82 B 0.5 0.065277778 1 0'])

 def test_real_stream_csv_scene_consumers_accept_complete_owner_and_unknown_resource_fields(self):
  with tempfile.TemporaryDirectory() as folder:
   out=Path(folder);lifecycle_files(out)
   result=suite.validate_full_lifecycle(out)
   self.assertEqual(result['runtime_counts']['runtime.create'],1)
   self.assertEqual(result['runtime_counts']['runtime.destroy'],1)
   self.assertGreater(result['runtime_counts']['runtime.tick'],0)
   report=json.loads((out/'lifecycle-summary.json').read_text())
   self.assertTrue(report['json_utf8_eof_validated']);self.assertTrue(report['validation_complete'])
   self.assertEqual(report['resource_observations']['samples_count'],1)
   self.assertEqual(report['renderer_submissions']['packets_count'],1)
   self.assertEqual(result['renderer_submissions'],report['renderer_submissions'])
   self.assertEqual(result['renderer_submissions']['context_generation_unknown_count'],1)
   self.assertEqual(report['capture']['lost'],0)

 def test_real_consumers_refuse_old_intro_early_stop_restore_loss_eof_and_unclosed_owners(self):
  for mutation in ('intro','camera','early-stop','restore','truncated','lost','eof','unclosed','missing-tick','duplicate-create','missing-resource','missing-submission','wrong-submission-thread','resident'):
   with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as folder:
    out=Path(folder);lifecycle_files(out,mutation)
    with self.assertRaises(ValueError):suite.validate_full_lifecycle(out)

 def test_actual_default_main_consumes_full_validation_and_preserves_failure_receipt(self):
  with tempfile.TemporaryDirectory() as folder:
   out=Path(folder)/'accepted';controlled_main(suite,out)
   result=json.loads((out/'result.json').read_text())
   self.assertEqual(result['status'],'PORTABLE_ES_FULL_CPU_LIFECYCLE_OBSERVED')
   self.assertEqual(result['lifecycle']['host_frames'],900)
   self.assertIn('not 3D',result['image_scope'])
   self.assertEqual(result['mood']['status'],'PORTABLE_PRESET_APPLY_ACK_OBSERVED')
   self.assertEqual(result['mood']['poses']['after/MoodAck'],[1,6,1])
   self.assertEqual(result['mood']['host_observations'],[600,800])
  for mutation in ('duplicate-create','early-liveness','weather','missing-submission'):
   with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as folder:
    out=Path(folder)/'rejected'
    with self.assertRaises(ValueError):controlled_main(suite,out,mutation)
    result=json.loads((out/'result.json').read_text())
    self.assertEqual(result['status'],'FAIL')
    self.assertEqual(result['process']['process']['returncode'],0)
    self.assertEqual(result['actual_backend'],'d3d11')

 def test_old_source_schedule_and_omitted_lifecycle_wire_fail_ordinary_controls(self):
  for mode in ('--old-intro-control','--old-lifecycle-control','--old-submission-control'):
   process=subprocess.run([sys.executable,'-B',str(Path(__file__).resolve()),mode],capture_output=True,text=True,timeout=30)
   self.assertEqual(process.returncode,1,process.stdout+process.stderr)
   self.assertIn('AssertionError',process.stderr)
   self.assertNotIn('SyntaxError',process.stderr);self.assertNotIn('ImportError',process.stderr)

MOOD_CONTROLS={
 '--old-mood-ack-control':'mood-missing-ack',
 '--old-mood-seq-control':'mood-wrong-seq',
 '--old-mood-provider-control':'mood-provider',
 '--old-mood-ground-control':'mood-ground',
 '--old-mood-input-control':'mood-input',
}


class MoodReopenTests(unittest.TestCase):
 def test_button_position_comes_from_actual_image_rectangle_and_handler(self):
  doc=json.loads(suite.MOOD_LAYOUT.read_text(encoding='utf-8'))
  self.assertEqual(suite.mood_click_position(doc),(1089/1280,1-145.5/720))
  changed=copy.deepcopy(doc)
  suite.component(changed,'MoodState6','UIImage')['anchoredX']-=16
  self.assertEqual(suite.mood_click_position(changed)[0],(1089-16)/1280)
  # Label bounds are not the button hit rectangle.
  suite.component(doc,'MoodState6','UIText')['anchoredX']-=100
  self.assertEqual(suite.mood_click_position(doc),(1089/1280,1-145.5/720))
  for key,value in (('sizeW',0),('anchorMaxX',.5),('pivotX',True),('anchoredY',float('nan'))):
   bad=copy.deepcopy(doc);suite.component(bad,'MoodState6','UIImage')[key]=value
   with self.subTest(key=key),self.assertRaises(ValueError):suite.mood_click_position(bad)
  bad=copy.deepcopy(doc);suite.component(bad,'MoodState6','UIButton')['onClickHandler']='es_btn_rain'
  with self.assertRaises(ValueError):suite.mood_click_position(bad)
  bad=copy.deepcopy(doc);next(e for e in bad['scene']['entities'] if e['name']=='MoodState6')['parentId']=0
  with self.assertRaises(ValueError):suite.mood_click_position(bad)

 def test_real_main_consumes_sixth_owner_request_apply_ack_and_keeps_failure_receipts(self):
  for mutation in (*MOOD_CONTROLS.values(),'mood-status','mood-script'):
   with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as folder:
    out=Path(folder)/'rejected'
    with self.assertRaises(ValueError):controlled_main(suite,out,mutation)
    result=json.loads((out/'result.json').read_text())
    self.assertEqual(result['status'],'FAIL')
    self.assertEqual(result['process']['process']['returncode'],0)
    self.assertEqual(result['actual_backend'],'d3d11')

 def test_snapshot_values_cannot_replace_actual_button_target_or_admission(self):
  with tempfile.TemporaryDirectory() as folder:
   out=Path(folder);lifecycle_files(out)
   before,after=[json.loads((out/f'scene-{f}.json').read_text()) for f in (600,800)]
   events=suite.capture_events('full-lifecycle')
   result=suite.validate_mood(before,after,events)
   self.assertEqual(result['poses']['before/MoodProvider'],[0,0,0])
   self.assertEqual(result['poses']['after/MoodRequest'],[1,6,0])
   self.assertIn('not physical input',result['scope'])
   for bad in (events.replace('0.850781250','0.5'),events.replace('650 B','651 B'),
               '\n'.join(events.splitlines()[:3])):
    with self.assertRaises(ValueError):suite.validate_mood(before,after,bad)
   for name,kind,key,value in [('MoodAck','Transform','posZ',0),('MoodAck','Transform','posX',True),
      ('MoodRequest','Transform','posY',7),('MoodPlan','Transform','posX',1),
      ('CampfireLight','Light','intensity',1)]:
    bad=copy.deepcopy(after);suite.component(bad,name,kind)[key]=value
    with self.subTest(name=name,key=key),self.assertRaises(ValueError):suite.validate_mood(before,bad,events)

 def test_omitted_actual_mood_wires_fail_ordinary_assertions(self):
  for mode in MOOD_CONTROLS:
   process=subprocess.run([sys.executable,'-B',str(Path(__file__).resolve()),mode],capture_output=True,text=True,timeout=30)
   self.assertEqual(process.returncode,1,process.stdout+process.stderr)
   self.assertIn('AssertionError',process.stderr)
   self.assertNotIn('SyntaxError',process.stderr);self.assertNotIn('ImportError',process.stderr)


def ordinary_control(mode):
 source=Path(suite.__file__).read_text(encoding='utf-8')
 if mode=='--old-intro-control':
  old="{'frames':900,'shot_seconds':12.0";new="{'frames':360,'shot_seconds':3.0"
 elif mode=='--old-submission-control':
  old='resource_observations=True,renderer_submissions=True';new='resource_observations=True'
 elif mode in MOOD_CONTROLS:
  if mode=='--old-mood-input-control':
   old="expected=[(398,'M'),(400,'B'),(402,'B'),(648,'M'),(650,'B'),(652,'B')]"
   new="expected=[(398,'M'),(400,'B'),(402,'B')]"
  else:
   old="result['mood']=validate_mood(*[json.loads((out/f'scene-{f}.json').read_text()) for f in (600,800)],events.read_text(encoding='utf-8'))"
   new="result['mood']={}"
 else:
  old="result['lifecycle']=validate_full_lifecycle(out)";new="result['lifecycle']={}"
 assert source.count(old)==1
 if mode=='--old-submission-control':
  # Remove the dependent new receipt transport too: otherwise a missing key
  # masks the ordinary false-acceptance assertion with an unrelated KeyError.
  receipt="'renderer_submissions':report['renderer_submissions']"
  assert source.count(receipt)==1
  source=source.replace(receipt,"'renderer_submissions':report.get('renderer_submissions',{})",1)
 module=types.ModuleType('controlled_es');module.__file__=suite.__file__
 exec(compile(source.replace(old,new,1),suite.__file__+':ordinary-old-control','exec'),module.__dict__)
 if mode=='--old-intro-control':
  env=module.capture_environment(Path('normal.jbundle'),'d3d11',Path('reports/cpu-control'),'full-lifecycle')
  assert env['JCE_MAX_FRAMES']=='900' and float(env['JCE_KPI_GAME_SHOTS'].split('|')[0])>11.1,'old schedule failed ordinary full-mode assertions'
 elif mode=='--old-lifecycle-control':
  with tempfile.TemporaryDirectory() as folder:
   rejected=False
   try:controlled_main(module,Path(folder)/'rejected','duplicate-create')
   except ValueError:rejected=True
   assert rejected,'omitted actual lifecycle wire accepted duplicate runtime owner'
 elif mode in MOOD_CONTROLS:
  with tempfile.TemporaryDirectory() as folder:
   rejected=False
   try:controlled_main(module,Path(folder)/'rejected',MOOD_CONTROLS[mode])
   except ValueError:rejected=True
   assert rejected,'omitted actual Mood wire accepted '+MOOD_CONTROLS[mode]
 else:
  with tempfile.TemporaryDirectory() as folder:
   rejected=False
   try:controlled_main(module,Path(folder)/'rejected','missing-submission')
   except ValueError:rejected=True
   assert rejected,'omitted actual submission consumer accepted absent renderer packet'

if __name__=='__main__':
 if len(sys.argv)==2 and sys.argv[1] in ('--old-intro-control','--old-lifecycle-control','--old-submission-control',*MOOD_CONTROLS):ordinary_control(sys.argv[1])
 else:unittest.main()
