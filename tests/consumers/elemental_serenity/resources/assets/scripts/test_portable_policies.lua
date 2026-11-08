local root=assert(arg[1])
local positions={flower_0={2,3,4},Firefly_0={1,2,3},Ground={0,0,0}}
local rotations={flower_0={10,20,30}}
local written={};local colors={};local values={CampfireLight='{ "intensity":2 }',Ground='{"albedoTex":"textures/ground_baked2/spring_day.png"}',Rocks='{"baseColorR":0.68,"baseColorG":0.57,"baseColorB":0.31,"roughness":1}'}
jce={find_by_name=function(n) return n end,find_by_prefix=function(p) return p=='flower_' and {'flower_0'} or {'Firefly_0'} end,
 get_position=function(e) return table.unpack(positions[e] or {0,0,0}) end,get_rotation=function(e) return table.unpack(rotations[e] or {0,0,0}) end,
 set_position=function(e,x,y,z) positions[e]={x,y,z} end,set_rotation=function(e,x,y,z) rotations[e]={x,y,z} end,
 particle_set_color=function(e,r,g,b) colors[e]={r,g,b} end,comp_get=function(e) return values[e] end,
 comp_set=function(e,kind,text) written[e]=text;return true end,log=function() end}
local function run(name) local m=assert(loadfile(root..'/'..name))();local self=setmetatable({},{__index=m});self:on_start();return self end
local fire=run('es_campfire_portable.lua');fire:on_update(.5);assert(written.CampfireLight2:find('"intensity":'));values.CampfireLight='{"intensity":0}';fire:on_update(.5);assert(written.CampfireLight2:find('"intensity":0}'))
local flower=run('es_flowers_portable.lua');flower:on_update(1);assert(rotations.flower_0[1]==10 and math.abs(rotations.flower_0[2]-20)<=3 and math.abs(rotations.flower_0[3]-30)<=5.5)
local flies=run('es_fireflies_portable.lua');flies:on_update(1);assert(math.abs(positions.Firefly_0[1]-1)<=.55 and math.abs(positions.Firefly_0[2]-2)<=.30);assert(colors.Firefly_0[1]>=.42 and colors.Firefly_0[1]<=1)
local prop=run('es_surfaces_portable.lua');prop:on_update(.25);assert(written.Rocks:find('"roughness":1'))
values.Ground='{"albedoTex":"textures/ground_baked2/rainy_day.png"}';prop:on_update(.25);assert(prop.wet>0 and prop.wet<1)
local expected=1-math.exp(-.25/3.5);assert(math.abs(prop.wet-expected)<1e-12)
values.Ground='{"albedoTex":"textures/ground_baked2/spring_day.png"}';local old=prop.wet;prop:on_update(.25);assert(prop.wet<old)
assert(loadfile(root..'/es_director_portable.lua'))
print('ES_PORTABLE_CPU PASS rain rise/dry decay, zero-light gate, bounded sway/drift/color, director syntax')
assert(loadfile(root..'/../../../tests/director_wind.lua'))()
