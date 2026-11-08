"""ES policy/reopen witness; default full mode observes reveal, Stop and CPU closure."""
import argparse
import json
import math
from pathlib import Path
import re
import sys
ROOT=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT));sys.path.insert(0,str(ROOT/'tools'));sys.path.insert(0,str(ROOT/'tests/consumers/benchmark/tools'))
from tools.perf_bench import run,read_execution_log
from tools.jce_determinism import assert_backend,read_png
from tools.benchmark_suite import read_frames
from tools.editor_lifecycle_probe import PHASES,validate_scenes
from tools.lifecycle_trace_report import analyze_trace_file
from gbuffer_asset_suite import sha256_file,write

def component(doc,name,kind):
 body=doc.get('scene',doc)
 matches=[e for e in body['entities'] if e.get('name')==name]
 if len(matches)!=1:raise ValueError('canonical entity unavailable: '+name)
 found=[c for c in matches[0]['components'] if c.get('type')==kind]
 if len(found)!=1:raise ValueError('canonical component unavailable: '+name+'/'+kind)
 return found[0]

# These are the existing portable script owners and their authored Transform
# probes, not VM/language claims. Each actual Lua owner writes start/update/
# resolved counters; runtime scene observations read them independently of log.
POLICY_OWNERS=(
 ('director','Director','es_director_portable.lua','EsLuaProbe'),
 ('fireflies','Fireflies','es_fireflies_portable.lua','EsPyProbe'),
 ('campfire','Campfire','es_campfire_portable.lua','EsJavaProbe'),
 ('flowers','FlowerSway','es_flowers_portable.lua','EsCppProbe'),
 ('surfaces','PropSurface','es_surfaces_portable.lua','EsCProbe'),
)

LIFECYCLE_REQUIRED=('editor.play.begin','editor.play.stop','runtime.create',
 'runtime.tick','runtime.destroy','engine.async_shutdown','engine.app_exit',
 'engine.renderer_destroy')
MOOD_LAYOUT=Path(__file__).resolve().parents[1]/'resources/assets/scenes/elemental_serenity.bundle.scene.json'


def mood_click_position(doc):
 # This fixture supports the actual non-stretched, screen-overlay authored
 # button only. Derive its centre; do not substitute a script dispatch.
 canvas=component(doc,'HudCanvas','Canvas')
 image=component(doc,'MoodState6','UIImage')
 button=component(doc,'MoodState6','UIButton')
 if canvas.get('renderMode')!=0 or button.get('onClickHandler')!='es_mood_rainy_day':
  raise ValueError('actual portable Mood button/canvas unavailable')
 body=doc.get('scene',doc)
 parent=next(e for e in body['entities'] if e.get('name')=='HudCanvas')
 target=next(e for e in body['entities'] if e.get('name')=='MoodState6')
 if target.get('parentId')!=parent.get('id') or type(parent.get('id')) is not int:
  raise ValueError('Mood button is not on the actual HudCanvas')
 def number(owner,key):
  v=owner.get(key)
  if type(v) not in (int,float) or not math.isfinite(v):raise ValueError('invalid Mood layout field: '+key)
  return v
 w,h=number(canvas,'refResX'),number(canvas,'refResY')
 if (w,h)!=(1280,720):raise ValueError('Mood fixture requires its authored 1280x720 Canvas')
 x,y=[],[]
 for axis,size,out in (('X',w,x),('Y',h,y)):
  lo,hi=number(image,'anchorMin'+axis),number(image,'anchorMax'+axis)
  pivot=number(image,'pivot'+axis);extent=number(image,'sizeW' if axis=='X' else 'sizeH')
  if lo!=hi or not 0<=lo<=1 or not 0<=pivot<=1 or extent<=0:
   raise ValueError('unsupported Mood button rectangle')
  centre=size*lo+number(image,'anchored'+axis)+(0.5-pivot)*extent
  if not extent/2<=centre<=size-extent/2:raise ValueError('Mood button outside Canvas')
  out.append(centre/size)
 return x[0],1-y[0]


def mood_events(doc):
 x,y=mood_click_position(doc)
 return ''.join(f'{frame} {kind} {x:.9f} {y:.9f} {a} {b}\n'
  for frame,kind,a,b in ((648,'M',0,0),(650,'B',1,1),(652,'B',1,0)))

def capture_plan(mode):
 if mode=='policy':return {'frames':360,'shot_seconds':3.0,'observe':(50,220)}
 if mode!='full-lifecycle':raise ValueError('unknown capture mode')
 return {'frames':900,'shot_seconds':12.0,'observe':(10,50,220,400,600,800,880),
         'stop_play_ticks':800,'fixed_dt':'0.016666666667'}

def capture_events(mode):
 # Explicit synthetic Game input, not a script callback or physical mouse.
 # Current reveal completes after 1.2+4.5s; Play400 is 6.67s, with a separate
 # before/after weather window. Early policy liveness remains at 50/220.
 first=398 if mode=='full-lifecycle' else 78
 rain=''.join(f'{frame} {kind} 0.5 0.065277778 {a} {b}\n'
  for frame,kind,a,b in ((first,'M',0,0),(first+2,'B',1,1),(first+4,'B',1,0)))
 return rain+(mood_events(json.loads(MOOD_LAYOUT.read_text(encoding='utf-8'))) if mode=='full-lifecycle' else '')

def validate_virtual_events(log):
 rows=[tuple(map(int,(frame,accepted,owner,focused,hovered,captured)))+(kind,)
  for frame,kind,accepted,owner,focused,hovered,captured in re.findall(
   r'Game event fixture source=synthetic-virtual-game frame=(\d+) type=([MBW]) accepted=([01]) owner=(\d+) focused=([01]) hovered=([01]) captured=([01])',log)]
 expected=[(398,'M'),(400,'B'),(402,'B'),(648,'M'),(650,'B'),(652,'B')]
 if [(r[0],r[-1]) for r in rows]!=expected or any(r[1]!=1 or r[2]!=1 for r in rows):
  raise ValueError('complete explicit virtual Game input admission unavailable')
 return {'source':'synthetic-virtual-game','rows':[{'play_visible_frame':r[0],'kind':r[-1],
  'accepted':r[1],'owner':r[2],'focused':r[3],'hovered':r[4],'captured':r[5]} for r in rows],
  'scope':'explicit virtual pointer through runtime commit; not physical mouse or native OS focus admission'}


def validate_mood(before,after,event_text):
 # Only inspect actual saved runtime documents. No provider, plan, request or
 # expected acknowledgement is written into the launched scene by this tool.
 poses={}
 def pose(doc,name,expected,phase):
  t=component(doc,name,'Transform');v=[t.get(k) for k in ('posX','posY','posZ')]
  if any(type(n) not in (int,float) or not math.isfinite(n) for n in v) or tuple(v)!=expected:
   raise ValueError('actual Mood '+phase+' '+name+' unavailable')
  poses[phase+'/'+name]=v
 for phase,doc in (('before',before),('after',after)):
  if component(doc,'MoodRequest','Script').get('scriptPath')!='scripts/es_mood_portable.lua':
   raise ValueError('portable Mood Script binding unavailable')
  pose(doc,'MoodProvider',(0,0,0),phase)
  pose(doc,'MoodPlan',(0,0,0),phase)
  if mood_click_position(doc)!=mood_click_position(before):raise ValueError('Mood rectangle changed during capture')
 pose(before,'MoodRequest',(0,-1,0),'before');pose(before,'MoodAck',(0,1,0),'before')
 pose(after,'MoodRequest',(1,6,0),'after');pose(after,'MoodAck',(1,6,1),'after')
 ready='READY | portable preset (native UNAVAILABLE)'
 applied='APPLIED | portable preset (native UNAVAILABLE) | rainy_day'
 if component(before,'MoodStatus','UIText').get('text')!=ready or component(after,'MoodStatus','UIText').get('text')!=applied:
  raise ValueError('actual portable Mood status transition unavailable')
 old=component(before,'Ground','MeshRenderer').get('albedoTex')
 new=component(after,'Ground','MeshRenderer').get('albedoTex')
 if (old,new)!=('textures/ground_baked2/rainy_night.png','textures/ground_baked2/rainy_day.png'):
  raise ValueError('actual Mood rainy-night to rainy-day application unavailable')
 for doc in (before,after):
  if any(component(doc,name,'Light').get('intensity')!=0 for name in ('CampfireLight','CampfireLight2')):
   raise ValueError('Mood rainy preset resurrected the extinguished fire')
 expected=mood_events(before).splitlines()
 actual=[row for row in event_text.splitlines() if row.split() and row.split()[0] in ('648','650','652')]
 if actual!=expected:raise ValueError('Mood events do not match the actual serialized button')
 return {'status':'PORTABLE_PRESET_APPLY_ACK_OBSERVED','button':'MoodState6',
  'handler':'es_mood_rainy_day','normalized_position':mood_click_position(before),
  'host_observations':[600,800],'play_visible_input_frames':[648,650,652],
  'poses':poses,'status_before':ready,'status_after':applied,'ground_before':old,'ground_after':new,
  'scope':'actual serialized request/director Ack/status and ground application; virtual Game click, not physical input, native provider or Mood pixel parity'}

def capture_environment(bundle,backend,out,mode):
 plan=capture_plan(mode)
 env={'JCE_SCENE':str(bundle.resolve()),'JCE_BACKEND':backend,'JCE_WINDOW_HIDDEN':'1',
  'JCE_MULTI_INSTANCE':'1','JCE_EDITOR_PROFILE_DIR':str(out/'profile'),
  'JCE_MAX_SECONDS':'0','JCE_MAX_FRAMES':str(plan['frames']),
  'JCE_FRAME_DT_FIXED':'0.016666666667','JCE_DBG_AUTOPLAY':'20','JCE_DBG_FOCUS_GAME':'1',
  'JCE_KPI_GAME_EVENTS':str(out/'events.txt'),
  'JCE_KPI_GAME_SHOTS':str(plan['shot_seconds'])+'|'+str(out/'game.png'),
  'JCE_KPI_GAME_CAPTURE_SIZE':'1280x720','JCE_EDITOR_OPERATION_METRICS':'1',
  'JCE_DBG_SCENE_OBSERVE_DIR':str(out),
  'JCE_DBG_SCENE_OBSERVE_AT':','.join(map(str,plan['observe']))}
 if mode=='full-lifecycle':
  env.update(JCE_DBG_PLAY_STOP_AT=str(plan['stop_play_ticks']),JCE_TRACE='1',
   JCE_TRACE_STREAM=str(out/'lifecycle-trace.json'),JCE_TRACE_EXPORT='',JCE_RESOURCE_TRACE='1')
 return env

def validate_full_lifecycle(out):
 samples=read_frames(out/'frames.csv',warmup=0,sample_count=None,allow_zero_wall=True)
 if list(samples['frame_indices'])!=list(range(900)):
  raise ValueError('full lifecycle requires all 900 contiguous host frames')
 phases={}
 for name in PHASES:
  series=samples['phase_ms'].get(name)
  if series is None or len(series)!=900 or any(math.isinf(v) or (math.isfinite(v) and v<0) for v in series):
   raise ValueError('invalid lifecycle phase series: '+name)
  positive=[(frame,value) for frame,value in zip(samples['frame_indices'],series) if math.isfinite(value) and value>0]
  if len(positive)!=1:raise ValueError('one positive lifecycle phase required: '+name)
  phases[name]={'frame':positive[0][0],'ms':positive[0][1]}
 create,capture,teardown,restore=[phases[n]['frame'] for n in ('ed_play_create','ed_play_capture','ed_play_teardown','ed_play_restore')]
 if not (10<capture==create<50<220<800<teardown==restore<880<900):
  raise ValueError('Play/Stop must bracket policy, post-reveal and restored snapshots')
 docs=[json.loads((out/f'scene-{f}.json').read_text()) for f in (10,800,880)]
 restoration=validate_scenes(*docs,require_mutation=True)
 during=docs[1]
 if component(during,'IntroTitle','UIText').get('text')!='':
  raise ValueError('director intro title did not clear before Stop')
 body=during.get('scene',during)
 cameras=[c for e in body['entities'] for c in e['components']
          if c.get('type')=='Camera' and c.get('primary') is True]
 if len(cameras)!=1:raise ValueError('one serialized primary Camera required')
 report={'status':'INVALID_OR_INCOMPLETE_CAPTURE'}
 try:
  report=analyze_trace_file(out/'lifecycle-trace.json',LIFECYCLE_REQUIRED,resource_observations=True,renderer_submissions=True)
  counts={row['name']:row['observed_count'] for row in report['required_phase_aggregates']}
  for name in ('runtime.create','runtime.destroy','editor.play.begin','editor.play.stop'):
   if counts.get(name)!=1:raise ValueError('exactly one lifecycle owner required: '+name)
  if counts.get('runtime.tick',0)<=0:raise ValueError('runtime tick absent')
  if report['capture'].get('mode')!='continuous':raise ValueError('continuous lifecycle capture required')
 except Exception as error:
  report.update(getattr(error,'evidence',{}))
  report.update(status='INVALID_OR_INCOMPLETE_CAPTURE',error=str(error))
  raise
 finally:write(out/'lifecycle-summary.json',report)
 return {'status':'FULL_PLAY_STOP_CPU_LIFECYCLE_OBSERVED','host_frames':900,
  'phases':phases,'scene_restore':restoration,'runtime_counts':counts,
  'renderer_submissions':report['renderer_submissions'],
  'trace_report':str(out/'lifecycle-summary.json'),'trace_report_sha256':sha256_file(out/'lifecycle-summary.json'),
  'intro_title_cleared':True,'serialized_primary_camera_count':len(cameras),
  'capture_schedule':{'game_seconds':12.0,'stop_after_play_updates':800},
  'scope':'fixed-dt hidden normal renderer host; serialized reveal/Camera and CPU/registered-owner closure, not diorama pixels, physical GPU completion, all OS threads, leak or performance certification'}

def validate_policy_lifecycle(before,after):
 evidence={}
 for role,owner,script,probe in POLICY_OWNERS:
  rows=[]
  for doc in (before,after):
   if component(doc,owner,'Script').get('scriptPath')!='scripts/'+script:
    raise ValueError('portable policy script binding unavailable: '+role)
   t=component(doc,probe,'Transform');values=[]
   for key in ('posX','posY','posZ'):
    v=t.get(key)
    # Actual Transform counters are float32; saturated/non-integral values
    # cannot certify a monotonically advancing lifecycle observation.
    if type(v) not in (int,float) or not math.isfinite(v) or v!=int(v) or not 0<=v<16777216:
     raise ValueError('invalid policy lifecycle counter: '+role+'/'+key)
    values.append(int(v))
   starts,updates,resolved=values
   if starts!=1 or updates<=0 or resolved<=0:
    raise ValueError('portable policy lifecycle unavailable: '+role)
   rows.append(values)
  if rows[1][1]<=rows[0][1] or rows[1][2]!=rows[0][2]:
   raise ValueError('portable policy lifecycle did not advance consistently: '+role)
  evidence[role]={'script':'scripts/'+script,'probe':probe,'starts':[r[0] for r in rows],
                  'updates':[r[1] for r in rows],'resolved':[r[2] for r in rows]}
 return evidence

def validate_content_record(record,bundle,policy):
 if not isinstance(record,dict):raise ValueError('content record must be an object')
 actual_hash=sha256_file(bundle)
 if policy=='qa-muted':
  if record.get('qa_content_mute') is not True or record.get('outputs',{}).get(bundle.name)!=actual_hash:
   raise ValueError('actual bundle must match explicit test-owned muted staging manifest')
 elif policy=='normal':
  if record.get('variant')!='normal' or record.get('qa_muted') is not False or record.get('file')!=bundle.name or record.get('sha256')!=actual_hash or type(record.get('bytes')) is not int or record['bytes']!=bundle.stat().st_size:
   raise ValueError('actual bundle must match normal catalog provenance hash, size and content policy')
 else:raise ValueError('unknown content policy')
 return actual_hash


def validate(log,before,after,image,*,qa_content_mute=True):
 if '] ERROR -' in log or 'failed to instantiate' in log or 'no cpp script class' in log:raise ValueError('actual bundle error')
 if qa_content_mute:
  if 'ES_QA_CONTENT_MUTE master_policy=0' not in log:raise ValueError('explicit test-owned muted policy not loaded')
 elif 'ES_QA_CONTENT_MUTE' in log:raise ValueError('normal content loaded QA mute policy')
 # Runtime host script INFO can be stripped in DIST. Do not accept liveness
 # merely by omitting those messages: require the existing actual owner probes
 # and stable Script bindings in both independently saved scene observations.
 lifecycle=validate_policy_lifecycle(before,after)
 boot_log={role:('OBSERVED' if (('es_director online:' if role=='director' else
             f'ES_PORTABLE {role} Lua started') in log) else 'NOT_OBSERVED')
           for role,_,_,_ in POLICY_OWNERS}
 old=component(before,'Ground','MeshRenderer')['albedoTex'];new=component(after,'Ground','MeshRenderer')['albedoTex']
 if old==new or 'rainy' not in new:raise ValueError('actual rainy UI command unavailable')
 if component(before,'flower_0','Transform')==component(after,'flower_0','Transform'):raise ValueError('actual flower sway unavailable')
 if component(before,'Firefly_0','Transform')==component(after,'Firefly_0','Transform'):raise ValueError('actual firefly drift unavailable')
 if component(after,'CampfireLight','Light')['intensity']!=0 or component(after,'CampfireLight2','Light')['intensity']!=0:raise ValueError('rain extinguish policy unavailable')
 body=after.get('scene',after)
 for e in body['entities']:
  for c in e.get('components',[]):
   if c.get('type')=='Script' and not c.get('scriptPath','').endswith('_portable.lua'):raise ValueError('external script driver remains')
   if c.get('type')=='UIText' and c.get('fontPath'):raise ValueError('unverified project font selected')
 w,h,ch,pixels=image
 if w<160 or h<160 or ch<3 or len(pixels)!=w*h*ch or len(set(tuple(pixels[i:i+3]) for i in range(0,len(pixels),ch)))<32:raise ValueError('Game image empty/flat')
 return {'status':'PORTABLE_ES_POLICIES_AND_GAME_IMAGE_OBSERVED','content_policy':'qa-muted' if qa_content_mute else 'normal','policy_lifecycle':lifecycle,'boot_log':boot_log,'lifecycle_channel':'actual serialized policy-owned Transform probes and Script bindings','image_scope':'nonflat readable Game image only; intro/UI can pass this check; not 3D diorama pixels or visual parity','scope':'actual public UI rain, sway/drift/fire policy; '+('QA content volume0' if qa_content_mute else 'normal catalog content, audio output not measured')+', portable Lua policy roles, not original external-language VM or application AI-bootstrap parity'}

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('exe','bundle','out'):p.add_argument('--'+n,type=Path,required=True)
 p.add_argument('--staging-manifest','--content-record',dest='staging_manifest',type=Path,required=True)
 p.add_argument('--content-policy',choices=('qa-muted','normal'),default='qa-muted',help='normal requires catalog provenance; default preserves explicit QA staging')
 p.add_argument('--mode',choices=('full-lifecycle','policy'),default='full-lifecycle',help='default: 900 host frames, 12s Game shot, explicit Stop/restore and complete CPU/resource trace; policy retains legacy 360-frame checks')
 p.add_argument('--terminal-ack',action='store_true',help='require the owned Engine cleanup return and final logger/trace acknowledgement from a rebuilt executable')
 p.add_argument('--backend',choices=('d3d11','d3d12','vulkan','gl'),default='d3d11');p.add_argument('--timeout',type=float,default=240);a=p.parse_args()
 manifest=json.loads(a.staging_manifest.read_text())
 try:validate_content_record(manifest,a.bundle,a.content_policy)
 except ValueError as error:p.error(str(error))
 out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);profile=out/'profile';profile.mkdir();write(profile/'editor-preferences.json',{'startup_mode':1})
 def input_hashes():
  values={'exe':sha256_file(a.exe),'bundle':sha256_file(a.bundle),'staging':sha256_file(a.staging_manifest)}
  if a.mode=='full-lifecycle':values['mood_layout']=sha256_file(MOOD_LAYOUT)
  return values
 hashes=input_hashes()
 events=out/'events.txt';events.write_text(capture_events(a.mode),encoding='utf-8',newline='\n')
 plan=capture_plan(a.mode);env=capture_environment(a.bundle,a.backend,out,a.mode)
 result={'status':'INCOMPLETE','mode':a.mode,'required_terminal_ack':a.terminal_ack,'environment':env,'input_sha256':hashes,'validator_sha256':sha256_file(Path(__file__))};write(out/'intent.json',result)
 try:
  proc=run(env,out/'engine.log',exe=a.exe.resolve(),frames=plan['frames'],timeout=a.timeout,samples_path=out/'frames.csv',require_terminal_ack=a.terminal_ack)
  result['process']=proc
  if proc['process']['returncode']!=0 or proc['process']['timed_out']:raise ValueError('abnormal editor exit')
  log=read_execution_log(out/'engine.log',proc['process']);actual=assert_backend(log,a.backend)
  result['actual_backend']=actual
  if a.mode=='full-lifecycle':
   result['virtual_input']=validate_virtual_events(log)
   result['early_policy_lifecycle']=validate_policy_lifecycle(*[json.loads((out/f'scene-{f}.json').read_text()) for f in (50,220)])
  weather_frames=(400,600) if a.mode=='full-lifecycle' else (50,220)
  result['weather_observation_frames']=weather_frames
  result.update(validate(log,*[json.loads((out/f'scene-{f}.json').read_text()) for f in weather_frames],read_png(out/'game.png'),qa_content_mute=a.content_policy=='qa-muted'))
  if a.mode=='full-lifecycle':
   result['lifecycle']=validate_full_lifecycle(out)
   result['mood']=validate_mood(*[json.loads((out/f'scene-{f}.json').read_text()) for f in (600,800)],events.read_text(encoding='utf-8'))
   result['status']='PORTABLE_ES_FULL_CPU_LIFECYCLE_OBSERVED'
  if hashes!=input_hashes():raise ValueError('inputs changed')
 except Exception as error:result.update(status='FAIL',error=str(error));raise
 finally:result['raw_sha256']={str(v.relative_to(out)):sha256_file(v) for v in out.rglob('*') if v.is_file() and v.name!='result.json'};write(out/'result.json',result)
if __name__=='__main__':main()
