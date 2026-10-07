-- Portable bundle candidate: public ScriptHost only, no native module.
local M = {}
local B = {}
local presets = {{9,9,10},{16,16,40},{30,16,99}}
local names = {"BEGINNER","INTERMEDIATE","EXPERT","CUSTOM"}
local digits = {0x1746bd,0x18723b,0xc93232,0x292b83,0x802020,0x147c83,0x111111,0x616161}
local function neighbors(b,i)
    local result = {}
    local x,y = i % b.w, i // b.w
    for dy=-1,1 do for dx=-1,1 do
        local nx,ny=x+dx,y+dy
        if (dx~=0 or dy~=0) and nx>=0 and nx<b.w and ny>=0 and ny<b.h then
            result[#result+1]=ny*b.w+nx
        end
    end end
    return result
end
function B.reset(w,h,mines,seed)
    assert(w>=5 and w<=30 and h>=5 and h<=16 and mines>=1 and mines<=w*h-9)
    local b={w=w,h=h,mines=mines,seed=seed~=0 and seed or 1,state=0,elapsed=0,flags=0,revealed=0,cells={}}
    for i=0,w*h-1 do b.cells[i]={mine=false,adjacent=0,revealed=false,flagged=false} end
    return b
end
local function generate(b,first)
    local candidates={}
    local fx,fy=first%b.w,first//b.w
    for i=0,b.w*b.h-1 do
        if math.abs(i%b.w-fx)>1 or math.abs(i//b.w-fy)>1 then candidates[#candidates+1]=i end
    end
    local random=b.seed
    for i=1,b.mines do
        random=(random ~ (random << 13)) & 0xffffffff
        random=(random ~ (random >> 17)) & 0xffffffff
        random=(random ~ (random << 5)) & 0xffffffff
        local j=i+random%(#candidates-i+1)
        candidates[i],candidates[j]=candidates[j],candidates[i]
        b.cells[candidates[i]].mine=true
    end
    for i=0,b.w*b.h-1 do
        for _,n in ipairs(neighbors(b,i)) do if b.cells[n].mine then b.cells[i].adjacent=b.cells[i].adjacent+1 end end
    end
    b.state=1
end
function B.reveal(b,i)
    if b.state>=2 or not b.cells[i] then return false end
    local c=b.cells[i]
    if c.revealed or c.flagged then return false end
    if b.state==0 then generate(b,i) end
    if c.mine then c.exploded=true;b.state=3;return true end
    local queue={i};local head=1
    c.revealed=true;b.revealed=b.revealed+1
    while head<=#queue do
        local current=queue[head];head=head+1
        if b.cells[current].adjacent==0 then
            for _,n in ipairs(neighbors(b,current)) do
                local next=b.cells[n]
                if not next.revealed and not next.flagged and not next.mine then
                    next.revealed=true;b.revealed=b.revealed+1;queue[#queue+1]=n
                end
            end
        end
    end
    if b.revealed==b.w*b.h-b.mines then b.state=2 end
    return true
end
function B.flag(b,i)
    if b.state>=2 or not b.cells[i] or b.cells[i].revealed then return false end
    local c=b.cells[i];c.flagged=not c.flagged;b.flags=b.flags+(c.flagged and 1 or -1);return true
end
function B.chord(b,i)
    if b.state~=1 or not b.cells[i] or not b.cells[i].revealed or b.cells[i].adjacent==0 then return false end
    local nearby=neighbors(b,i);local count=0
    for _,n in ipairs(nearby) do if b.cells[n].flagged then count=count+1 end end
    if count~=b.cells[i].adjacent then return false end
    local changed=false
    for _,n in ipairs(nearby) do if b.state==1 and B.reveal(b,n) then changed=true end end
    return changed
end
local function self_test()
    for seed=1,16 do
        local b=B.reset(9,9,10,seed);assert(B.reveal(b,40));local mines=0
        for i=0,80 do if b.cells[i].mine then mines=mines+1 end end
        assert(mines==10 and not b.cells[40].mine)
        for _,n in ipairs(neighbors(b,40)) do assert(not b.cells[n].mine) end
        for i=0,80 do if not b.cells[i].mine then B.reveal(b,i) end end
        assert(b.state==2 and b.revealed==71)
    end
    local b=B.reset(5,5,1,1);assert(B.flag(b,0) and b.flags==1)
    assert(not B.reveal(b,0));assert(B.flag(b,0) and b.flags==0)
end
M.rules=B

M.self_test=self_test
local function rgb(v) return ((v>>16)&255)/255,((v>>8)&255)/255,(v&255)/255 end
local function patch(e,kind,text) assert(jce.comp_set(e,kind,text),"Canvas component patch refused") end
local function widget(self,x,y,w,h,text,size,fill,ink,action)
    self.count=self.count+1;local e=self.widgets[self.count];assert(e,"Widget pool exhausted")
    local layout='"anchorMinX":0,"anchorMaxX":0,"anchorMinY":1,"anchorMaxY":1,"pivotX":0,"pivotY":1'
    local r,g,b=rgb(fill);local alpha=fill==0 and 0 or 1
    patch(e,"UIImage",string.format('{%s,"anchoredX":%.2f,"anchoredY":%.2f,"sizeW":%.2f,"sizeH":%.2f,"colorR":%.4f,"colorG":%.4f,"colorB":%.4f,"colorA":%d}',layout,x,-y,w,h,r,g,b,alpha))
    r,g,b=rgb(ink)
    patch(e,"UIText",string.format('{%s,"anchoredX":%.2f,"anchoredY":%.2f,"sizeW":%.2f,"sizeH":%.2f,"fontSize":%.2f,"alignment":1,"colorR":%.4f,"colorG":%.4f,"colorB":%.4f,"colorA":1}',layout,x,-y,w,h,size,r,g,b))
    jce.ui_set_text(e,text)
    patch(e,"UIButton",'{"interactable":false}') -- Pointer edges below own commands once.
    -- Current Canvas tint lerps toward button state RGBA. Keep it identical
    -- to the owned image so disabled state cannot erase the background.
    local fr,fg,fb=rgb(fill)
    for _,state in ipairs({"normal","highlight","pressed","disabled"}) do
        patch(e,"UIButton",string.format('{"%sR":%.4f,"%sG":%.4f,"%sB":%.4f,"%sA":%d}',state,fr,state,fg,state,fb,state,alpha))
    end
    if self.count==1 then
        patch(e,"UIImage",'{"anchorMinX":0,"anchorMinY":0,"anchorMaxX":1,"anchorMaxY":1,"pivotX":0,"pivotY":0,"anchoredX":0,"anchoredY":0,"sizeW":0,"sizeH":0}')
    end
    self.rects[self.count]={x,y,w,h,action or 0}
end
local function record_key(b)
    local id=string.format('%dx%d/%d',b.w,b.h,b.mines)
    local keys={['9x9/10']='mines_b',['16x16/40']='mines_m',['30x16/99']='mines_e'}
    return keys[id] or string.format('mines_c_%d_%d_%d',b.w,b.h,b.mines)
end
local function service_available()
    return type(jce.stat_available)=='function' and jce.stat_available()==true
        and type(jce.stat_get)=='function' and type(jce.stat_set)=='function' and type(jce.stat_flush)=='function'
end
local function stored_best(self,b)
    if not service_available() then return nil end
    local v=jce.stat_get(record_key(b),0)
    return type(v)=='number' and v%1==0 and v>0 and v<=2147483647 and v or nil
end
local function save_record(self,b)
    local ms=math.max(1,math.min(2147483647,math.floor(b.elapsed*1000)))
    local key=record_key(b);self.records[key]=math.min(self.records[key] or ms,ms)
    if not service_available() then self.save_status='UNAVAILABLE';return end
    local prior=stored_best(self,b)
    if prior and prior<=ms then self.save_status='STORED';return end
    if jce.stat_set(key,ms)==true and jce.stat_flush()==true then
        self.save_status='SAVED';self.saved[key]=ms
    else self.save_status='SAVE FAILED' end
end
local function refresh(self)
    local previous=self.count or 0;self.count=0
    local function W(...) widget(self,...) end
    W(0,0,1280,720,"",1,self.background,0,0)
    W(40,24,1200,58,"MINESWEEPER",38,0,0xf2f4f6)
    W(40,78,1200,26,"Portable Lua bundle candidate",15,0,0xa6bec9)
    if self.menu~=0 then
        W(380,142,520,64,"Choose your board",24,0,0xeef3f5)
        for i=1,3 do local p=presets[i];W(410,220+(i-1)*78,460,62,string.format('%s  %d x %d / %d mines',names[i],p[1],p[2],p[3]),20,0xd7dde0,0x202830,i) end
        W(410,454,460,62,"CUSTOM BOARD",22,0xd7dde0,0x202830,4)
        W(350,550,580,40,type(jce.request_quit)=="function" and "EXIT" or "EXIT: UNAVAILABLE",17,0,0xb7c9d2,7)
    else
        local b=self.board;local cell=b.h<=9 and 46 or 27;local bx=(1280-b.w*cell)/2;local by=195
        W(50,125,250,46,string.format('MINES %03d',b.mines-b.flags),25,0x10181d,0xff6363)
        W(980,125,250,46,string.format('TIME %03d',math.min(999,math.floor(b.elapsed))),25,0x10181d,0xff6363)
        W(332,125,190,46,"MENU [Esc]",18,0xd7dde0,0x202830,6)
        W(536,125,230,46,"RESTART [N]",18,0xd7dde0,0x202830,5)
        W(780,125,160,46,self.flag_mode and "FLAG MODE" or "REVEAL MODE",15,0xc6d0d6,0x202830,9)
        W(20,185,185,55,"AI HOST: UNAVAILABLE",13,0,0xb7c9d2)
        W(bx-8,by-8,b.w*cell+16,b.h*cell+16,"",1,0x747d84,0)
        for i=0,b.w*b.h-1 do
            local c=b.cells[i];local fill=c.revealed and 0xb5bcc1 or 0xe2e5e7;local ink=0x222222;local text=""
            if c.exploded then fill=0xef5350;text="*"
            elseif b.state==3 and c.flagged and not c.mine then text="X";ink=0xad2626
            elseif b.state==3 and c.mine then text="*"
            elseif c.flagged or b.state==2 and c.mine then text="F";ink=0xbd2424
            elseif c.revealed and c.adjacent>0 then text=tostring(c.adjacent);ink=digits[c.adjacent] end
            W(bx+(i%b.w)*cell+1,by+(i//b.w)*cell+1,cell-2,cell-2,text,cell*.62,fill,ink,1000+i)
        end
        local state=({"READY","PLAYING","YOU WIN","GAME OVER"})[b.state+1]
        W(80,640,1120,32,string.format('%s | %s | Safe cells %d / %d',state,names[self.difficulty],b.revealed,b.w*b.h-b.mines),22,0,0xe8eef1)
        local best=self.records[record_key(b)];local durable=self.saved[record_key(b)] or (self.save_status~='SAVE FAILED' and stored_best(self,b))
        W(80,678,1120,28,string.format('SESSION %s | STORED %s | %s',best and string.format('%.2fs',best/1000) or '--',durable and string.format('%.2fs',durable/1000) or '--',self.save_status or (service_available() and 'AVAILABLE' or 'UNAVAILABLE')),17,0,0xb7c9d2)
    end
    W(260,700,760,20,"Left reveal / Right flag / Middle chord; first 3 x 3 safe",15,0,0xb7c9d2)
    if self.menu==2 then
        W(350,190,580,440,"",1,0x293844,0)
        for i=1,3 do
            W(465,230+(i-1)*84,350,56,({"WIDTH","HEIGHT","MINES"})[i]..": "..self.custom[i],23,0,0xf1f4f5)
            W(380,230+(i-1)*84,66,56,"-",28,0xc6d0d6,0x202830,20+(i-1)*2)
            W(830,230+(i-1)*84,66,56,"+",28,0xc6d0d6,0x202830,21+(i-1)*2)
        end
        W(400,506,220,62,"START",22,0x80d6b5,0x203630,8);W(660,506,220,62,"BACK",22,0xc6d0d6,0x202830,6)
    end
    for i=self.count+1,previous do patch(self.widgets[i],"UIImage",'{"anchoredX":-10000,"colorA":0}');jce.ui_set_text(self.widgets[i],"") end
    self.dirty=false
end
local function start(self,difficulty)
    local p=difficulty<4 and presets[difficulty] or self.custom
    self.board=B.reset(p[1],p[2],p[3],self.seed);self.seed=self.seed+1;self.difficulty=difficulty;self.menu=0;self.dirty=true
    jce.log(string.format("MINES_PORTABLE_START width=%d height=%d mines=%d seed=%d",p[1],p[2],p[3],self.board.seed))
end
local function command(self,action,button)
    if action>=1000 and self.menu==0 then
        local b=self.board;local before=b.state;local i=action-1000
        if button==1 and self.flag_mode then button=3 end
        local changed=button==3 and B.flag(b,i) or button==2 and B.chord(b,i) or button==1 and B.reveal(b,i)
        if changed then
            jce.log(string.format('MINES_PORTABLE_ACTION button=%d index=%d state=%d flags=%d revealed=%d',button,i,b.state,b.flags,b.revealed))
            self.dirty=true
            if before~=2 and b.state==2 then
                save_record(self,b)
            end
        end
        return
    end
    if button~=1 then return end
    if action>=1 and action<=3 then start(self,action)
    elseif action==4 then self.menu=2;self.dirty=true
    elseif action==5 then start(self,self.difficulty)
    elseif action==6 then self.menu=1;self.dirty=true
    elseif action==7 then
        local accepted=type(jce.request_quit)=='function' and jce.request_quit()==true
        jce.log(accepted and 'MINES_PORTABLE_QUIT requested' or 'MINES_PORTABLE_QUIT unavailable/refused')
    elseif action==8 then start(self,4)
    elseif action==9 then self.flag_mode=not self.flag_mode;self.dirty=true
    elseif action>=20 and action<=25 then
        local slot=(action-20)//2+1;local delta=action%2==1 and 1 or -1
        local max=slot==1 and 30 or slot==2 and 16 or self.custom[1]*self.custom[2]-9
        self.custom[slot]=math.max(slot==3 and 1 or 5,math.min(max,self.custom[slot]+delta))
        self.custom[3]=math.min(self.custom[3],self.custom[1]*self.custom[2]-9)
        jce.set_position(self.custom_entity,table.unpack(self.custom));self.dirty=true
    end
end
M.save_record=save_record
M.command=command
function M:on_start()
    self_test();self.widgets={};self.rects={};self.records={};self.saved={};self.pointer={};self.count=0
    self.menu=1;self.difficulty=1;self.custom={20,16,50};self.seed=1;self.background=0x202830;self.dirty=true;self.serial=0
    for i=0,529 do self.widgets[i+1]=assert(jce.find_by_name(string.format('Widget%03d',i))) end
    self.intent=assert(jce.find_by_name('InputIntent'));self.custom_entity=assert(jce.find_by_name('CustomSettings'));self.theme=assert(jce.find_by_name('Theme'))
    jce.log('MINES_PORTABLE_BOOT Lua rules + pointer + Canvas; LLM UNAVAILABLE; stats/quit use actual public host services')
end
function M:on_update(dt)
    if self.menu==0 and self.board.state==1 and dt>0 and dt<60 then self.board.elapsed=self.board.elapsed+dt end
    local a,b,serial=jce.get_position(self.intent)
    if serial~=self.serial then self.serial=serial;command(self,math.floor(a),math.floor(b)) end
    local w,h,m=jce.get_position(self.custom_entity)
    if w%1==0 and h%1==0 and m%1==0 and w>=5 and w<=30 and h>=5 and h<=16 and m>=1 and m<=w*h-9 then
        if w~=self.custom[1] or h~=self.custom[2] or m~=self.custom[3] then self.custom={w,h,m};self.dirty=true end
    end
    local r,g,b=jce.get_position(self.theme)
    if r>=0 and r<=1 and g>=0 and g<=1 and b>=0 and b<=1 then
        local color=(math.floor(r*255+.5)<<16)|(math.floor(g*255+.5)<<8)|math.floor(b*255+.5)
        if color~=self.background then self.background=color;self.dirty=true end
    end
    if self.menu==0 and math.floor(self.board.elapsed)~=self.last_second then self.last_second=math.floor(self.board.elapsed);self.dirty=true end
    if self.dirty then refresh(self) end
    local x,y,width,height=jce.get_pointer_position();local scale=math.sqrt(width/1280*height/720)
    for button=1,3 do
        local down=jce.is_pointer_down(button)
        if down and not self.pointer[button] and scale>0 then
            for i=self.count,1,-1 do local q=self.rects[i]
                if x/scale>=q[1] and y/scale>=q[2] and x/scale<q[1]+q[3] and y/scale<q[2]+q[4] then command(self,q[5],button);break end
            end
        end
        self.pointer[button]=down
    end
end
return M
