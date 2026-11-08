local M = {}

-- One Event -> Condition -> Action table for this authored scene.
local rules = {
    enter = {tag = "west_gate", height = 2, report = "raised"},
    stay = {tag = "west_gate", report = "held"},
    exit = {tag = "west_gate", height = 0, report = "lowered"}
}

function M:on_start()
    local kind, _, target = jce.get_param(self.entity, "target")
    if kind == 3 and target and target ~= 0 then
        self.target = target
        self.x, self.y, self.z = jce.get_position(target)
    end
    if self.x == nil then
        jce.log("TRIGGER_GRAPH_LAB missing_target")
    end
end

local function dispatch(self, event, observer, tag)
    local rule = rules[event]
    if tag ~= rule.tag or self.x == nil then return end
    if event == "stay" and self.reported_stay then return end
    if event == "enter" then self.reported_stay = false end
    if event == "stay" then self.reported_stay = true end
    if rule.height then
        jce.set_position(self.target, self.x, self.y + rule.height, self.z)
    end
    jce.log(string.format("TRIGGER_GRAPH_LAB event=%s tag=%s observer=%s action=%s",
                          event, tag, tostring(observer), rule.report))
end

function M:on_trigger_enter(observer, tag)
    dispatch(self, "enter", observer, tag)
end

function M:on_trigger_stay(observer, tag)
    dispatch(self, "stay", observer, tag)
end

function M:on_trigger_exit(observer, tag)
    dispatch(self, "exit", observer, tag)
end

return M
