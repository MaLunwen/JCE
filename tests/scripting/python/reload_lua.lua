-- reload_lua.lua -- the module compile_module/rebind_instance swaps in.
--
-- Deliberately a DIFFERENT on_update from lifecycle_lua.lua, and no
-- on_destroy: rebinding must replace the whole method table, so "v2" proves
-- the swap happened and the absence of on_destroy proves it was a REPLACEMENT
-- rather than an overlay.
local M = {}

function M.on_update(self, dt)
	jce.log("v2")
end

return M
