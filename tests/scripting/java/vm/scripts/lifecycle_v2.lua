-- lifecycle_v2.lua — the hot-reload target for scripts/lifecycle.lua.
--
-- jce_script_rebind_instance swaps the METHODS and keeps the per-instance
-- STATE.  That pair is the whole contract, and a test that checked only one
-- half would pass for a rebind that reset `self`.  So on_update reports a
-- DIFFERENT code (21, not 20) — proving the new methods are live — while
-- continuing to count from the value the old one left behind.

local M = {}

local function report(code, a, b)
    jce.set_position(code, a, b, 0)
end

function M:on_update(dt)
    self.count = self.count + 1
    report(21, self.count, dt)
end

function M:on_destroy()
    report(61, self.count, 0)
end

return M
