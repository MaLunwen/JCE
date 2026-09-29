-- lifecycle_lua.lua -- the REFERENCE half of the lifecycle differential.
--
-- Lua is the reference lifecycle.  This file and lifecycle_py.py express the
-- same script semantics in the two languages; lifecycle_runner.c drives BOTH
-- through the public jce_script_* entry points, over the same recording mock
-- host, and lifecycle_differential.py compares the two recorded streams.
--
-- RULES THIS FIXTURE FOLLOWS, EACH FOR A MEASURED REASON:
--
--   * No number is ever formatted into text by the script.  Lua's
--     string.format and Python's % are both C-printf-shaped but not the same
--     implementation, and a float that round-trips differently would be a
--     RED with no defect behind it.  Numbers go through host calls, where the
--     mock formats them once, in C, for both sides.
--
--   * A value's TYPE is reported through `tname`, not assumed.  nil, 0 and
--     false are three different things and a differential that compared only
--     the value would pass all three against each other -- which is the
--     failure the brief for this work names.  The nil<->None mapping is the
--     contract emit_python.py states ("None is Python's spelling of Lua's
--     nil"); everything else maps to itself.
--
--   * `jce.log` carries only literal markers.  It is the wire both languages
--     write to, and the mock appends it to the same ordered buffer as every
--     host call, which is what makes "the host call happened BEFORE the error
--     line" a comparable fact rather than two facts to correlate.

local M = {}

local function tname(v)
	local t = type(v)
	if t == "nil" then return "nil" end
	if t == "boolean" then return "boolean" end
	if t == "number" then return "number" end
	if t == "string" then return "string" end
	return t
end

-- ── instance lifecycle ────────────────────────────────────────────────────

function M.on_start(self)
	jce.log("on_start")
	self.ticks = 0
	jce.set_position(self.entity, 1.5, 2.25, -0.5)
	-- Started here on purpose: Lua resumes the body up to its first wait
	-- INSIDE start_coroutine, so "co-a" belongs to on_start's stream and not
	-- to the first update_coroutines.  That ordering is the whole reason this
	-- case exists.
	jce.start_coroutine(function()
		jce.log("co-a")
		jce.wait_seconds(0.5)
		jce.log("co-b")
	end)
end

function M.on_update(self, dt)
	self.ticks = self.ticks + 1
	jce.log("on_update")
	jce.set_velocity(self.entity, dt, 0.0, 0.0)
	-- The third tick raises.  The engine must log it and KEEP GOING: the
	-- fourth tick below is what proves the dispatch after an error still runs.
	if self.ticks == 3 then
		error("boom")
	end
end

function M.on_fixed_update(self, dt)
	-- Logs the dt so the differential compares a VALUE, not just a call: two
	-- backends that both printed a bare "on_fixed_update" would agree while
	-- passing different numbers, and silence compared with silence is equal.
	jce.log("on_fixed_update " .. tostring(dt))
end

function M.on_destroy(self)
	jce.log("on_destroy")
end

function M.on_collision(self, other)
	jce.log("on_collision")
	jce.destroy_entity(other)
end

-- Dispatched by name through call_message, so the method name is the message.
function M.on_ping(self, num, str)
	jce.log("on_ping")
	jce.anim_set_float(self.entity, tname(str), num)
end

function M.on_anim_event(self, id, name, f0, f1, i0)
	jce.log("on_anim_event")
	jce.ui_set_text(self.entity, tname(name))
	jce.anim_set_float(self.entity, "f0", f0)
	jce.anim_set_float(self.entity, "f1", f1)
	jce.anim_set_int(self.entity, "i0", i0)
	jce.anim_set_int(self.entity, "id", id)
end

-- ── named globals: the two slots whose absence is invisible ───────────────
--
-- These are GLOBAL, not members of M.  jce_script_call_named* looks a global
-- up by name; every UISlider / UIToggle / UIDropdown / UIInputField handler
-- in the engine arrives this way, and both call_named_num and call_named_str
-- return false for "no such global" -- indistinguishable from a correctly
-- absent handler.  A VM that omitted them would break every UI callback in
-- the game and report nothing, anywhere.

function on_named_hit(entity)
	jce.log("named")
	jce.destroy_entity(entity)
end

function on_slider(entity, value)
	jce.log("named_num")
	jce.anim_set_float(entity, "slider", value)
end

function on_field(entity, text)
	jce.log("named_str")
	jce.ui_set_text(entity, tname(text))
end

return M
