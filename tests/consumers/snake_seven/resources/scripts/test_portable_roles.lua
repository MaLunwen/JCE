local root=assert(arg[1]);local positions={};local keys={};local text={};local logs={}
local names={'GameState','GameMeta','Direction','Pending','Verdict','SnakeHead','PrevHead','CppDiag','Food','Canvas','ScoreText','BestText','StateText','HintText'}
for _,n in ipairs(names) do positions[n]={0,0,0} end
for i=0,63 do positions[string.format('Body%02d',i)]={0,-100,0} end
jce={find_by_name=function(n) return positions[n] and n or nil end,get_position=function(e) return table.unpack(assert(positions[e])) end,
 set_position=function(e,x,y,z) positions[e]={x,y,z} end,is_key_down=function(k) return keys[k] or false end,log=function(s) logs[#logs+1]=s end,
 ui_set_text=function(e,s) text[e]=s end,stat_available=function() return false end}
local roles={};local spec={{'manager_portable.lua','GameState'},{'input_portable.lua','Direction'},{'board_portable.lua','Verdict'},{'snake_portable.lua','SnakeHead'},{'body_portable.lua','Body00'},{'food_portable.lua','Food'},{'hud_portable.lua','Canvas'}}
for _,row in ipairs(spec) do local m=assert(loadfile(root..'/'..row[1]))();local obj=setmetatable({entity=row[2]},{__index=m});obj:on_start();roles[#roles+1]=obj end
local function frame(dt) for _,obj in ipairs(roles) do obj:on_update(dt) end end
frame(.016);assert(positions.GameState[1]==0 and text.StateText=='SNAKE')
keys[40]=true;frame(.016);keys[40]=false;frame(.016);assert(positions.GameState[1]==1 and positions.GameMeta[2]==3 and positions.GameMeta[3]==1)
-- UP means absolute negative Z; reversal DOWN must not replace pending UP.
keys[82]=true;frame(.01);keys[82]=false;frame(.18);local z=positions.SnakeHead[3];assert(z<.5 and positions.Direction[3]==-1)
keys[81]=true;frame(.01);keys[81]=false;frame(.18);assert(positions.SnakeHead[3]<z and positions.Direction[3]==-1)
-- Pause freezes movement, then menu and restart reset score/tick/run.
keys[19]=true;frame(.01);keys[19]=false;frame(.01);assert(positions.GameState[1]==2)
local paused=positions.SnakeHead[3];frame(1);assert(positions.SnakeHead[3]==paused and text.StateText=='PAUSED')
keys[20]=true;frame(.01);keys[20]=false;frame(.01);assert(positions.GameState[1]==0)
keys[40]=true;frame(.01);keys[40]=false;frame(.01);assert(positions.GameMeta[3]==2 and positions.GameState[2]==0)
-- Food cannot spawn on active head/body cells; repeated LCG draws stay in bounds.
local food=roles[6];for i=1,150 do assert(food:spawn());local x,y,z=table.unpack(positions.Food);local cx,cz=math.floor(x+15),math.floor(z+10);assert(cx>=0 and cx<30 and cz>=0 and cz<20);assert(x~=positions.SnakeHead[1] or z~=positions.SnakeHead[3]) end
positions.Food={table.unpack(positions.SnakeHead)};food:on_update(0);assert(positions.GameState[2]==10 and positions.GameMeta[2]==4 and positions.Verdict[3]==1)
-- Independent wall verdict and manager game-over; unavailable stats never fake save.
positions.SnakeHead={15.5,0,0};roles[3]:on_update(0);assert(positions.Verdict[1]==1);roles[1]:on_update(0);roles[7]:on_update(0);assert(positions.GameState[1]==3 and text.StateText=='GAME OVER' and positions.GameMeta[1]==10)
local failures=table.concat(logs,'\n');assert(failures:find('statistics UNAVAILABLE',1,true) and not failures:find('best persisted',1,true))
-- Actual service refusal must remain a failure rather than persisted output.
jce.stat_available=function() return true end;jce.stat_get=function() return 0 end;jce.stat_set=function() return false end;jce.stat_flush=function() error('flush after failed set') end
local m=assert(loadfile(root..'/manager_portable.lua'))();local obj=setmetatable({entity='GameState'},{__index=m});obj:on_start();positions.GameState={1,20,1};positions.Verdict={1,0,0};obj:on_update(0);assert(logs[#logs]=='SNAKE_PORTABLE best save failed')
print('SNAKE_PORTABLE_CPU PASS absolute UP/no reverse, state edges/pause/restart, free-cell spawn,10-point growth, wall/HUD, unavailable/failed persistence')
