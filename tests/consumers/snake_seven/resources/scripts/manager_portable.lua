-- Portable Lua policy; original seven-driver project is preserved.
local M={}
local function get(e) return jce.get_position(e) end
local function set(e,x,y,z) jce.set_position(e,x,y,z) end
local function first(n) return assert(jce.find_by_name(n),'missing '..n) end
local function cell(x,z) return math.floor(x+15),math.floor(z+10) end
local function fresh(self)
 local _,_,run=get(self.meta);set(self.entity,1,0,0);set(self.meta,self.best,3,run+1);set(self.dir,1,0,0);set(self.pending,0,0,0);set(self.verdict,0,0,0)
end
function M:on_start()
 self.meta=first('GameMeta');self.dir=first('Direction');self.pending=first('Pending');self.verdict=first('Verdict');self.best=0
 self.persist=type(jce.stat_available)=='function' and jce.stat_available()
 if self.persist then local v=jce.stat_get('snake_best',0);if v>=0 and v%1==0 and v<=2147483647 then self.best=v end end
 jce.log(self.persist and 'SNAKE_PORTABLE statistics host available' or 'SNAKE_PORTABLE statistics UNAVAILABLE; session best only')
 set(self.entity,0,0,0);set(self.meta,self.best,3,0);jce.log('SNAKE_PORTABLE manager Lua started')
end
function M:on_update(dt)
 local state,score,tick=get(self.entity);local dx,intent,dz=get(self.pending);intent=math.floor(intent)
 if intent&1~=0 then intent=intent&~1;set(self.pending,dx,intent,dz);if state==0 or state==3 then fresh(self);return end end
 if intent&2~=0 then intent=intent&~2;set(self.pending,dx,intent,dz);if state==1 or state==2 then set(self.entity,state==1 and 2 or 1,score,tick);return end end
 if intent&4~=0 then intent=intent&~4;set(self.pending,dx,intent,dz);if state==2 or state==3 then set(self.entity,0,0,0);return end end
 if state~=1 then return end
 local wall,collision,ate=get(self.verdict);if ate>=.5 then set(self.verdict,wall,collision,0) end
 if wall>=.5 or collision>=.5 then
  set(self.entity,3,score,tick)
  if score>self.best then self.best=score
   if self.persist then local saved=jce.stat_set('snake_best',score) and jce.stat_flush();jce.log(saved and 'SNAKE_PORTABLE best persisted' or 'SNAKE_PORTABLE best save failed') end
  end
  local _,len,run=get(self.meta);set(self.meta,self.best,len,run)
 end
end
return M
