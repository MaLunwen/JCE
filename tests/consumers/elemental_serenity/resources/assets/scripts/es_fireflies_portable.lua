local M={}
local function first(name) return jce.find_by_name(name) end
local function number(text,key,default) return tonumber((text or ''):match('"'..key..'"%s*:%s*([%d%.eE+%-]+)')) or default end
local function probe(self) if self.probe then jce.set_position(self.probe,1,self.frames,self.found) end end
function M:on_start()
 self.frames=0;self.t=0;self.rows={};self.probe=first('EsPyProbe')
 for i,e in ipairs(jce.find_by_prefix('Firefly_')) do local x,y,z=jce.get_position(e);self.rows[#self.rows+1]={e,x or 0,y or 0,z or 0,((i-1)*2.399963)%6.283185} end
 self.found=#self.rows;probe(self);jce.log('ES_PORTABLE fireflies Lua started')
end
function M:on_update(dt)
 self.frames=self.frames+1;self.t=self.t+dt
 for _,q in ipairs(self.rows) do local ph=q[5];local t=self.t
  jce.set_position(q[1],q[2]+.55*math.sin(t*.42+ph),q[3]+.30*math.sin(t*.83+ph*1.7),q[4]+.55*math.cos(t*.42+ph))
  local k=.5-.5*math.cos(t*1.7+ph);jce.particle_set_color(q[1],.42+.58*k,.55+.39*k,.16+.26*k)
 end
 probe(self)
end
return M
