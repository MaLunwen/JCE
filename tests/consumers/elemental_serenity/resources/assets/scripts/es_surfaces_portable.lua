local M={}
local function first(name) return jce.find_by_name(name) end
local function number(text,key,default) return tonumber((text or ''):match('"'..key..'"%s*:%s*([%d%.eE+%-]+)')) or default end
local function probe(self) if self.probe then jce.set_position(self.probe,1,self.frames,self.found) end end
local palettes={
{"textures/ground_baked2/spring_day.png",0.965000,0.840000,0.480000},
{"textures/ground_baked2/spring_night.png",0.202500,0.172500,0.094500},
{"textures/ground_baked2/winter_day.png",0.875000,0.925000,0.975000},
{"textures/ground_baked2/winter_night.png",0.202500,0.217500,0.232500},
{"textures/ground_baked2/autumn_day.png",0.925000,0.725000,0.525000},
{"textures/ground_baked2/autumn_night.png",0.202500,0.157500,0.112500},
{"textures/ground_baked2/rainy_day.png",0.575000,0.525000,0.475000},
{"textures/ground_baked2/rainy_night.png",0.112500,0.106500,0.097500},
}
local function state(self)
 local text=self.ground and jce.comp_get(self.ground,'MeshRenderer') or ''
 for i,p in ipairs(palettes) do if text:find(p[1],1,true) then return i end end
 return nil
end
local function chase(v,target,tau,dt) return v+(target-v)*(1-math.exp(-dt/tau)) end
function M:on_start()
 self.frames=0;self.poll=0;self.probe=first('EsCProbe');self.ground=first('Ground');self.props={};self.state=state(self);self.wet=self.state and self.state>=7 and 1 or 0
 self.tint=self.state and {palettes[self.state][2],palettes[self.state][3],palettes[self.state][4]} or {0,0,0}
 for _,name in ipairs({'Rocks','Trees','Bridge','Camp','Tent'}) do local e=first(name);local text=e and jce.comp_get(e,'MeshRenderer');if text then local q={e=e,stone=name=='Rocks',base={},last={}};for i,key in ipairs({'baseColorR','baseColorG','baseColorB'}) do q.base[i]=number(text,key,1);q.last[i]=q.base[i] end;q.rough=number(text,'roughness',1);q.last[4]=q.rough;self.props[#self.props+1]=q end end
 self.found=#self.props;probe(self);jce.log('ES_PORTABLE surfaces Lua started')
end
function M:on_update(dt)
 self.frames=self.frames+1;self.poll=self.poll+dt
 if self.poll>=.25 then self.poll=0;self.state=state(self) or self.state end
 if self.state then
  local target=self.state>=7 and 1 or 0;self.wet=chase(self.wet,target,target>self.wet and 3.5 or 14,dt)
  for i=1,3 do self.tint[i]=chase(self.tint[i],palettes[self.state][i+1],1.1,dt) end
  for _,q in ipairs(self.props) do local v={};local changed=false
   for i=1,3 do v[i]=(q.stone and self.tint[i] or q.base[i])*(1-.34*self.wet) end;v[4]=q.rough+(.34-q.rough)*self.wet
   for i=1,4 do if math.abs(v[i]-q.last[i])>1e-4 then changed=true end end
   if changed and jce.comp_set(q.e,'MeshRenderer',string.format('{"baseColorR":%.9g,"baseColorG":%.9g,"baseColorB":%.9g,"roughness":%.9g}',table.unpack(v))) then q.last=v end
  end
 end
 probe(self)
end
return M
