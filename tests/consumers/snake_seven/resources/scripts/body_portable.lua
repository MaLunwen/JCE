-- Portable Lua policy; original seven-driver project is preserved.
local M={}
local function get(e) return jce.get_position(e) end
local function set(e,x,y,z) jce.set_position(e,x,y,z) end
local function first(n) return assert(jce.find_by_name(n),'missing '..n) end
local function cell(x,z) return math.floor(x+15),math.floor(z+10) end
local function park(self,n) for i=n+1,64 do set(self.body[i],0,-100,0) end end
function M:on_start()
 self.head=first('SnakeHead');self.prev=first('PrevHead');self.state=first('GameState');self.meta=first('GameMeta');self.verdict=first('Verdict');self.diag=first('CppDiag');self.body={};self.history={};self.lasttick=-1;self.run=-1
 for i=0,63 do self.body[i+1]=first(string.format('Body%02d',i)) end;set(self.diag,99,0,0);jce.log('SNAKE_PORTABLE body Lua started')
end
function M:on_update(dt)
 set(self.diag,5,self.lasttick,#self.history)
 local state,_,tick=get(self.state);local _,len,run=get(self.meta)
 if run~=self.run then self.run=run;self.lasttick=tick;self.history={};park(self,0);return end
 if state~=1 or tick==self.lasttick then return end;self.lasttick=tick
 local x,_,z=get(self.prev);x,z=cell(x,z);table.insert(self.history,1,{x,z});if #self.history>64 then table.remove(self.history) end
 len=math.max(0,math.min(64,math.floor(len)))
 for i=1,math.min(len,#self.history) do local q=self.history[i];set(self.body[i],q[1]-14.5,0,q[2]-9.5) end;park(self,len)
 local hx,_,hz=get(self.head);hx,hz=cell(hx,hz)
 for i=1,math.min(len,#self.history) do local q=self.history[i];if q[1]==hx and q[2]==hz then local w,_,a=get(self.verdict);set(self.verdict,w,1,a);return end end
end
return M
