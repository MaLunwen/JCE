"""Generate bounded public component data; validate with jce_scene_kit before cooking."""
import json
import math
import shutil
from pathlib import Path
import struct
import subprocess
import sys
import zlib

PROJECT = Path(__file__).resolve().parents[1]
REPO = PROJECT.parents[2]

def main():
    assets = PROJECT/'resources'
    # Original Three.js disables gammaInput/gammaOutput: its mip averages
    # operate on stored values. Preserve bytes and express that in import data.
    for folder in ['textures','ui']:
        for image in (assets/folder).glob('*'):
            if image.suffix.lower() in ('.png','.jpg','.jpeg'):
                Path(str(image)+'.import.json').write_text('{"colorSpace":"linear"}\n',encoding='utf-8',newline='\n')
    (assets/'scenes').mkdir(parents=True, exist_ok=True)
    (assets/'scripts').mkdir(exist_ok=True)
    (assets/'textures').mkdir(exist_ok=True)
    (assets/'Settings').mkdir(exist_ok=True)
    shutil.copyfile(PROJECT/'Settings/RenderPipeline.rp.json',assets/'Settings/RenderPipeline.rp.json')
    def chunk(kind, data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    white = b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1,1,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0\xff\xff\xff\xff'))+chunk(b'IEND',b'')
    (assets/'textures/white.png').write_bytes(white)
    (assets/'scripts/TdController.jcec').write_text('// Compiled C99 scene class TdController. See src/controller.c.\n',encoding='utf-8',newline='\n')
    entities = []
    def add(name, components, parent=0):
        ident = len(entities)+1
        entities.append({'id':ident, 'name':name, 'parentId':parent,
                         'components':[dict(type=kind,**value) for kind,value in components.items()]})
        return ident
    def transform(x=0,y=0,z=0,sx=1,sy=1,sz=1,rx=0,ry=0):
        return dict(posX=x,posY=y,posZ=z,scaleX=sx,scaleY=sy,scaleZ=sz,rotX=rx,rotY=ry,rotZ=0)
    def mesh(color=(1,1,1), path='', texture='textures/white.png', visible=True):
        return dict(meshPath=path,meshShape=0,albedoTex=texture,visible=visible,
                    baseColorR=color[0],baseColorG=color[1],baseColorB=color[2],baseColorA=1,
                    metallic=0,roughness=0.85,castsShadow=True,receivesShadow=True,materialPath='materials/classic.mat.json',materialOverrides=127)
    add('MainCamera',{'Transform':transform(60,80,110,rx=-math.degrees(math.atan2(80,math.hypot(60,110))),ry=math.degrees(math.atan2(60,110))),
                      'Camera':dict(primary=True,orthographic=False,fov=40,nearClip=0.1,farClip=1000,clearMode=1)})
    add('Sun',{'Transform':transform(),'DirectionalLight':dict(dirX=66/math.sqrt(66**2+84**2+60**2),dirY=-84/math.sqrt(66**2+84**2+60**2),dirZ=60/math.sqrt(66**2+84**2+60**2),intensity=.7,colorR=1,colorG=.96,colorB=.9,castsShadow=True)})
    ground=mesh(path='models/ground.glb',texture='textures/level-01_COLOR.png')
    ground.update(metallic=1/15,roughness=1,mrTex='textures/level-01_SPEC.png')
    add('Ground',{'Transform':transform(0,-.6,0),'MeshRenderer':ground})
    for name,x,z,color in [('Start',-45,45,(30/255,1,0)),('End',45,-45,(1,0,0))]:
        marker=mesh(color);marker.update(meshShape=2,materialPath='materials/unlit.mat.json',alphaMode=2,baseColorA=.4,castsShadow=False,receivesShadow=False)
        add(name,{'Transform':transform(x,0,z,9,1,9),'MeshRenderer':marker})
    selection=mesh(visible=False)
    selection.update(meshShape=2,alphaMode=2,baseColorA=.2,roughness=1-30/128,castsShadow=False,receivesShadow=False)
    add('Selection',{'Transform':transform(0,.001,0,9,1,9),'MeshRenderer':selection})
    add('TdController',{'Transform':transform(),'Script':{'scriptPath':'scripts/TdController.jcec'}})
    # Runtime entities are instantiated lazily through the public prefab host.
    prefabs = assets/'prefabs'; prefabs.mkdir(exist_ok=True)
    def prefab(name, components):
        doc={'contract':{'name':'jce.scene','major':1,'minor':0},
             'scene':{'version':1,'entities':[{'id':1,'name':name,'parentId':0,
                      'components':[dict(type=kind,**value) for kind,value in components.items()]}]}}
        (prefabs/(name+'.prefab.json')).write_text(json.dumps(doc,indent=2)+'\n',encoding='utf-8',newline='\n')
    for kind in range(3):
        image=f'textures/tower-0{kind+1}.jpg'
        body=mesh(path=f'models/tower-0{kind+1}.glb',texture=image)
        body.update(emissiveTex=image,emissiveR=1 if kind==1 else .808,
                    emissiveG=.992 if kind==1 else .808,emissiveB=.91 if kind==1 else .808)
        prefab(f'tower-{kind}',{'Transform':transform(),'MeshRenderer':body})
    for kind in range(2):
        body=mesh(path='models/ufo.glb' if kind else 'models/ghost.glb',
                  texture='textures/ufo-yellow.jpg' if kind else 'textures/ghost.png')
        prefab(f'enemy-{kind}',{'Transform':transform(),'MeshRenderer':body})
        prefab(f'bullet-{kind}',{'Transform':transform(),'BillboardRenderer':dict(
            texturePath=f'textures/bullet-0{kind+1}.png',mode=0,sizeX=1,sizeY=1,
            colorR=1,colorG=1,colorB=1,colorA=1,visible=True)})
    blood=mesh(texture='textures/blood-128.png')
    blood.update(meshShape=2,materialPath='materials/unlit.mat.json',alphaMode=2,castsShadow=False,receivesShadow=False)
    prefab('blood',{'Transform':transform(sx=4,sy=1,sz=4),'MeshRenderer':blood})
    dependencies={'contract':{'name':'jce.bundle.dependencies','major':1,'minor':0},
                  'assets':(['prefabs/'+p.name for p in sorted(prefabs.glob('*.prefab.json'))]
                            + ['ui/'+p.name for p in sorted((assets/'ui').glob('*.png'))]
                            + ['textures/'+p.name for p in sorted((assets/'textures').glob('*')) if p.suffix.lower() in ('.png','.jpg','.jpeg')]
                            + ['licenses/'+p.name for p in sorted((assets/'licenses').glob('*')) if p.is_file()])}
    (assets/'scripts/TdController.jcec.deps.json').write_text(json.dumps(dependencies,indent=2)+'\n',encoding='utf-8',newline='\n')
    canvas = add('Canvas',{'Transform':transform(),'Canvas':dict(renderMode=0,sortOrder=80,refResX=1280,refResY=720,matchWidthOrHeight=0.5)})
    rectangle = dict(anchorMinX=0,anchorMaxX=0,anchorMinY=1,anchorMaxY=1,pivotX=0,pivotY=1,
                     anchoredX=-10000,anchoredY=0,sizeW=1,sizeH=1)
    button = dict(interactable=False,fadeDuration=0,onClickHandler='td_ui_click')
    for state in ('normal','highlight','pressed','disabled'):
        for channel in 'RGB': button[state+channel] = 1
        button[state+'A'] = 0
    for i in range(96):
        add(f'Hud{i:04}',{'Transform':transform(),'UIImage':dict(rectangle,colorA=0,raycastTarget=True),
                          'UIText':dict(rectangle,text='',fontPath='',fontSize=20,colorR=1,colorG=1,colorB=1,colorA=1,alignment=1,verticalAlignment=1),'UIButton':button},canvas)
    rendering=dict(version=1,ambient=dict(color=[243/255,253/255,1],intensity=.6),
                   fog=dict(enabled=False),iblEnabled=False,dynamicGI=0,
                   shadows=dict(distance=250,cascades=1,resolution=512,soft=1),
                   postfx=dict(tonemap=False,bloom=False,fxaa=False,vignette=False,chromatic=False,
                               grayscale=False,custom=False,ssao=False,ssr=False,exposure=1,gamma=1),
                   environment=dict(timeOfDay=dict(enabled=False),sky=dict(mode=0,cloudCoverage=0)),
                   look=dict(wrap=0,hemisphere=True,groundColor=[247/255,1,243/255],rimIntensity=0,autoExposure=False,dof=False))
    scene = assets/'scenes/td_lab.scene.json'
    scene.write_text(json.dumps({'contract':{'name':'jce.scene','major':1,'minor':0},'scene':{'version':1,'entities':entities,'rendering':rendering}},indent=2)+'\n',encoding='utf-8',newline='\n')
    print('AUTHORED',len(entities),'public component entities')
    subprocess.run([sys.executable,str(REPO/'tools/jce_scene_kit.py'),'check',str(scene),'--assets-root',str(assets)],check=True)

if __name__ == '__main__':
    main()
