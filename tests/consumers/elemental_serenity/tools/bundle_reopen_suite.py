"""ES scene policy and Game-image witness using explicitly muted QA content."""
import argparse
import json
from pathlib import Path
import sys
ROOT=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT));sys.path.insert(0,str(ROOT/'tests/consumers/benchmark/tools'))
from tools.perf_bench import run,read_execution_log
from tools.jce_determinism import assert_backend,read_png
from gbuffer_asset_suite import sha256_file,write

def component(doc,name,kind):
 body=doc.get('scene',doc)
 matches=[e for e in body['entities'] if e.get('name')==name]
 if len(matches)!=1:raise ValueError('canonical entity unavailable')
 return next(c for c in matches[0]['components'] if c['type']==kind)

def validate(log,before,after,image):
 if '] ERROR -' in log or 'failed to instantiate' in log or 'no cpp script class' in log:raise ValueError('actual bundle error')
 if 'ES_QA_CONTENT_MUTE master_policy=0' not in log:raise ValueError('explicit test-owned muted policy not loaded')
 for role in ('fireflies','campfire','flowers','surfaces'):
  if f'ES_PORTABLE {role} Lua started' not in log:raise ValueError('auxiliary policy boot missing')
 if 'es_director online:' not in log:raise ValueError('director boot missing')
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
 return {'status':'PORTABLE_ES_POLICIES_AND_GAME_IMAGE_OBSERVED','scope':'actual public UI rain, sway/drift/fire policy; QA content volume0, not normal audio or application AI-bootstrap parity'}

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('exe','bundle','staging-manifest','out'):p.add_argument('--'+n,type=Path,required=True)
 p.add_argument('--backend',choices=('d3d11','d3d12','vulkan','gl'),default='d3d11');p.add_argument('--timeout',type=float,default=240);a=p.parse_args()
 manifest=json.loads(a.staging_manifest.read_text())
 if not manifest.get('qa_content_mute') or manifest.get('outputs',{}).get(a.bundle.name)!=sha256_file(a.bundle):p.error('actual bundle must match explicit test-owned muted staging manifest')
 out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);profile=out/'profile';profile.mkdir();write(profile/'editor-preferences.json',{'startup_mode':1})
 events=out/'events.txt';events.write_text('78 M 0.5 0.065277778 0 0\n80 B 0.5 0.065277778 1 1\n82 B 0.5 0.065277778 1 0\n',encoding='utf-8',newline='\n')
 env={'JCE_SCENE':str(a.bundle.resolve()),'JCE_BACKEND':a.backend,'JCE_WINDOW_HIDDEN':'1','JCE_MULTI_INSTANCE':'1','JCE_EDITOR_PROFILE_DIR':str(profile),'JCE_FRAME_DT_FIXED':'0.016666666667','JCE_DBG_AUTOPLAY':'20','JCE_DBG_FOCUS_GAME':'1','JCE_KPI_GAME_EVENTS':str(events),'JCE_KPI_GAME_SHOTS':'3.0|'+str(out/'game.png'),'JCE_KPI_GAME_CAPTURE_SIZE':'1280x720','JCE_EDITOR_OPERATION_METRICS':'1','JCE_DBG_SCENE_OBSERVE_DIR':str(out),'JCE_DBG_SCENE_OBSERVE_AT':'50,220'}
 hashes={'exe':sha256_file(a.exe),'bundle':sha256_file(a.bundle),'staging':sha256_file(a.staging_manifest)};result={'status':'INCOMPLETE','environment':env,'input_sha256':hashes,'validator_sha256':sha256_file(Path(__file__))};write(out/'intent.json',result)
 try:
  proc=run(env,out/'engine.log',exe=a.exe.resolve(),frames=360,timeout=a.timeout,samples_path=out/'frames.csv')
  if proc['process']['returncode']!=0 or proc['process']['timed_out']:raise ValueError('abnormal editor exit')
  log=read_execution_log(out/'engine.log',proc['process']);actual=assert_backend(log,a.backend)
  result.update(validate(log,*[json.loads((out/f'scene-{f}.json').read_text()) for f in (50,220)],read_png(out/'game.png')));result.update(process=proc,actual_backend=actual)
  if hashes!={'exe':sha256_file(a.exe),'bundle':sha256_file(a.bundle),'staging':sha256_file(a.staging_manifest)}:raise ValueError('inputs changed')
 except Exception as error:result.update(status='FAIL',error=str(error));raise
 finally:result['raw_sha256']={str(v.relative_to(out)):sha256_file(v) for v in out.rglob('*') if v.is_file() and v.name!='result.json'};write(out/'result.json',result)
if __name__=='__main__':main()
