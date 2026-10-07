-- Portable Lua policy; original seven-driver project is preserved.
local M={}
local function get(e) return jce.get_position(e) end
local function set(e,x,y,z) jce.set_position(e,x,y,z) end
local function first(n) return assert(jce.find_by_name(n),'missing '..n) end
local function cell(x,z) return math.floor(x+15),math.floor(z+10) end
function M:on_start() self.state=first('GameState');self.head=first('SnakeHead');self.meta=first('GameMeta');self.run=-1;jce.log('SNAKE_PORTABLE board Lua started') end
function M:on_update(dt)
 local state=get(self.state);local _,_,run=get(self.meta);local wall,collision,ate=get(self.entity)
 if run~=self.run then self.run=run;set(self.entity,0,collision,ate);return end
 if state~=1 then return end
 local x,_,z=get(self.head);x,z=cell(x,z);local out=x<0 or x>=30 or z<0 or z>=20
 if wall~=(out and 1 or 0) then set(self.entity,out and 1 or 0,collision,ate) end
end
return M
