"""Rebuild the reference ground asset from measured geometry, using the formal importer."""
from pathlib import Path
import json,subprocess
PROJECT=Path(__file__).resolve().parents[1]
REPO=PROJECT.parents[2]
g=json.loads((PROJECT/'ground-reference.json').read_text())
out=PROJECT/'build/ground.obj'
lines=['# Reference BoxGeometry after computeVertexNormals; basis x,z,-y.','o Ground']
for face,uvs in zip(g['faces'],g['uvs']):
    start=(len(lines)-2)//3+1
    for k,uv in zip(['a','b','c'],uvs):
        p=g['vertices'][face[k]];n=face['vertexNormals'][['a','b','c'].index(k)]
        lines += [f"v {p['x']} {p['z']} {-p['y']}",f"vt {uv['x']} {uv['y']}",f"vn {n['x']} {n['z']} {-n['y']}"]
# OBJ indices track each v/vt/vn independent list, one triangle per measured face.
for i in range(12):
    a=i*3+1;lines.append('f '+' '.join(f'{j}/{j}/{j}' for j in range(a,a+3)))
out.write_text('\n'.join(lines)+'\n',encoding='utf-8',newline='\n')
subprocess.run([str(REPO/'build/host/tools/jce_cook.exe'),'--convert-model',str(out),'--geometry-only','--out',str(PROJECT/'resources/models/ground.glb')],check=True)
