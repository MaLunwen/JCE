-- Pure CPU rules checks; invoked with an explicit controller path.
local controller=assert(arg[1],"controller path required")
local M=assert(loadfile(controller))()
M.self_test()
local B=M.rules
for _,p in ipairs({{9,9,10},{16,16,40},{30,16,99},{5,5,16}}) do
    for seed=1,32 do
        local first=(p[1]*p[2])//2
        local a=B.reset(p[1],p[2],p[3],seed)
        local b=B.reset(p[1],p[2],p[3],seed)
        assert(B.reveal(a,first) and B.reveal(b,first))
        local mines=0
        for i=0,p[1]*p[2]-1 do
            assert(a.cells[i].mine==b.cells[i].mine)
            if a.cells[i].mine then mines=mines+1 end
            if not a.cells[i].mine then B.reveal(a,i) end
        end
        assert(mines==p[3] and a.state==2 and a.revealed==p[1]*p[2]-p[3])
    end
end
local a=B.reset(5,5,1,7)
assert(B.flag(a,0) and a.flags==1 and not B.reveal(a,0))
assert(B.flag(a,0) and a.flags==0)
a=B.reset(9,9,10,7)
B.reveal(a,40)
for i=0,80 do if a.cells[i].mine then assert(B.reveal(a,i) and a.state==3);break end end
assert(not B.flag(a,1) and not B.reveal(a,1))
print("PORTABLE_MINES_RULES: PASS deterministic presets, first safety, exact mines, flags, win/loss")

-- Exercise the actual controller's public component output, not a copy of its math.
local components={}
jce={find_by_name=function(name) return name end,log=function() end,
    comp_set=function(entity,kind,text)
        components[entity]=components[entity] or {};local c=components[entity][kind] or {};components[entity][kind]=c
        for key,value in text:gmatch('"([%w_]+)":([%d%.%-]+)') do c[key]=tonumber(value) end
        return true
    end,ui_set_text=function() end,set_position=function() end,
    get_position=function(name)
        if name=="CustomSettings" then return 20,16,50 end
        if name=="Theme" then return 24/255,45/255,64/255 end
        return 0,0,0
    end,get_pointer_position=function() return 0,0,0,0 end,is_pointer_down=function() return false end}
local instance=setmetatable({},{__index=M});instance:on_start();instance:on_update(0)
local image=components.Widget000.UIImage;local button=components.Widget000.UIButton
assert(math.abs(image.colorR-24/255)<.0001 and math.abs(image.colorG-45/255)<.0001 and math.abs(image.colorB-64/255)<.0001 and image.colorA==1)
for _,state in ipairs({"normal","highlight","pressed","disabled"}) do
    assert(button[state.."A"]==1)
    for _,channel in ipairs({"R","G","B"}) do assert(math.abs(button[state..channel]-image["color"..channel])<.0001) end
end
print("PORTABLE_MINES_UI: PASS opaque background and identical state RGBA")

assert(image.anchorMinX==0 and image.anchorMinY==0 and image.anchorMaxX==1 and image.anchorMaxY==1 and image.sizeW==0 and image.sizeH==0)
print("PORTABLE_MINES_BACKGROUND: PASS full Canvas stretch")

-- Host failures must not be reported as durable saves or accepted exits.
local host=setmetatable({records={},saved={}},{__index=M})
local board={w=9,h=9,mines=10,elapsed=1.234}
jce.stat_available=function() return false end
M.save_record(host,board);assert(host.save_status=='UNAVAILABLE' and host.records.mines_b==1234)
jce.stat_available=function() return true end;jce.stat_get=function() return 0 end
jce.stat_set=function() return false end;jce.stat_flush=function() error('flush after failed set') end
M.save_record(host,board);assert(host.save_status=='SAVE FAILED' and host.saved.mines_b==nil)
jce.stat_set=function(k,v) assert(k=='mines_b' and v==1234);return true end
jce.stat_flush=function() return false end;M.save_record(host,board);assert(host.save_status=='SAVE FAILED')
jce.stat_flush=function() return true end;M.save_record(host,board);assert(host.save_status=='SAVED' and host.saved.mines_b==1234)
local logs={};jce.log=function(v) logs[#logs+1]=v end
jce.request_quit=function() return false end;M.command(host,7,1);assert(logs[#logs]:match('refused'))
jce.request_quit=function() return true end;M.command(host,7,1);assert(logs[#logs]=='MINES_PORTABLE_QUIT requested')
print('PORTABLE_MINES_SERVICES: PASS unavailable, set/flush failure, successful save and actual quit result')
