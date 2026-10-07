-- Portable Lua policy; original seven-driver project is preserved.
local M={}
local function get(e) return jce.get_position(e) end
local function set(e,x,y,z) jce.set_position(e,x,y,z) end
local function first(n) return assert(jce.find_by_name(n),'missing '..n) end
local function cell(x,z) return math.floor(x+15),math.floor(z+10) end
local function spawn(self)
 local occupied={};local function mark(e) local x,y,z=get(e);if y>-50 then x,z=cell(x,z);occupied[z*30+x]=true end end
 mark(self.head);for _,e in ipairs(self.body) do mark(e) end
 local free={};for z=0,19 do for x=0,29 do if not occupied[z*30+x] then free[#free+1]={x,z} end end end
 if #free==0 then jce.log('SNAKE_PORTABLE no free cell left');return false end
 self.seed=(self.seed*1103515245+12345)&0x7fffffff;local q=free[self.seed%#free+1];local _,y=get(self.entity);set(self.entity,q[1]-14.5,y,q[2]-9.5);return true
end
M.spawn=spawn
function M:on_start()
 self.state=first('GameState');self.meta=first('GameMeta');self.head=first('SnakeHead');self.verdict=first('Verdict');self.body={};for i=0,63 do self.body[i+1]=first(string.format('Body%02d',i)) end;self.run=-1;self.seed=12345;jce.log('SNAKE_PORTABLE food Lua started')
end
function M:on_update(dt)
 local state,score,tick=get(self.state);local best,len,run=get(self.meta)
 if run~=self.run then self.run=run;self.seed=12345+math.floor(run)*7919;spawn(self);return end
 if state~=1 then return end
 local hx,_,hz=get(self.head);local fx,_,fz=get(self.entity);hx,hz=cell(hx,hz);fx,fz=cell(fx,fz)
 if hx~=fx or hz~=fz then return end
 set(self.state,state,score+10,tick);set(self.meta,best,len+1,run);local wall,collision=get(self.verdict);set(self.verdict,wall,collision,1);spawn(self)
end
return M
