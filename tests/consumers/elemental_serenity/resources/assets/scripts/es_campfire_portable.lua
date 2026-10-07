local M={}
local function first(name) return jce.find_by_name(name) end
local function number(text,key,default) return tonumber((text or ''):match('"'..key..'"%s*:%s*([%d%.eE+%-]+)')) or default end
local function probe(self) if self.probe then jce.set_position(self.probe,1,self.frames,self.found) end end
function M:on_start()
 self.frames=0;self.t=0;self.a=first('CampfireLight');self.b=first('CampfireLight2');self.probe=first('EsJavaProbe');self.found=(self.a and 1 or 0)+(self.b and 1 or 0);probe(self);jce.log('ES_PORTABLE campfire Lua started')
end
function M:on_update(dt)
 self.frames=self.frames+1;self.t=self.t+dt
 if self.b then
  local base=self.a and number(jce.comp_get(self.a,'Light'),'intensity',0) or 0
  local t=self.t+1.9;local f=math.sin(t*10)*.5+math.sin(t*23)*.3+math.sin(t*41)*.2
  jce.comp_set(self.b,'Light',string.format('{"lightType":1,"colorR":0.97,"colorG":0.50,"colorB":0.18,"radius":1,"castsShadow":false,"intensity":%.9g}',math.max(0,base*.62*(1+.26*f))))
 end
 probe(self)
end
return M
