-- caged_kingdom lightning laboratory.
--
-- A bounded dielectric-breakdown model grows a stepped leader through a
-- Laplace potential field.  The solve is spread across frames; rendering,
-- return stroke, flash, camera shake, and speed-of-sound thunder all consume
-- the same deterministic channel tree in editor Play and the shipped runtime.

local GRID_W = 31
local GRID_H = 25
local GRID_CELLS = GRID_W * GRID_H
local MAX_NODES = 124
local MIN_NODES_BEFORE_ATTACHMENT = 82
local MAX_LINE_POINTS = 64
local RELAXATIONS_PER_GROWTH = 5
local MAX_GROWTH_PER_FRAME = 7
local GROWTH_PER_SECOND = 380.0
local ETA = 1.65
local BRANCH_COUNT = 6
local ATTACHMENT_DURATION = 0.055
local RETURN_DURATION = 0.035
local DART_DURATION = 0.018
local STROKE_DECAY_DURATION = 0.105
local FINAL_AFTERGLOW_DURATION = 0.22
local SPEED_OF_SOUND_MPS = 343.0
local THUNDER_MIN_DISTANCE = 18.0
local THUNDER_MAX_DISTANCE = 2500.0
local THUNDER_ROLLOFF = 0.18
local BASE_EXPOSURE = 0.82
local CLOUD_HEIGHT = 24.0
local FIELD_WIDTH = 18.0
local FIELD_DEPTH = 5.0

-- The positions match the authored conductor tops in lightning_lab.scene.json.
-- Height and horizontal distance bias attachment, but every candidate remains
-- possible so deterministic strike sequences still vary.
local STRIKE_TARGETS = {
  {name = "LightningRod_Center", x = 0.0, y = 8.4, z = 0.0},
  {name = "LightningRod_West", x = -8.0, y = 5.6, z = -3.0},
  {name = "LightningRod_East", x = 8.0, y = 6.6, z = 2.0},
}

local D = {
  ids = {},
  branch_ids = {},
  phase = "idle",
  next_strike = 1.0,
  strike_serial = 0,
  seed = 1337,
  growth_accum = 0.0,
  return_t = 0.0,
  phase_t = 0.0,
  stroke_index = 0,
  stroke_total = 0,
  stroke_energy = 0.0,
  interstroke_delay = 0.0,
  thunder_events = {},
  main_points = {},
  downward_points = {},
  upward_points = {},
  branch_points = {},
}

local json = {}

local function json_escape(s)
  return s:gsub("\\", "\\\\")
          :gsub('"', '\\"')
          :gsub("\n", "\\n")
          :gsub("\r", "\\r")
          :gsub("\t", "\\t")
end

function json.encode(value)
  local kind = type(value)
  if kind == "nil" then return "null" end
  if kind == "boolean" then return value and "true" or "false" end
  if kind == "number" then
    if value ~= value or value == math.huge or value == -math.huge then
      return "0"
    end
    return string.format("%.9g", value)
  end
  if kind == "string" then return '"' .. json_escape(value) .. '"' end
  if kind ~= "table" then error("unsupported JSON value") end

  local count = 0
  local array = true
  for key, _ in pairs(value) do
    count = count + 1
    if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then array = false end
  end
  if array then
    for i = 1, count do
      if value[i] == nil then array = false; break end
    end
  end
  local out = {}
  if array then
    for i = 1, count do out[i] = json.encode(value[i]) end
    return "[" .. table.concat(out, ",") .. "]"
  end

  local keys = {}
  for key, _ in pairs(value) do keys[#keys + 1] = key end
  table.sort(keys)
  for i, key in ipairs(keys) do
    out[i] = json.encode(key) .. ":" .. json.encode(value[key])
  end
  return "{" .. table.concat(out, ",") .. "}"
end

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function rand01()
  D.seed = (D.seed * 48271) % 2147483647
  return D.seed / 2147483647
end

local function grid_index(x, y)
  return (y - 1) * GRID_W + x
end

local function reverse_points(points)
  local out = {}
  for i = #points, 1, -1 do out[#out + 1] = points[i] end
  return out
end

local function partial_points(points, fraction)
  if #points == 0 then return {} end
  local count = math.max(2, math.floor((#points - 1) *
                         clamp(fraction, 0.0, 1.0) + 1.5))
  count = math.min(count, #points)
  local out = {}
  for i = 1, count do out[i] = points[i] end
  return out
end

local function refine_points(points, roughness)
  if #points < 2 then return points end
  local source = points
  local max_source = math.floor((MAX_LINE_POINTS + 1) * 0.5)
  if #points > max_source then
    source = {}
    for i = 0, max_source - 1 do
      local index = math.floor(i * (#points - 1) /
                               (max_source - 1) + 0.5) + 1
      source[#source + 1] = points[index]
    end
  end

  local out = {source[1]}
  for i = 1, #source - 1 do
    local a, b = source[i], source[i + 1]
    local dx, dy, dz = b[1] - a[1], b[2] - a[2], b[3] - a[3]
    local length = math.sqrt(dx * dx + dy * dy + dz * dz)
    local horizontal = math.sqrt(dx * dx + dz * dz)
    local side_x, side_z = 1.0, 0.0
    if horizontal > 0.0001 then
      side_x, side_z = -dz / horizontal, dx / horizontal
    end
    local lateral = (rand01() - 0.5) * length * roughness
    local depth = (rand01() - 0.5) * length * roughness * 0.55
    local mid_y = 0.5 * (a[2] + b[2]) +
                  (rand01() - 0.5) * length * roughness * 0.10
    local min_y, max_y = math.min(a[2], b[2]), math.max(a[2], b[2])
    mid_y = clamp(mid_y, min_y, max_y)
    out[#out + 1] = {
      0.5 * (a[1] + b[1]) + side_x * lateral,
      mid_y,
      0.5 * (a[3] + b[3]) + side_z * lateral + depth,
    }
    out[#out + 1] = b
  end
  return out
end

local function select_strike_target()
  local weights = {}
  local total = 0.0
  for i, target in ipairs(STRIKE_TARGETS) do
    local dx = target.x - D.strike_center_x
    local dz = target.z - D.strike_center_z
    local distance = math.sqrt(dx * dx + dz * dz)
    local height = target.y / STRIKE_TARGETS[1].y
    local weight = (0.35 + height ^ 1.8) /
                   ((2.5 + distance) ^ 1.35)
    weights[i] = weight
    total = total + weight
  end
  local pick = rand01() * total
  for i, weight in ipairs(weights) do
    pick = pick - weight
    if pick <= 0.0 then
      D.target = STRIKE_TARGETS[i]
      return
    end
  end
  D.target = STRIKE_TARGETS[1]
end

local function resolve_one(name)
  local entity, count = jce.find_by_name(name)
  if count ~= 1 then
    jce.log("lightning_lab: expected one entity named '" .. name .. "'")
    return nil
  end
  return entity
end

local function resolve_entities()
  D.ids.leader = resolve_one("LightningLeader")
  D.ids.return_stroke = resolve_one("LightningReturnStroke")
  D.ids.core = resolve_one("LightningCore")
  D.ids.halo = resolve_one("LightningHalo")
  D.ids.upward_streamer = resolve_one("LightningUpwardStreamer")
  D.ids.impact_light = resolve_one("LightningImpactLight")
  D.ids.cloud_light = resolve_one("LightningCloudLight")
  D.ids.cloud_scatter_a = resolve_one("LightningCloudScatter_A")
  D.ids.cloud_scatter_b = resolve_one("LightningCloudScatter_B")
  D.ids.impact_glow = resolve_one("LightningImpactGlow")
  D.ids.camera = resolve_one("StormCamera")
  D.branch_ids = {}
  for i = 0, BRANCH_COUNT - 1 do
    D.branch_ids[#D.branch_ids + 1] = resolve_one("LightningBranch_" .. i)
  end
end

local function line_payload(points, width_start, width_end, color, alpha)
  local payload = {
    type = "LineRenderer",
    materialPath = "",
    positionCount = math.min(#points, MAX_LINE_POINTS),
    widthStart = width_start,
    widthEnd = width_end,
    colorStartR = color[1], colorStartG = color[2], colorStartB = color[3],
    colorStartA = alpha,
    colorEndR = color[1], colorEndG = color[2], colorEndB = color[3],
    colorEndA = alpha,
    useWorldSpace = true,
    loop = false,
  }
  local count = payload.positionCount
  if count > 0 then
    for i = 0, count - 1 do
      local source = 1
      if count > 1 then
        source = math.floor(i * (#points - 1) / (count - 1) + 0.5) + 1
      end
      local point = points[source]
      payload["px" .. i] = point[1]
      payload["py" .. i] = point[2]
      payload["pz" .. i] = point[3]
    end
  end
  return payload
end

local function write_line(entity, points, width_start, width_end, color, alpha)
  if not entity then return end
  jce.comp_set(entity, "LineRenderer",
    json.encode(line_payload(points, width_start, width_end, color, alpha)))
end

local function clear_lines()
  write_line(D.ids.leader, {}, 0.045, 0.018, {0.28, 0.58, 1.0}, 0.0)
  write_line(D.ids.return_stroke, {}, 0.14, 0.035, {0.92, 0.97, 1.0}, 0.0)
  write_line(D.ids.core, {}, 0.065, 0.018, {1.0, 0.985, 0.95}, 0.0)
  write_line(D.ids.halo, {}, 0.34, 0.11, {0.28, 0.48, 1.0}, 0.0)
  write_line(D.ids.upward_streamer, {}, 0.032, 0.012,
             {0.78, 0.88, 1.0}, 0.0)
  for _, entity in ipairs(D.branch_ids) do
    write_line(entity, {}, 0.026, 0.009, {0.36, 0.66, 1.0}, 0.0)
  end
end

local function set_light(entity, intensity, radius, color)
  if not entity then return end
  jce.comp_set(entity, "Light", json.encode({
    type = "Light", lightType = 1,
    colorR = color[1], colorG = color[2], colorB = color[3],
    intensity = intensity, radius = radius,
    castsShadow = false, shadowBias = 0.0,
  }))
end

local function set_glow(energy)
  local entity = D.ids.impact_glow
  if not entity then return end
  jce.comp_set(entity, "MeshRenderer", json.encode({
    type = "MeshRenderer", meshPath = "", materialPath = "", meshShape = 1,
    baseColorR = 0.45 + 0.55 * energy,
    baseColorG = 0.60 + 0.40 * energy,
    baseColorB = 1.0, baseColorA = clamp(energy, 0.0, 1.0),
    metallic = 0.0, roughness = 0.24,
    emissiveR = 2.0 * energy, emissiveG = 3.2 * energy,
    emissiveB = 5.0 * energy,
    normalScale = 1.0, aoStrength = 1.0,
    alphaMode = 2, alphaCutoff = 0.01,
    doubleSided = true, castsShadow = false, receivesShadow = false,
  }))
end

local function reset_flash()
  set_light(D.ids.impact_light, 0.0, 24.0, {0.62, 0.78, 1.0})
  set_light(D.ids.cloud_light, 0.0, 34.0, {0.42, 0.58, 1.0})
  set_light(D.ids.cloud_scatter_a, 0.0, 25.0, {0.36, 0.48, 0.86})
  set_light(D.ids.cloud_scatter_b, 0.0, 25.0, {0.36, 0.48, 0.86})
  set_glow(0.0)
  jce.render_set(json.encode({postfx = {exposure = BASE_EXPOSURE}}))
end

local function init_field()
  D.phi = {}
  D.channel = {}
  D.nodes = {}
  D.frontier = {}
  D.frontier_by_cell = {}
  D.ground_node = nil

  for y = 1, GRID_H do
    local potential = 1.0 - (y - 1) / (GRID_H - 1)
    for x = 1, GRID_W do D.phi[grid_index(x, y)] = potential end
  end

  local root_x = math.floor(GRID_W * (0.42 + rand01() * 0.16))
  local root = {x = root_x, y = GRID_H, parent = 0,
                z = 0.0, dz = 0.0}
  D.nodes[1] = root
  D.channel[grid_index(root.x, root.y)] = 1
  D.phi[grid_index(root.x, root.y)] = 0.0
end

local NEIGHBOURS = {
  {-1, -1}, {0, -1}, {1, -1},
  {-1,  0},          {1,  0},
}

local function add_frontier_from(node_index)
  local node = D.nodes[node_index]
  for _, offset in ipairs(NEIGHBOURS) do
    local x, y = node.x + offset[1], node.y + offset[2]
    if x >= 1 and x <= GRID_W and y >= 1 and y <= GRID_H then
      local cell = grid_index(x, y)
      if not D.channel[cell] and not D.frontier_by_cell[cell] then
        local candidate = {x = x, y = y, parent = node_index, cell = cell}
        D.frontier[#D.frontier + 1] = candidate
        D.frontier_by_cell[cell] = candidate
      end
    end
  end
end

local function solve_potential()
  for _ = 1, RELAXATIONS_PER_GROWTH do
    for y = 2, GRID_H - 1 do
      for x = 2, GRID_W - 1 do
        local cell = grid_index(x, y)
        if not D.channel[cell] then
          D.phi[cell] = 0.25 * (
            D.phi[grid_index(x - 1, y)] + D.phi[grid_index(x + 1, y)] +
            D.phi[grid_index(x, y - 1)] + D.phi[grid_index(x, y + 1)])
        end
      end
    end
  end
end

local function choose_frontier()
  local total = 0.0
  local weights = {}
  for i, candidate in ipairs(D.frontier) do
    local weight = 0.0
    if candidate.y > 1 or #D.nodes >= MIN_NODES_BEFORE_ATTACHMENT then
      local parent = D.nodes[candidate.parent]
      local potential = math.max(D.phi[candidate.cell] or 0.0, 0.0001)
      local direction_bias = candidate.y < parent.y and 3.8 or 0.42
      local ground_fraction = 1.0 - (candidate.y - 1) / (GRID_H - 1)
      local target_delta = math.abs(candidate.x - D.target_grid_x)
      local target_bias = 1.0 /
        (1.0 + target_delta * ground_fraction * 0.22)
      weight = (potential ^ ETA) * direction_bias * target_bias *
               (0.72 + rand01() * 0.56)
    end
    weights[i] = weight
    total = total + weight
  end
  if total <= 0.0 then
    for i, candidate in ipairs(D.frontier) do
      if candidate.y > 1 then return i end
    end
    return #D.frontier
  end
  local target = rand01() * total
  local cumulative = 0.0
  for i, weight in ipairs(weights) do
    cumulative = cumulative + weight
    if cumulative >= target then return i end
  end
  return #D.frontier
end

local function deepest_node()
  local best = 1
  for i = 2, #D.nodes do
    if D.nodes[i].y < D.nodes[best].y then best = i end
  end
  return best
end

local function grow_one()
  if #D.frontier == 0 then add_frontier_from(deepest_node()) end
  if #D.frontier == 0 then return false end
  solve_potential()
  local selected = choose_frontier()
  local candidate = D.frontier[selected]
  D.frontier[selected] = D.frontier[#D.frontier]
  D.frontier[#D.frontier] = nil
  D.frontier_by_cell[candidate.cell] = nil

  local parent = D.nodes[candidate.parent]
  local dz = (parent.dz or 0.0) * 0.34 + (rand01() - 0.5) * 1.05
  local node = {
    x = candidate.x, y = candidate.y, parent = candidate.parent,
    z = clamp(parent.z + dz, -FIELD_DEPTH * 0.5, FIELD_DEPTH * 0.5),
    dz = dz,
  }
  D.nodes[#D.nodes + 1] = node
  local node_index = #D.nodes
  D.channel[candidate.cell] = node_index
  D.phi[candidate.cell] = 0.0
  add_frontier_from(node_index)
  if node.y == 1 and #D.nodes >= MIN_NODES_BEFORE_ATTACHMENT then
    D.ground_node = node_index
  end
  return true
end

local function path_indices_to_root(node_index)
  local reversed = {}
  local cursor = node_index
  while cursor and cursor > 0 do
    reversed[#reversed + 1] = cursor
    cursor = D.nodes[cursor].parent
  end
  local path = {}
  for i = #reversed, 1, -1 do path[#path + 1] = reversed[i] end
  return path
end

local function node_world(node)
  local nx = (node.x - 1) / (GRID_W - 1) - 0.5
  local ny = (node.y - 1) / (GRID_H - 1)
  local ground_pull = (1.0 - ny) ^ 1.10
  local raw_x = D.strike_center_x + nx * FIELD_WIDTH
  local raw_z = D.strike_center_z + node.z
  local correction_x = D.ground_correction_x or
                       (D.target.x - D.strike_center_x)
  local correction_z = D.ground_correction_z or
                       (D.target.z - D.strike_center_z)
  return {
    raw_x + correction_x * ground_pull,
    D.target.y + ny * (CLOUD_HEIGHT - D.target.y),
    raw_z + correction_z * ground_pull,
  }
end

local function world_points(indices)
  local points = {}
  for _, index in ipairs(indices) do points[#points + 1] = node_world(D.nodes[index]) end
  return points
end

local function show_current_leader()
  local points = world_points(path_indices_to_root(deepest_node()))
  write_line(D.ids.leader, points, 0.048, 0.014, {0.28, 0.58, 1.0}, 0.62)
end

local function force_ground_connection()
  local cursor = deepest_node()
  while D.nodes[cursor].y > 1 and #D.nodes < MAX_NODES + GRID_H do
    local parent = D.nodes[cursor]
    local x = clamp(parent.x + (rand01() < 0.5 and -1 or 1), 1, GRID_W)
    local dz = (parent.dz or 0.0) * 0.3 + (rand01() - 0.5) * 0.8
    local node = {x = x, y = parent.y - 1, parent = cursor,
                  z = clamp(parent.z + dz, -FIELD_DEPTH * 0.5,
                            FIELD_DEPTH * 0.5), dz = dz}
    D.nodes[#D.nodes + 1] = node
    cursor = #D.nodes
  end
  D.ground_node = cursor
end

local function find_branch_paths(main_indices)
  local main = {}
  local children = {}
  for _, index in ipairs(main_indices) do main[index] = true end
  for i = 2, #D.nodes do
    local parent = D.nodes[i].parent
    children[parent] = (children[parent] or 0) + 1
  end

  local candidates = {}
  for i = 2, #D.nodes do
    if not main[i] and not children[i] then
      local reversed = {}
      local cursor = i
      while cursor and cursor > 0 and not main[cursor] and #reversed < 24 do
        reversed[#reversed + 1] = cursor
        cursor = D.nodes[cursor].parent
      end
      if cursor and main[cursor] and #reversed >= 2 then
        local indices = {cursor}
        for n = #reversed, 1, -1 do indices[#indices + 1] = reversed[n] end
        candidates[#candidates + 1] = indices
      end
    end
  end
  table.sort(candidates, function(a, b) return #a > #b end)
  return candidates
end

local function extract_branches(main_indices)
  local candidates = find_branch_paths(main_indices)
  D.branch_points = {}
  for i = 1, BRANCH_COUNT do
    D.branch_points[i] = candidates[i] and
      refine_points(world_points(candidates[i]), 0.24) or {}
  end
end

local function show_growth_branches()
  local main_indices = path_indices_to_root(deepest_node())
  local candidates = find_branch_paths(main_indices)
  for i, entity in ipairs(D.branch_ids) do
    local points = candidates[i] and world_points(candidates[i]) or {}
    write_line(entity, points, 0.019, 0.005,
               {0.22, 0.36, 0.74}, 0.16)
  end
end

local function split_attachment_path()
  local attachment_y = D.target.y + 2.5
  local split = math.max(1, #D.main_points - 2)
  for i = #D.main_points - 1, 1, -1 do
    if D.main_points[i][2] >= attachment_y then
      split = i
      break
    end
  end

  D.downward_points = {}
  for i = 1, split do
    D.downward_points[#D.downward_points + 1] = D.main_points[i]
  end
  D.upward_points = {}
  for i = #D.main_points, split, -1 do
    D.upward_points[#D.upward_points + 1] = D.main_points[i]
  end
end

local function finish_growth()
  if not D.ground_node then force_ground_connection() end
  local ground = D.nodes[D.ground_node]
  local ground_nx = (ground.x - 1) / (GRID_W - 1) - 0.5
  D.ground_correction_x = D.target.x -
    (D.strike_center_x + ground_nx * FIELD_WIDTH)
  D.ground_correction_z = D.target.z -
    (D.strike_center_z + ground.z)
  local main_indices = path_indices_to_root(D.ground_node)
  D.main_points = refine_points(world_points(main_indices), 0.28)
  D.return_points = reverse_points(D.main_points)
  extract_branches(main_indices)
  split_attachment_path()
  write_line(D.ids.leader, D.downward_points, 0.036, 0.012,
             {0.24, 0.38, 0.76}, 0.32)
  for i, entity in ipairs(D.branch_ids) do
    write_line(entity, D.branch_points[i], 0.023, 0.006,
               {0.27, 0.42, 0.82}, 0.24)
  end
  D.phase_t = 0.0
  D.phase = "attachment"
end

local function update_growth(dt)
  D.growth_accum = D.growth_accum + dt * GROWTH_PER_SECOND
  local count = math.min(math.floor(D.growth_accum), MAX_GROWTH_PER_FRAME)
  if count <= 0 then return end
  D.growth_accum = D.growth_accum - count
  for _ = 1, count do
    if D.ground_node or #D.nodes >= MAX_NODES or not grow_one() then break end
  end
  show_current_leader()
  show_growth_branches()
  if D.ground_node or #D.nodes >= MAX_NODES then finish_growth() end
end

local function schedule_thunder()
  local impact = D.main_points[#D.main_points]
  local middle = D.main_points[math.max(1, math.floor(#D.main_points * 0.55))]
  local camera = {impact[1], impact[2], impact[3]}
  if D.ids.camera then
    local cx, cy, cz = jce.get_position(D.ids.camera)
    if cx then camera = {cx, cy, cz} end
  end

  local function distance_to(point)
    local dx = camera[1] - point[1]
    local dy = camera[2] - point[2]
    local dz = camera[3] - point[3]
    return math.sqrt(dx * dx + dy * dy + dz * dz)
  end

  local crack_delay = distance_to(impact) / SPEED_OF_SOUND_MPS
  local roll_delay = distance_to(middle) / SPEED_OF_SOUND_MPS + 0.16
  D.thunder_events[#D.thunder_events + 1] = {
    delay = crack_delay, path = "sounds/lightning_lab_thunder_crack.wav",
    pos = {impact[1], impact[2], impact[3]}, volume = 0.98,
  }
  D.thunder_events[#D.thunder_events + 1] = {
    delay = roll_delay, path = "sounds/lightning_lab_thunder_roll.wav",
    pos = {middle[1], middle[2], middle[3]}, volume = 0.78,
  }
end

local function position_flash_lights()
  local impact = D.main_points[#D.main_points]
  local cloud = D.main_points[1]
  if D.ids.impact_light then
    jce.set_position(D.ids.impact_light, impact[1], impact[2] + 1.0, impact[3])
  end
  if D.ids.cloud_light then
    jce.set_position(D.ids.cloud_light, cloud[1], cloud[2] - 2.0, cloud[3])
  end
  if D.ids.impact_glow then
    jce.set_position(D.ids.impact_glow, impact[1], impact[2] + 0.12, impact[3])
  end
  if D.ids.cloud_scatter_a then
    jce.set_position(D.ids.cloud_scatter_a,
                     cloud[1] - 5.0, cloud[2] - 1.5, cloud[3] + 2.5)
  end
  if D.ids.cloud_scatter_b then
    jce.set_position(D.ids.cloud_scatter_b,
                     cloud[1] + 5.0, cloud[2] - 2.4, cloud[3] - 2.0)
  end
end

local function render_flash_channels(points, energy)
  write_line(D.ids.return_stroke, points, 0.16, 0.045,
             {0.82, 0.90, 1.0}, clamp(energy, 0.0, 1.0))
  write_line(D.ids.core, points, 0.064, 0.018,
             {1.0, 0.985, 0.95}, clamp(energy * 1.45, 0.0, 1.0))
  write_line(D.ids.halo, points, 0.38, 0.12,
             {0.24, 0.42, 1.0}, clamp(energy * 0.27, 0.0, 0.34))
  write_line(D.ids.leader, D.main_points, 0.035, 0.011,
             {0.38, 0.50, 0.86}, clamp(energy * 0.16, 0.0, 0.22))
  for i, entity in ipairs(D.branch_ids) do
    write_line(entity, D.branch_points[i], 0.024, 0.006,
               {0.42, 0.54, 0.90}, clamp(energy * 0.30, 0.0, 0.34))
  end
end

local function apply_flash_lighting(energy)
  local fast = clamp(energy * energy, 0.0, 1.0)
  set_light(D.ids.impact_light, 72.0 * fast, 30.0, {0.82, 0.90, 1.0})
  set_light(D.ids.cloud_light, 38.0 * fast, 42.0, {0.54, 0.66, 1.0})
  set_light(D.ids.cloud_scatter_a, 19.0 * fast, 28.0,
            {0.38, 0.48, 0.88})
  set_light(D.ids.cloud_scatter_b, 16.0 * fast, 27.0,
            {0.34, 0.44, 0.82})
  set_glow(fast)
  jce.render_set(json.encode({postfx = {
    exposure = BASE_EXPOSURE + 0.92 * fast,
  }}))
end

local function begin_return(stroke_index)
  D.stroke_index = stroke_index
  D.return_t = 0.0
  D.phase = "return"
end

local function complete_return()
  D.stroke_energy = D.stroke_index == 1 and 1.0 or
    (0.76 ^ (D.stroke_index - 1)) * (0.88 + rand01() * 0.10)
  position_flash_lights()
  render_flash_channels(D.return_points, D.stroke_energy)
  apply_flash_lighting(D.stroke_energy)
  jce.shake_camera(0.58 * D.stroke_energy)
  if D.stroke_index == 1 then schedule_thunder() end
  D.phase_t = 0.0
  D.phase = "stroke_decay"
end

local function update_attachment(dt)
  D.phase_t = D.phase_t + dt
  local fraction = clamp(D.phase_t / ATTACHMENT_DURATION, 0.0, 1.0)
  write_line(D.ids.leader, D.downward_points, 0.036, 0.011,
             {0.26, 0.40, 0.78}, 0.28 + 0.08 * (1.0 - fraction))
  write_line(D.ids.upward_streamer,
             partial_points(D.upward_points, fraction),
             0.030, 0.010, {0.72, 0.84, 1.0}, 0.56)
  if fraction >= 1.0 then
    write_line(D.ids.leader, D.main_points, 0.036, 0.011,
               {0.30, 0.44, 0.82}, 0.34)
    D.stroke_total = 2 + math.floor(rand01() * 3.0)
    begin_return(1)
  end
end

local function update_return(dt)
  D.return_t = D.return_t + dt
  local fraction = clamp(D.return_t / RETURN_DURATION, 0.0, 1.0)
  render_flash_channels(partial_points(D.return_points, fraction), 1.0)
  apply_flash_lighting(0.20 + 0.80 * fraction)
  if fraction >= 1.0 then complete_return() end
end

local function update_stroke_decay(dt)
  D.phase_t = D.phase_t + dt
  local remaining = clamp(1.0 - D.phase_t / STROKE_DECAY_DURATION, 0.0, 1.0)
  local energy = D.stroke_energy * remaining * remaining * remaining
  render_flash_channels(D.return_points, energy)
  apply_flash_lighting(energy)
  if remaining <= 0.0 then
    reset_flash()
    if D.stroke_index < D.stroke_total then
      D.interstroke_delay = 0.045 + rand01() * 0.050
      D.phase_t = 0.0
      D.phase = "interstroke"
    else
      D.phase_t = 0.0
      D.phase = "final_afterglow"
    end
  end
end

local function update_interstroke(dt)
  D.phase_t = D.phase_t + dt
  write_line(D.ids.leader, D.main_points, 0.028, 0.009,
             {0.24, 0.32, 0.62}, 0.035)
  if D.phase_t >= D.interstroke_delay then
    D.phase_t = 0.0
    D.phase = "dart"
  end
end

local function update_dart(dt)
  D.phase_t = D.phase_t + dt
  local fraction = clamp(D.phase_t / DART_DURATION, 0.0, 1.0)
  write_line(D.ids.leader, partial_points(D.main_points, fraction),
             0.032, 0.010, {0.48, 0.58, 0.92}, 0.22)
  if fraction >= 1.0 then begin_return(D.stroke_index + 1) end
end

local function update_final_afterglow(dt)
  D.phase_t = D.phase_t + dt
  local energy = clamp(1.0 - D.phase_t / FINAL_AFTERGLOW_DURATION, 0.0, 1.0)
  render_flash_channels(D.return_points, energy * 0.075)
  if energy <= 0.0 then
    clear_lines()
    reset_flash()
    D.phase = "idle"
    D.next_strike = 7.0 + rand01() * 4.0
  end
end

local function update_thunder(dt)
  local i = 1
  while i <= #D.thunder_events do
    local event = D.thunder_events[i]
    event.delay = event.delay - dt
    if event.delay <= 0.0 then
      local p = event.pos
      jce.play_sound(event.path, p[1], p[2], p[3], event.volume,
                     THUNDER_MIN_DISTANCE, THUNDER_MAX_DISTANCE,
                     THUNDER_ROLLOFF)
      table.remove(D.thunder_events, i)
    else
      i = i + 1
    end
  end
end

local function begin_strike()
  D.strike_serial = D.strike_serial + 1
  D.seed = (1337 + D.strike_serial * 7919) % 2147483647
  D.strike_center_x = (rand01() - 0.5) * 12.0
  D.strike_center_z = (rand01() - 0.5) * 7.0
  select_strike_target()
  D.target_grid_x = clamp(math.floor(
    ((D.target.x - D.strike_center_x) / FIELD_WIDTH + 0.5) *
    (GRID_W - 1) + 1.5), 1, GRID_W)
  D.growth_accum = 0.0
  D.main_points = {}
  D.downward_points = {}
  D.upward_points = {}
  D.branch_points = {}
  D.stroke_index = 0
  D.stroke_total = 0
  D.phase_t = 0.0
  D.ground_correction_x = nil
  D.ground_correction_z = nil
  clear_lines()
  reset_flash()
  init_field()
  add_frontier_from(1)
  show_current_leader()
  jce.log("lightning_lab: stepped leader targeting " .. D.target.name)
  D.phase = "growth"
  D.next_strike = 999.0
end

function ck_lightning_trigger()
  begin_strike()
end

local M = {}

function M:on_start()
  resolve_entities()
  clear_lines()
  reset_flash()
  D.phase = "idle"
  D.next_strike = 1.0
  D.thunder_events = {}
end

function M:on_update(dt)
  dt = clamp(dt or 0.0, 0.0, 0.1)
  update_thunder(dt)
  if D.phase == "growth" then update_growth(dt)
  elseif D.phase == "attachment" then update_attachment(dt)
  elseif D.phase == "return" then update_return(dt)
  elseif D.phase == "stroke_decay" then update_stroke_decay(dt)
  elseif D.phase == "interstroke" then update_interstroke(dt)
  elseif D.phase == "dart" then update_dart(dt)
  elseif D.phase == "final_afterglow" then update_final_afterglow(dt)
  else
    D.next_strike = D.next_strike - dt
    if D.next_strike <= 0.0 then begin_strike() end
  end
end

return M
