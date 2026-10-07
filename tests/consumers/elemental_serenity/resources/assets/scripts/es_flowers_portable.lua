local M={}
local function first(name) return jce.find_by_name(name) end
local function number(text,key,default) return tonumber((text or ''):match('"'..key..'"%s*:%s*([%d%.eE+%-]+)')) or default end
local function probe(self) if self.probe then jce.set_position(self.probe,1,self.frames,self.found) end end
function M:on_start()
 self.frames=0;self.t=0;self.rows={};self.probe=first('EsCppProbe')
 for _,e in ipairs(jce.find_by_prefix('flower_')) do local x,y,z=jce.get_rotation(e);local px=jce.get_position(e);self.rows[#self.rows+1]={e,x or 0,y or 0,z or 0,(px or 0)*.11} end
 self.found=#self.rows;probe(self);jce.log('ES_PORTABLE flowers Lua started')
end
function M:on_update(dt)
 self.frames=self.frames+1;self.t=self.t+dt
 for _,q in ipairs(self.rows) do jce.set_rotation(q[1],q[2],q[3]+3*math.sin(self.t*.87+q[5]),q[4]+5.5*math.sin(self.t*1.35+q[5])) end
 probe(self)
end
return M
