-- Portable Lua controls for project rules. The jce table is a fake host.
local root = assert(arg[1], "pass the trigger_graph_lab project directory")
local beacon = 777 -- Simulated remap from authored scene ID 102.
local camera = 778
local position = {[beacon] = {0, 0, 0}, [camera] = {-3, 1, 8}}
local events = {}
local keys = {}
local moves = 0

jce = {
    get_param = function(entity, name)
        assert(entity == 779 and name == "target")
        return 3, 0, beacon
    end,
    get_position = function(entity)
        return table.unpack(position[entity])
    end,
    set_position = function(entity, x, y, z)
        assert(entity == beacon or entity == camera)
        position[entity] = {x, y, z}
        moves = moves + 1
    end,
    log = function(message) events[#events + 1] = message end,
    is_key_down = function(key) return keys[key] == true end
}

local gate = dofile(root .. "/resources/scripts/gate_rules.lua")
gate.entity = 779
gate:on_start()
gate:on_trigger_enter(1, "wrong_gate")
assert(moves == 0 and #events == 0)
gate:on_trigger_enter(1, "west_gate")
assert(position[beacon][2] == 2 and #events == 1)
gate:on_trigger_stay(1, "west_gate")
gate:on_trigger_stay(1, "west_gate")
assert(position[beacon][2] == 2 and #events == 2)
gate:on_trigger_exit(1, "west_gate")
assert(position[beacon][2] == 0 and #events == 3)
gate:on_trigger_enter(1, "west_gate")
gate:on_trigger_stay(1, "west_gate")
assert(position[beacon][2] == 2 and #events == 5)

local controls = dofile(root .. "/resources/scripts/observer_controls.lua")
controls.entity = camera
controls:on_start()
local before = moves
keys[31] = true
controls:on_update()
controls:on_update()
assert(position[camera][1] == 0 and moves == before + 1)
keys[31] = false
keys[32] = true
controls:on_update()
assert(position[camera][1] == 3 and position[camera][2] == 1
       and position[camera][3] == 8)
keys[32] = false
keys[30] = true
controls:on_update()
assert(position[camera][1] == -3)
print("Trigger Graph Lab Lua rules and key edges passed")
