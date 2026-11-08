"""Serialized ES Mood UI/state nodes shared by authored and portable scenes."""
STATES = ("spring_day", "spring_night", "winter_day", "winter_night",
          "autumn_day", "autumn_night", "rainy_day", "rainy_night")
def append_mood_nodes(entities):
 if any(e.get('name') == 'MoodRequest' for e in entities):
  raise ValueError('Mood nodes already exist; do not duplicate serialized owner')
 canvases=[e for e in entities if e.get('name')=='HudCanvas']
 if len(canvases)!=1:raise ValueError('unique existing HudCanvas required')
 canvas=canvases[0]['id'];nextid=max(e['id'] for e in entities)+1
 def transform(pos=(0,0,0)):
  return dict(type='Transform',posX=pos[0],posY=pos[1],posZ=pos[2],rotX=0,rotY=0,rotZ=0,scaleX=1,scaleY=1,scaleZ=1)
 def add(name,comps,parent=0):
  nonlocal nextid
  entities.append(dict(id=nextid,name=name,parentId=parent,components=[transform()]+comps+[dict(type='EditorMeta',name=name)]));nextid+=1
 add('MoodRequest',[dict(type='Script',scriptPath='scripts/es_mood_portable.lua')])
 add('MoodPlan',[]);add('MoodAck',[]);add('MoodProvider',[])
 def rect(x,y,w,h):return dict(anchorMinX=1,anchorMinY=0,anchorMaxX=1,anchorMaxY=0,pivotX=1,pivotY=0,anchoredX=x,anchoredY=y,sizeW=w,sizeH=h)
 def text(value,x,y,w,h):return dict(type='UIText',text=value,fontPath='',fontSize=13,alignment=0,colorR=.96,colorG=.96,colorB=.94,colorA=1,**rect(x,y,w,h))
 add('MoodStatus',[text('READY | authored preset provider',-14,18,490,42)],canvas)
 buttons=[('MoodNext','Next preset','es_mood_next',-14,66,118)]
 for i,state in enumerate(STATES):
  buttons.append(('MoodState'+str(i),state.replace('_',' / '),'es_mood_'+state,-14-(3-i%4)*120,102+(i//4)*30,114))
 for name,label,handler,x,y,w in buttons:
  image=dict(type='UIImage',colorR=.07,colorG=.09,colorB=.11,colorA=.86,**rect(x,y,w,27))
  button=dict(type='UIButton',onClickHandler=handler,normalR=.07,normalG=.09,normalB=.11,normalA=.86,highlightR=.19,highlightG=.24,highlightB=.29,highlightA=.95,pressedR=.04,pressedG=.06,pressedB=.08,pressedA=1)
  add(name,[image,button,text(label,x-4,y+3,w-8,22)],canvas)
 return entities
