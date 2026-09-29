--[[ snake.lua -- MODULE 1 of 7: THE SNAKE, in Lua.

Owns the logic clock and the head.  Nothing else advances a tick, and
nothing else writes the head's position.

WHAT IT DOES NOT OWN, on purpose: walls are Java's, the body and
self-collision are C++'s, food and growth are Python's, the state machine
is C's, input is JS's, the HUD is C#'s.  Each module has exactly one thing
to be wrong about, which is the only reason a seven-language game is
debuggable at all.

THE SPEED CURVE IS HERE (spec 14): interval starts at 0.18s and falls
toward a 0.07s floor as the score rises.  A bar or readout that disagrees
with the snake's pace is this function and nothing else.

READ-MODIFY-WRITE.  set_position writes all three components, so a module
that wants to change one slot must read the other two first and put them
back.  That is safe without any locking because scripts run sequentially
within a frame -- no two module updates interleave -- but it IS required:
writing a slot you did not read is how another module's value gets reset
to whatever this script last happened to see.
]]

local COLS, ROWS = 30, 20          -- spec 11
local STEP_INIT = 0.18             -- spec 14, seconds per cell at score 0
local STEP_MIN  = 0.07             -- spec 14, the floor
local STEP_DROP = 0.0005           -- per point; 0.18 -> 0.07 over 220 points

local HX = (COLS - 1) / 2.0
local HZ = (ROWS - 1) / 2.0

-- How many body segments the per-tick trace reports.  The trace exists so
-- the game can be VERIFIED rather than watched: a snake is a state machine
-- whose whole correctness is a sequence of cells, and a screenshot cannot
-- show that the body stayed connected or that the food never landed on it.
-- Six segments is enough to catch a discontinuity, which is always adjacent.
local TRACE_BODY = 6

local M = {}

local function to_world(cx, cz) return cx - HX, cz - HZ end

local function to_cell_x(x) return math.floor(x + HX + 0.5) end
local function to_cell_z(z) return math.floor(z + HZ + 0.5) end

local function to_cell(x, z)
    return to_cell_x(x), to_cell_z(z)
end

local function step_for(score)
    local s = STEP_INIT - score * STEP_DROP
    if s < STEP_MIN then s = STEP_MIN end
    return s
end

function M:on_start()
    self.state_e = jce.find_by_name("GameState")
    self.meta_e  = jce.find_by_name("GameMeta")
    self.dir_e   = jce.find_by_name("Direction")
    self.pend_e  = jce.find_by_name("Pending")
    self.prev_e  = jce.find_by_name("PrevHead")
    self.acc = 0.0
    self.run = -1
    self.verdict_e = jce.find_by_name("Verdict")
    self.food_e = jce.find_by_name("Food")
    self.diag_e = jce.find_by_name("CppDiag")
    self.body_e = {}
    for i = 0, TRACE_BODY - 1 do
        self.body_e[i] = jce.find_by_name(string.format("Body%02d", i))
    end
    jce.log("SNAKE lua: clock up on entity " .. tostring(self.entity)
            .. "; board " .. COLS .. "x" .. ROWS)
end

function M:on_update(dt)
    local st, score = jce.get_position(self.state_e)
    local _, _, run = jce.get_position(self.meta_e)

    -- A new run resets this module's own state.  Modules share no code, so
    -- the run counter is how a restart reaches all seven of them.
    if run ~= self.run then
        self.run = run
        self.acc = 0.0
        local wx, wz = to_world(math.floor(COLS / 2), math.floor(ROWS / 2))
        -- Y is PINNED, not carried over: the head may be sitting at the menu
        -- parking height right now, and reusing the value read back would
        -- start the new game with an invisible snake.
        jce.set_position(self.entity, wx, 0.0, wz)
        jce.set_position(self.prev_e, wx, 0.0, wz)
    end

    if st == 0 then
        -- MENU: park the head off-board with the body.
        --
        -- Without this the head sits alone at the centre of the board, on top
        -- of the menu text, and reads as a stray green square rather than as
        -- a snake -- the body is already parked, so there is nothing next to
        -- it to make it legible.  Visible in the first capture of the menu
        -- and in nothing else: no trace line carries "what the menu looks
        -- like", which is why it took a screenshot to find.
        local hx0, hy0, hz0 = jce.get_position(self.entity)
        if hy0 and hy0 > -50.0 then
            jce.set_position(self.entity, hx0, -100.0, hz0)
        end
    end

    if st ~= 1 then
        -- The trace has to cover the states where the clock is STOPPED too,
        -- or "the game is sitting in the menu" and "the game is wedged"
        -- produce the same evidence: no lines at all.  Throttled, because
        -- a menu can be on screen for minutes.
        self.idle = (self.idle or 0) + dt
        if self.idle >= 0.5 then
            self.idle = 0.0
            local pdx, intent, pdz = jce.get_position(self.pend_e)
            local dx, _, dz = jce.get_position(self.dir_e)
            -- enter= is a DIFFERENT question from intent=.  If the key is
            -- down and the intent is still 0, the input module is at fault;
            -- if the key never reads down, nothing below the input layer
            -- is.  One line that tells the two apart is worth more than
            -- two runs that do not.
            local enter = jce.is_key_down(40)
            -- head and the two verdicts belong here too: after a game over
            -- the clock stops, so the LAST thing that happened is only ever
            -- visible on an idle line.  Without them, "died at the wall"
            -- and "stopped for some other reason" look identical.
            local hx2, _, hz2 = jce.get_position(self.entity)
            local vw, vs = jce.get_position(self.verdict_e)
            jce.log(string.format(
                "SNAKEIDLE state=%d score=%d intent=%d pend=%d,%d dir=%d,%d "
                .. "enter=%s head=%d,%d wall=%d self=%d",
                st, score, intent, pdx, pdz, dx, dz, tostring(enter),
                to_cell_x(hx2), to_cell_z(hz2),
                vw >= 0.5 and 1 or 0, vs >= 0.5 and 1 or 0))
        end
        return
    end

    self.acc = self.acc + dt
    local step = step_for(score)
    if self.acc < step then return end
    self.acc = self.acc - step

    -- ONE direction change per tick (spec 15).  The pending pair is consumed
    -- HERE and nowhere else, so two presses inside a single tick cannot both
    -- land and the snake cannot be turned back into itself by a fast double
    -- tap.  The reversal test is against the direction being REPLACED, which
    -- is the one that is actually still in effect.
    local dx, turn, dz = jce.get_position(self.dir_e)
    local pdx, _, pdz  = jce.get_position(self.pend_e)
    if (pdx ~= 0 or pdz ~= 0) and not (pdx == -dx and pdz == -dz) then
        dx, dz = pdx, pdz
        jce.set_position(self.dir_e, dx, turn + 1, dz)
    end

    local hx, hy, hz = jce.get_position(self.entity)
    local cx, cz = to_cell(hx, hz)

    -- The trace is emitted BEFORE this tick's move, describing the world as
    -- it stands right now.
    --
    -- That placement is not cosmetic.  Emitting it after the move reports a
    -- body that has not been updated yet: the C++ module reacts to the tick
    -- counter, so during the frame this script moves the head, the segments
    -- still hold last tick's cells.  The trace then shows head and first
    -- segment two cells apart on every single line -- a continuity check
    -- reading it would fail a game that is completely correct, and the
    -- defect would be in the instrument.  Before the move, everything the
    -- other six modules wrote for the previous tick has settled.
    local fx, _, fz = jce.get_position(self.food_e)
    local bw, bs = jce.get_position(self.verdict_e)
    local _, blen = jce.get_position(self.meta_e)
    local dgx, dgy, dgz = jce.get_position(self.diag_e)
    local parts = {}
    local resolved = 0
    for i = 0, TRACE_BODY - 1 do
        local e = self.body_e[i]
        if e then
            resolved = resolved + 1
            local bx, by, bz = jce.get_position(e)
            if by and by > -50.0 then
                parts[#parts + 1] = string.format("%d,%d",
                    to_cell_x(bx), to_cell_z(bz))
            end
        end
    end
    local _, _, tk = jce.get_position(self.state_e)
    jce.log(string.format(
        -- bres= is how many body ENTITIES this script resolved by name, as
        -- distinct from how many are ON the board.  An empty body= can mean
        -- "the body module placed nothing" or "this script could not find
        -- the entities to look at", and those have nothing in common; with
        -- one number the two are the same observation.
        "SNAKETRACE tick=%d state=%d score=%d len=%d head=%d,%d food=%d,%d "
        .. "wall=%d self=%d bres=%d cpp=%d,%d,%d body=%s",
        tk, st, score, blen, cx, cz, to_cell_x(fx), to_cell_z(fz),
        bw >= 0.5 and 1 or 0, bs >= 0.5 and 1 or 0, resolved,
        dgx or -1, dgy or -1, dgz or -1,
        table.concat(parts, " ")))

    -- The cell the head is LEAVING, written as part of the same move.  The
    -- body module reads this rather than the head, so it does not depend on
    -- running before or after this script within the frame.
    jce.set_position(self.prev_e, hx, 0.0, hz)

    local nx, nz = to_world(cx + dx, cz + dz)
    jce.set_position(self.entity, nx, hy, nz)

    -- Advance the tick LAST: every other module treats a changed tick as
    -- "the world has moved", so it must not change before the world has.
    local s2, sc2, tick = jce.get_position(self.state_e)
    jce.set_position(self.state_e, s2, sc2, tick + 1)

end

return M
