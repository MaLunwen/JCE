-- extra.lua — a second, minimal script, loaded through jce_script_instantiate
-- (i.e. through JceScriptHost::read_file) rather than from a source string.
-- Its only job is to prove that path works and that two scripts coexist in one
-- VM without seeing each other's state.

local M = {}

function M:on_start()
    jce.set_position(80, self.entity, 0, 0)
end

return M
