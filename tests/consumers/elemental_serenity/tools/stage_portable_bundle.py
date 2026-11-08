"""Bounded test-owned ES staging, then the existing public bundle CLI."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

# The portable director retains the original director's runtime-selected
# season textures/audio. The public packer reads <selected-script>.deps.json;
# this explicit application alias preserves that declaration without scanning
# Lua strings or creating a second dependency resolver.
PORTABLE_DEPENDENCY_SOURCES = {
 'scripts/es_director_portable.lua': 'scripts/es_director.lua.deps.json',
}

def _portable_dependency_files(source,files):
 declarations=[]
 for _,rel in files:
  if rel.parts[0]!='scripts' or not rel.name.endswith('_portable.lua'):continue
  selected=rel.as_posix();sidecar=rel.with_name(rel.name+'.deps.json')
  own=source/sidecar
  if own.is_file():declarations.append((own,sidecar));continue
  declared=PORTABLE_DEPENDENCY_SOURCES.get(selected)
  if declared:
   original=source/declared
   if not original.is_file():raise ValueError(f'required portable dependency declaration missing: selected_script={selected} source_sidecar={declared} staged_sidecar={sidecar.as_posix()}')
   declarations.append((original,sidecar))
 return declarations

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

PIPELINE_SOURCE='Settings/RenderPipeline.rp.json'
PIPELINE_ADDRESS='settings/render_pipeline.rp.json'

def project_settings(project):
 project=project.resolve()
 if not project.is_dir():raise ValueError('explicit project directory missing')
 path=project/PIPELINE_SOURCE
 try:path.lstat()
 except FileNotFoundError:return {}
 for part in (path.parent,path):
  if part.is_symlink() or getattr(part.lstat(),'st_file_attributes',0)&1024:raise ValueError('reparse project settings rejected')
 if not path.is_file():raise ValueError('project render pipeline must be a regular file')
 size=path.stat().st_size
 if not 0<size<=1048576:raise ValueError('project render pipeline requires 1..1048576 bytes')
 with path.open('rb') as stream:data=stream.read(1048577)
 if not 0<len(data)<=1048576:raise ValueError('project render pipeline changed beyond its byte bound')
 if not isinstance(json.loads(data),dict):raise ValueError('project render pipeline must contain a JSON object')
 return {PIPELINE_ADDRESS:{'source_path':PIPELINE_SOURCE,'source_sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data)}}

def verify_project_settings_bom(project,inputs,documents):
 if project_settings(project)!=inputs:raise ValueError('project settings changed during export')
 witness={}
 for address,expected in inputs.items():
  entries=[item for doc in documents for item in doc['assets'] if item['path']==address]
  if len(entries)!=1:raise ValueError('canonical project render pipeline missing or duplicated in actual bundle BOM')
  entry=entries[0]
  if entry.get('size')!=expected['bytes'] or not isinstance(entry.get('content_id'),str) or len(entry['content_id'])!=16 or any(c not in '0123456789abcdefABCDEF' for c in entry['content_id']):
   raise ValueError('canonical project render pipeline BOM size/hash invalid')
  witness[address]={'source_sha256':expected['source_sha256'],'bundle_content_id':entry['content_id'],'bytes':entry['size'],'scope':'BOM identity; archive payload and runtime application require separate native validation'}
 return witness
def _capture_scene(path,seeds,qa_mute):
 doc=json.loads(path.read_text());body=doc.get('scene',doc)
 if seeds is not None:
  directors=[c for e in body['entities'] for c in e.get('components',[]) if c.get('type')=='Script' and c.get('scriptPath')=='scripts/es_director_portable.lua']
  if len(directors)!=1:raise ValueError('capture requires exactly one portable Director Script')
  director=directors[0];params=director.get('params',[])
  if not isinstance(params,list) or any(not isinstance(p,dict) or not isinstance(p.get('name'),str) for p in params):raise ValueError('invalid Director Script params')
  if len({p['name'] for p in params})!=len(params):raise ValueError('duplicate Director Script parameter')
  params=[p for p in params if p['name'] not in ('capture_seed_a','capture_seed_b')]
  if len(params)>6:raise ValueError('capture seeds exceed the eight Script parameter slots')
  params.extend({'name':name,'kind':0,'number':seed,'entity':0,'text':''} for name,seed in zip(('capture_seed_a','capture_seed_b'),seeds))
  director['params']=params
 if qa_mute:
  for e in body['entities']:
   for c in e.get('components',[]):
    if c.get('type')=='AudioSource':c['volume']=0
 return json.dumps(doc,indent=2)+'\n'

def stage(source,out,max_bytes,qa_mute=False,capture_seeds=None):
 if capture_seeds is not None and (not isinstance(capture_seeds,(tuple,list)) or len(capture_seeds)!=2 or any(type(s) is not int or not 0<=s<=16777215 for s in capture_seeds)):
  raise ValueError('capture seeds require two integers in [0,16777215]')
 source=source.resolve();out=out.resolve()
 if out==source or source in out.parents:raise ValueError('staging output must be outside source assets')
 allowed={'models','textures','particles','audio','materials'};files=[]
 for path in source.rglob('*'):
  if path.is_symlink() or getattr(path.lstat(),'st_file_attributes',0)&1024:raise ValueError('reparse resource rejected')
  if not path.is_file():continue
  rel=path.relative_to(source)
  if rel.as_posix() in ('models/ground_plane.glb','textures/water_data.png') or rel.as_posix().startswith('textures/ground_baked/'):continue
  keep=(rel.parts[0] in allowed or str(rel).replace('\\','/')=='scenes/elemental_serenity.bundle.scene.json'
        or rel.parts[0]=='scripts' and path.name.endswith('_portable.lua') or rel.name in ('render_settings.json','bundle_roots.json')
        or rel.as_posix() in ('scene_ai/elemental_catalog.json','scene_ai/bootstrap_recipes.json'))
  if keep:files.append((path,rel))
 files.extend(_portable_dependency_files(source,files))
 if sum(p.stat().st_size for p,_ in files)>max_bytes:raise ValueError('bounded resource budget exceeded')
 scene_text=None
 if qa_mute or capture_seeds is not None:
  scene_text=_capture_scene(source/'scenes/elemental_serenity.bundle.scene.json',capture_seeds,qa_mute)
 out.mkdir(parents=True,exist_ok=False);manifest={}
 for path,rel in files:
  dest=out/rel;dest.parent.mkdir(parents=True,exist_ok=True)
  if rel.name=='bundle_roots.json':
   doc=json.loads(path.read_text());doc['assets']=[x for x in doc.get('assets',[]) if not (x if isinstance(x,str) else x.get('path','')).startswith('fonts/')]
   dest.write_text(json.dumps(doc,indent=2)+'\n',encoding='utf-8',newline='\n')
  elif qa_mute and rel.name=='es_director_portable.lua':
   text=path.read_text();assert text.count('local MASTER = 0.55')==1
   text=text.replace('local MASTER = 0.55',"local MASTER = 0.0 -- test-owned content mute, not normal bundle policy")
   text=text.replace('jce.play_sound("audio/sounds/ui_interactions/click.mp3", 0.25)', 'jce.play_sound("audio/sounds/ui_interactions/click.mp3", 0.0)')
   if 'click.mp3", 0.25' in text:raise ValueError('unmuted UI one-shot remains')
   text=text.replace('function script.on_start(self)',"function script.on_start(self)\n  jce.log('ES_QA_CONTENT_MUTE master_policy=0')")
   if 'ES_QA_CONTENT_MUTE' not in text:raise ValueError('director lifecycle selector changed')
   dest.write_text(text,encoding='utf-8',newline='\n')
  elif scene_text is not None and rel.name=='elemental_serenity.bundle.scene.json':
   dest.write_text(scene_text,encoding='utf-8',newline='\n')
  else:shutil.copyfile(path,dest)
  manifest[rel.as_posix()]={'source_path':path.relative_to(source).as_posix(),'source_sha256':sha(path),'staged_sha256':sha(dest),'bytes':dest.stat().st_size}
 if not (out/'scenes/elemental_serenity.bundle.scene.json').is_file():raise ValueError('selected entry missing')
 return manifest

def export_command(packer,assets,out,target,project):
 selected=(assets/'scenes/elemental_serenity.bundle.scene.json').resolve()
 if not selected.is_file():raise ValueError('actual selected scene missing before export')
 if not project.is_dir():raise ValueError('explicit project directory missing')
 return [str(packer.resolve()),'--project',str(project.resolve()),'--resource-root',str(assets.resolve()),'--scene',str(selected),'--out',str(out.resolve()),'--target',target,'--single-file','--id','elemental_serenity_portable','--cook-assets']

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--project',type=Path,required=True,help='authored project owning Settings/RenderPipeline.rp.json');p.add_argument('--resource-root',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--packer',type=Path,required=True);p.add_argument('--qa-mute',action='store_true',help='explicit test-owned zero-volume content, not normal application audio validation');p.add_argument('--capture-seeds',nargs=2,type=int,metavar=('A','B'),help='opt-in Lua VM capture seed pair, each 0..16777215; normal randomness is unchanged when absent');p.add_argument('--max-bytes',type=int,default=1073741824);p.add_argument('--target',default='windows',choices=('windows','linux','macos','android','ios'));a=p.parse_args()
 if not a.packer.is_file() or a.max_bytes<=0:p.error('existing packer and positive budget required')
 if a.capture_seeds is not None and any(not 0<=s<=16777215 for s in a.capture_seeds):p.error('capture seeds must be in [0,16777215]')
 settings=project_settings(a.project);settings_bytes=sum(item['bytes'] for item in settings.values())
 if settings_bytes>=a.max_bytes:p.error('bounded resource budget exceeded by project settings')
 root=a.out.resolve();root.mkdir(parents=True,exist_ok=False);assets=root/'resources';manifest=stage(a.resource_root,assets,a.max_bytes-settings_bytes,a.qa_mute,a.capture_seeds)
 command=export_command(a.packer,assets,root/'bundles',a.target,a.project)
 record={'qa_content_mute':a.qa_mute,'capture_seeds':a.capture_seeds,'capture_rng_scope':'shared Lua VM only; native particles, GPU and frame scheduling remain independent','scope':'local validation only; per-asset rights unresolved; no fonts/private workflows/cache trees copied','inputs':manifest,'project_root':str(a.project.resolve()),'project_settings':settings,'packer_sha256':sha(a.packer),'command':command}
 (root/'staging-manifest.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8',newline='\n')
 proc=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace',check=False);(root/'export.log').write_text(proc.stdout,encoding='utf-8',newline='\n')
 record['export_returncode']=proc.returncode
 (root/'staging-manifest.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8',newline='\n')
 if proc.returncode:raise RuntimeError('public bundle CLI failed; original inputs unchanged')
 outputs=list((root/'bundles').glob('*.jbundle.json'))
 if not outputs:raise ValueError('actual bundle dependency manifest missing')
 documents=[json.loads(path.read_text()) for path in outputs]
 for doc in documents:assert all(not x['path'].startswith('fonts/') for x in doc['assets']),'font leaked into actual graph'
 record['project_settings_bom']=verify_project_settings_bom(a.project,settings,documents)
 record['outputs']={p.name:sha(p) for p in (root/'bundles').iterdir() if p.is_file()}
 (root/'staging-manifest.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8',newline='\n')
if __name__=='__main__':main()
