local M = {}

-- JCE physical key codes for the top-row digits 1, 2 and 3.
local stations = {
    {key = 30, x = -3},
    {key = 31, x = 0},
    {key = 32, x = 3}
}

function M:on_start()
    self.keys = {}
    local _, y, z = jce.get_position(self.entity)
    self.y, self.z = y, z
end

function M:on_update()
    for i, station in ipairs(stations) do
        local down = jce.is_key_down(station.key)
        if down and not self.keys[i] then
            jce.set_position(self.entity, station.x, self.y, self.z)
        end
        self.keys[i] = down
    end
end

return M
