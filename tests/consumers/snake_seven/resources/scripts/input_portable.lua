-- Portable Lua policy; original seven-driver project is preserved.
local M={}
local function get(e) return jce.get_position(e) end
local function set(e,x,y,z) jce.set_position(e,x,y,z) end
local function first(n) return assert(jce.find_by_name(n),'missing '..n) end
local function cell(x,z) return math.floor(x+15),math.floor(z+10) end
local function key(...) for _,k in ipairs({...}) do if jce.is_key_down(k) then return true end end;return false end
function M:on_start() self.dir=first('Direction');self.pending=first('Pending');self.edges={};jce.log('SNAKE_PORTABLE input Lua started') end
function M:on_update(dt)
 local px,intent,pz=get(self.pending);local dx,_,dz=get(self.dir);local x,z=0,0
 if key(82,26) then z=-1 elseif key(81,22) then z=1 elseif key(80,4) then x=-1 elseif key(79,7) then x=1 end
 if (x~=0 or z~=0) and not(x==-dx and z==-dz) then px=x;pz=z end
 for i,now in ipairs({key(40,44),key(41,19),key(20)}) do if now and not self.edges[i] then intent=math.floor(intent)|(1<<(i-1)) end;self.edges[i]=now end
 set(self.pending,px,intent,pz)
end
return M
