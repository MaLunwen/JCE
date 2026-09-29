"""Author component data through the public automation API, in a changeset."""
import argparse
import json
from pathlib import Path
import sys
PROJECT=Path(__file__).resolve().parents[1]
REPO=PROJECT.parents[1]
sys.path.insert(0,str(REPO/'tools'/'automation'))
from jce_automation import api, scenemodel
from ai_director import validate

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--changeset',required=True)
    ap.add_argument('--proposal',type=Path,required=True)
    args=ap.parse_args()
    cfg=validate(json.loads(args.proposal.read_text(encoding='utf-8')))
    scene='resources/scenes/minesweeper.scene.json'
    existing=json.loads((PROJECT/scene).read_text(encoding='utf-8'))
    names={e['name'] for e in scenemodel.entities_of(existing)}
    def create(name,components,parent=None):
        if name in names:return
        payload={'scene':scene,'name':name,'components':components}
        if parent:payload['parent']=parent
        result=api.call('entity.create',payload,project=str(PROJECT),changeset=args.changeset)
        if not result.get('ok'):raise RuntimeError(json.dumps(result,ensure_ascii=False))
        names.add(name)
    def position(x=0,y=0,z=0):return {'posX':x,'posY':y,'posZ':z}
    create('MainCamera',{'Transform':position(0,0,10),'Camera':{'primary':True,'orthographic':True,'orthoSize':10,'nearClip':0.1,'farClip':100}})
    create('Canvas',{'Transform':{},'Canvas':{'renderMode':0,'sortOrder':80,'refResX':1280,'refResY':720,'matchWidthOrHeight':0.5}})
    for name,xyz in [('InputIntent',(0,0,0)),('CustomSettings',(20,16,50)),('Theme',(32/255,40/255,48/255)),('AiProposal',(cfg['width'],cfg['height'],cfg['mines'])),('AiColor',(cfg['red']/255,cfg['green']/255,cfg['blue']/255)),('AiVersion',(1,0,0))]:
        create(name,{'Transform':position(*xyz)})
    create('MinesController',{'Transform':{},'Script':{'scriptPath':'scripts/MinesController.jcec'}})
    create('Director',{'Transform':{},'Script':{'scriptPath':'scripts/director.lua'}})
    rect={'anchorMinX':0,'anchorMaxX':0,'anchorMinY':1,'anchorMaxY':1,'pivotX':0,'pivotY':1,'anchoredX':-10000,'anchoredY':0,'sizeW':1,'sizeH':1}
    button={'interactable':False,'fadeDuration':0,'onClickHandler':'mines_ui_click'}
    for state in ('normal','highlight','pressed','disabled'):
        for channel in 'RGBA':button[state+channel]=1
    for i in range(530):
        create('Widget%03d'%i,{'Transform':{},'UIImage':dict(rect,colorA=0,raycastTarget=True),'UIText':dict(rect,text='',fontPath='',fontSize=20,colorR=1,colorG=1,colorB=1,colorA=1,alignment=1,verticalAlignment=1),'UIButton':button},'Canvas')
        if i%100==0:print('Authored widgets',i,flush=True)
    print('AUTHORED',len(names),'entities via automation',flush=True)
if __name__=='__main__':main()
