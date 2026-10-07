-- Portable Lua policy; original seven-driver project is preserved.
local M={}
local function get(e) return jce.get_position(e) end
local function set(e,x,y,z) jce.set_position(e,x,y,z) end
local function first(n) return assert(jce.find_by_name(n),'missing '..n) end
local function cell(x,z) return math.floor(x+15),math.floor(z+10) end
function M:on_start()
 self.state=first('GameState');self.meta=first('GameMeta');self.score=first('ScoreText');self.best=first('BestText');self.title=first('StateText');self.hint=first('HintText');self.shown={};jce.log('SNAKE_PORTABLE HUD Lua started')
end
function M:on_update(dt)
 local state,score=get(self.state);local best=get(self.meta)
 if score~=self.shown.score then self.shown.score=score;jce.ui_set_text(self.score,'SCORE '..math.floor(score)) end
 if best~=self.shown.best then self.shown.best=best;jce.ui_set_text(self.best,'BEST '..math.floor(best)) end
 if state==self.shown.state and state~=3 then return end;self.shown.state=state
 local title,hint='SNAKE','PRESS ENTER TO START      ARROWS / WASD TO STEER'
 if state==1 then title='';hint='' elseif state==2 then title='PAUSED';hint='P / ESC  RESUME      ENTER  RESTART      Q  QUIT TO MENU' elseif state==3 then title='GAME OVER';hint='SCORE '..math.floor(score)..'      BEST '..math.floor(best)..'      ENTER  PLAY AGAIN' end
 jce.ui_set_text(self.title,title);jce.ui_set_text(self.hint,hint)
end
return M
