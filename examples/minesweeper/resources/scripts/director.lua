-- Shared scene driver. AI proposals are data, never Lua source.
local M={}
local intent,proposal,color,version,custom,theme
local serial,revision=0,-1
local keys={}
local function submit(action,button)
    serial=serial+1
    jce.set_position(intent,action,button or 1,serial)
end
function mines_ui_click(entity)
    local action=jce.get_position(entity)
    submit(action,1)
end
function M:on_start()
    intent=jce.find_by_name("InputIntent")
    proposal=jce.find_by_name("AiProposal")
    color=jce.find_by_name("AiColor")
    version=jce.find_by_name("AiVersion")
    custom=jce.find_by_name("CustomSettings")
    theme=jce.find_by_name("Theme")
    jce.log("MINES_DIRECTOR Lua started: commands + AI component validation")
end
function M:on_update(dt)
    local bindings={{30,1},{31,2},{32,3},{41,6},{17,5},{9,9}}
    for _,binding in ipairs(bindings) do
        local down=jce.is_key_down(binding[1])
        if down and not keys[binding[1]] then submit(binding[2]) end
        keys[binding[1]]=down
    end
    local next_revision=jce.get_position(version)
    if next_revision==revision then return end
    local w,h,m=jce.get_position(proposal)
    local r,g,b=jce.get_position(color)
    if w%1~=0 or h%1~=0 or m%1~=0 or w<5 or w>30 or h<5 or h>16 or m<1 or m>w*h-9
       or r<0 or r>1 or g<0 or g>1 or b<0 or b>1 then
        jce.log("MINES_DIRECTOR rejected invalid component proposal")
        revision=next_revision
        return
    end
    jce.set_position(custom,w,h,m)
    jce.set_position(theme,r,g,b)
    revision=next_revision
    jce.log(string.format("MINES_DIRECTOR applied revision=%d challenge=%dx%d/%d",revision,w,h,m))
end
return M
