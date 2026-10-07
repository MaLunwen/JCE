"""Bounded test-owned ES staging, then the existing public bundle CLI."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def stage(source,out,max_bytes,qa_mute=False):
 source=source.resolve();out=out.resolve()
 if out==source or source in out.parents:raise ValueError('staging output must be outside source assets')
 allowed={'models','textures','particles','audio','materials'};files=[]
 for path in source.rglob('*'):
  if path.is_symlink() or getattr(path.lstat(),'st_file_attributes',0)&1024:raise ValueError('reparse resource rejected')
  if not path.is_file():continue
  rel=path.relative_to(source)
  if rel.as_posix() in ('models/ground_plane.glb','textures/water_data.png') or rel.as_posix().startswith('textures/ground_baked/'):continue
  keep=(rel.parts[0] in allowed or str(rel).replace('\\','/')=='scenes/elemental_serenity.bundle.scene.json'
        or rel.parts[0]=='scripts' and path.name.endswith('_portable.lua') or rel.name in ('render_settings.json','bundle_roots.json'))
  if keep:files.append((path,rel))
 if sum(p.stat().st_size for p,_ in files)>max_bytes:raise ValueError('bounded resource budget exceeded')
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
  elif qa_mute and rel.name=='elemental_serenity.bundle.scene.json':
   doc=json.loads(path.read_text());body=doc.get('scene',doc)
   for e in body['entities']:
    for c in e.get('components',[]):
     if c.get('type')=='AudioSource':c['volume']=0
   dest.write_text(json.dumps(doc,indent=2)+'\n',encoding='utf-8',newline='\n')
  else:shutil.copyfile(path,dest)
  manifest[rel.as_posix()]={'source_sha256':sha(path),'staged_sha256':sha(dest),'bytes':dest.stat().st_size}
 if not (out/'scenes/elemental_serenity.bundle.scene.json').is_file():raise ValueError('selected entry missing')
 return manifest

def export_command(packer,assets,out,target):
 selected=(assets/'scenes/elemental_serenity.bundle.scene.json').resolve()
 if not selected.is_file():raise ValueError('actual selected scene missing before export')
 return [str(packer.resolve()),'--resource-root',str(assets.resolve()),'--scene',str(selected),'--out',str(out.resolve()),'--target',target,'--single-file','--id','elemental_serenity_portable','--cook-assets']

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--resource-root',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--packer',type=Path,required=True);p.add_argument('--qa-mute',action='store_true',help='explicit test-owned zero-volume content, not normal application audio validation');p.add_argument('--max-bytes',type=int,default=1073741824);p.add_argument('--target',default='windows',choices=('windows','linux','macos','android','ios'));a=p.parse_args()
 if not a.packer.is_file() or a.max_bytes<=0:p.error('existing packer and positive budget required')
 root=a.out.resolve();root.mkdir(parents=True,exist_ok=False);assets=root/'resources';manifest=stage(a.resource_root,assets,a.max_bytes,a.qa_mute)
 command=export_command(a.packer,assets,root/'bundles',a.target)
 record={'qa_content_mute':a.qa_mute,'scope':'local validation only; per-asset rights unresolved; no fonts/private workflows/cache trees copied','inputs':manifest,'packer_sha256':sha(a.packer),'command':command}
 (root/'staging-manifest.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8',newline='\n')
 proc=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace',check=False);(root/'export.log').write_text(proc.stdout,encoding='utf-8',newline='\n')
 record['export_returncode']=proc.returncode
 (root/'staging-manifest.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8',newline='\n')
 if proc.returncode:raise RuntimeError('public bundle CLI failed; original inputs unchanged')
 outputs=list((root/'bundles').glob('*.jbundle.json'))
 if not outputs:raise ValueError('actual bundle dependency manifest missing')
 for path in outputs:
  doc=json.loads(path.read_text());assert all(not x['path'].startswith('fonts/') for x in doc['assets']),'font leaked into actual graph'
 record['outputs']={p.name:sha(p) for p in (root/'bundles').iterdir() if p.is_file()}
 (root/'staging-manifest.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8',newline='\n')
if __name__=='__main__':main()
