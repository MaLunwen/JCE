-- lifecycle.lua — THE REFERENCE.
--
-- Every observable this script produces goes through the host, because the
-- host is the one thing both languages share: jce.set_position(code, a, b, 0)
-- carries a numbered event and two floats, and the recorder writes the float's
-- IEEE bits.  Nothing is formatted into a string by the script — C's %g and
-- Java's %g disagree, and a differential that compares printf output is
-- comparing printf.
--
-- The Java expression of this same script is scripts/lifecycle.java.  They are
-- meant to be read side by side; a change to one that is not made to the other
-- is a failing differential, which is the point.

local M = {}

local function report(code, a, b)
    jce.set_position(code, a, b, 0)
end

function M:on_start()
    self.count = 0
    self.boom = false
    jce.log("start")
    report(1, self.entity, 0)

    -- A fallible host call that SUCCEEDS: three numbers.
    local x, y = jce.get_position(self.entity)
    report(2, x or -1, y or -1)

    -- The same call that FAILS.  Lua sees nil; Java sees false with the array
    -- untouched.  Reporting the discrimination as 1/0 is how two languages
    -- with different spellings of absence answer the same question.
    local missing = jce.get_position(4242)
    report(3, (missing ~= nil) and 1 or 0, 0)

    -- Runs immediately up to the first wait, then again 0.5s of scheduler time
    -- later.  jce_script_update_coroutines is what advances it.
    jce.start_coroutine(function()
        report(10, 0, 0)
        jce.wait_seconds(0.5)
        report(11, 0, 0)
    end)
end

function M:on_update(dt)
    self.count = self.count + 1
    report(20, self.count, dt)
    if self.boom then
        self.boom = false
        error("update boom")
    end
end

-- Code 21, beside on_update's 20.  Reports the dt so the differential
-- compares a VALUE: two backends that merely both called something would
-- agree while passing different numbers, and this suite's own history is
-- that silence compared with silence reads as equal.
function M:on_fixed_update(dt)
    report(21, 1, dt)
end

function M:on_collision(other)
    report(30, other, 0)
end

-- Message dispatch.  `s` has three states and they are three different
-- answers: nil (the C side passed NULL), the empty string, and text.
function M:ping(n, s)
    local tag
    if s == nil then tag = 0
    elseif s == "" then tag = 1
    else tag = 2 end
    report(40, n, tag)
end

-- Declared with NO parameters although the dispatcher passes two: Lua drops
-- the extras, and the Java side's named/message resolver has the same rule for
-- the same reason.
function M:arm_boom()
    self.boom = true
    report(41, 1, 0)
end

function M:on_anim_event(id, name, f0, f1, i0)
    local tag
    if name == nil then tag = 0
    elseif name == "" then tag = 1
    else tag = 2 end
    report(50, id, tag)
    report(51, f0, f1)
    report(52, i0, 0)
end

function M:on_destroy()
    report(60, self.count, 0)
end

-- Global handlers: what UIButton / UISlider / UIInputField dispatch into.
function on_ping(e)
    report(70, e, 0)
end

function on_value(e, v)
    report(71, e, v)
end

function on_text(e, s)
    report(72, e, (s == nil) and -1 or #s)
end

function on_boom(e)
    error("boom from on_boom")
end

return M
