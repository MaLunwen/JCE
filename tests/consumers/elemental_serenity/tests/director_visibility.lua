-- Execute complete current Directors against the frozen, unmodified old owner.
-- Counted public host ports model flat authored component JSON and patch merge;
-- they do not execute the Engine serializer, renderer or a native Play session.
-- No private Lua upvalues, function extraction or source-text assertions are used.
local assets = assert(arg[1], "expected resources/assets/scripts directory")
local frozen = assert(arg[2], "expected frozen old portable Director path")
assert(_VERSION == "Lua 5.4", "this fixture requires the existing Lua 5.4 mechanism")

local function read(path)
  local file = assert(io.open(path, "rb"))
  local text = file:read("a"); file:close(); return text
end
local reference_source = read(frozen)
local sources = {
  {name = "portable", source = read(assets .. "/es_director_portable.lua")},
  {name = "authored", source = read(assets .. "/es_director.lua")},
}
local failures, assertions, cases = 0, 0, 0
local function check(value, message)
  assertions = assertions + 1
  if not value then
    failures = failures + 1
    if failures <= 40 then io.stderr:write("ES_VISIBILITY_FAIL " .. message .. "\n") end
  end
end
local function clone(value)
  local copy = {}; for key, item in pairs(value) do copy[key] = item end; return copy
end
local function encode_flat(value)
  local fields = {}
  for key, item in pairs(value) do
    local text = type(item) == "number" and string.format("%.17g", item)
      or type(item) == "boolean" and tostring(item) or string.format("%q", item)
    fields[#fields + 1] = string.format('"%s":%s', key, text)
  end
  table.sort(fields); return "{" .. table.concat(fields, ",") .. "}"
end
local function decode_flat(text)
  local value, count = {}, 0
  for key, item in text:gmatch('"([^"]+)"%s*:%s*([^,}]+)') do
    count = count + 1
    if item == "true" then value[key] = true
    elseif item == "false" then value[key] = false
    elseif item:sub(1, 1) == '"' then value[key] = item:sub(2, -2)
    else value[key] = assert(tonumber(item), "invalid flat host-port payload") end
  end
  return value, count
end

local function instance(source, options)
  options = options or {}
  local base = options.generation and 4294967296 or 0
  local p = {components = {}, flowers = {}, foliage = {}, grass = {}, initial = {},
             mesh_gets = 0, mesh_sets = 0, full_mesh_patches = 0,
             foliage_sets = 0, grass_sets = 0, render_sets = 0, created_mesh = 0,
             mesh_rejections = 0, mesh_journal = options.track_mesh and {} or nil,
             existing_flower_count = options.missing and 101 or 102}
  for i = 1, 102 do
    local e = base + 1000 + i
    p.flowers[i] = e
    if i ~= options.missing then
      local value = {type = "MeshRenderer", meshPath = "models/card.glb",
        materialPath = "", baseColorR = .75, baseColorG = .5, baseColorB = .25,
        roughness = 1, metallic = 0, alphaCutoff = .4, alphaMode = 1,
        visible = i % 3 == 0, doubleSided = true, castsShadow = false,
        receivesShadow = true, albedoTex = "textures/flower_" .. i .. ".png"}
      p.components[e] = {MeshRenderer = clone(value)}
      p.initial[e] = clone(value)
    else p.components[e] = {} end
  end
  for i = 1, 40 do
    local e = base + 2000 + i; p.foliage[i] = e
    p.components[e] = {FoliageCluster = {type = "FoliageCluster", leafCount = 45,
      radius = 1.2, squashY = .8, leafScale = 1, seed = i, visible = true,
      shadowR = .1, shadowG = .1, shadowB = .1,
      midR = .2, midG = .2, midB = .2, highR = .3, highG = .3, highB = .3,
      multR = 1, multG = 1, multB = 1, alphaTex = "textures/leaf.png"}}
  end
  for i = 1, 4 do
    local e = base + 3000 + i; p.grass[i] = e
    p.components[e] = {GrassField = {type = "GrassField", seed = i, cards = 4,
      density = 8, areaX = 2, areaZ = 2, visible = true,
      rootR = .1, rootG = .2, rootB = .3, tipR = .4, tipG = .5, tipB = .6}}
  end
  local host = {
    get_param = function() return nil end,
    find_by_name = function() return nil end,
    find_by_prefix = function(prefix)
      if prefix == "flower_" then return p.flowers end
      if prefix == "BushC" then return p.foliage end
      if prefix == "Grass" then return p.grass end
      return {}
    end,
    comp_get = function(e, kind)
      if kind == "MeshRenderer" then p.mesh_gets = p.mesh_gets + 1 end
      local row = p.components[e] and p.components[e][kind]
      return row and encode_flat(row) or nil
    end,
    has_component = function(e, kind)
      return p.components[e] ~= nil and p.components[e][kind] ~= nil
    end,
    comp_set = function(e, kind, text)
      local patch, fields = decode_flat(text)
      local row = assert(p.components[e], "wrong/stale full entity identity")[kind]
      if kind == "MeshRenderer" then
        p.mesh_sets = p.mesh_sets + 1
        if fields ~= 1 or type(patch.visible) ~= "boolean" then
          p.full_mesh_patches = p.full_mesh_patches + 1
        end
        if not row then p.created_mesh = p.created_mesh + 1 end
        local accepted = p.mesh_sets ~= p.reject_mesh_at
        if p.mesh_journal then
          p.mesh_journal[#p.mesh_journal + 1] = {
            entity = e, visible = patch.visible, accepted = accepted,
          }
        end
        if not accepted then
          p.reject_mesh_at = nil -- One refused write, with no host mutation.
          p.mesh_rejections = p.mesh_rejections + 1
          return false
        end
      elseif kind == "FoliageCluster" then p.foliage_sets = p.foliage_sets + 1
      elseif kind == "GrassField" then p.grass_sets = p.grass_sets + 1
      else error("unexpected component port " .. tostring(kind)) end
      if not row then row = {}; p.components[e][kind] = row end
      for key, item in pairs(patch) do row[key] = item end
      return true
    end,
    render_set = function() p.render_sets = p.render_sets + 1; return true end,
    log = function() end,
    play_sound = function() end,
  }
  local env = setmetatable({jce = host}, {__index = _G})
  p.script = assert(load(source, "@actual-complete-director", "t", env))()
  p.self = setmetatable({entity = base + 186}, {__index = p.script})
  p.env = env
  function p:start() self.script.on_start(self.self) end
  function p:update(dt) self.script.on_update(self.self, dt) end
  function p:state(name) assert(self.env["es_state_" .. name])() end
  return p
end

local function same_components(current, reference, label)
  local same = true
  for e, kinds in pairs(reference.components) do
    local actual = current.components[e]
    if not actual then same = false; break end
    for kind, expected in pairs(kinds) do
      if not actual[kind] or encode_flat(actual[kind]) ~= encode_flat(expected) then
        same = false; break
      end
    end
    for kind in pairs(actual) do if not kinds[kind] then same = false end end
    if not same then break end
  end
  check(same, label .. ": visible, authored non-visible fields or dynamic colors diverged")
end
local function visibility_changes(p, before)
  local changes = 0
  for _, e in ipairs(p.flowers) do
    local value = p.components[e].MeshRenderer
    if value and before[e] ~= value.visible then changes = changes + 1 end
  end
  return changes
end
local function visibility(p)
  local values = {}
  for _, e in ipairs(p.flowers) do
    local value = p.components[e].MeshRenderer
    if value then values[e] = value.visible end
  end
  return values
end
local baseline_calls = {mixes = 0, gets = 0, sets = 0}
local dts = {0, 1 / 600, 1 / 165, .05, .2, .3, .6}
local function transition(current, reference, state, label)
  local before = visibility(reference)
  local gets, writes = current.mesh_gets, current.mesh_sets
  local ref_gets, ref_sets = reference.mesh_gets, reference.mesh_sets
  local fol, grass = current.foliage_sets, current.grass_sets
  local ref_fol, ref_grass = reference.foliage_sets, reference.grass_sets
  current:state(state); reference:state(state)
  for i, dt in ipairs(dts) do
    current:update(dt); reference:update(dt)
    same_components(current, reference, label .. " step" .. i)
  end
  local wanted = visibility_changes(reference, before)
  check(current.mesh_gets == gets, label .. ": hot tween still reads flower component JSON")
  check(current.mesh_sets - writes == wanted,
        label .. ": expected changed-only visibility writes=" .. wanted ..
        " actual=" .. (current.mesh_sets - writes))
  check(current.foliage_sets - fol == reference.foliage_sets - ref_fol and
        current.grass_sets - grass == reference.grass_sets - ref_grass,
        label .. ": dynamic40 foliage /4 grass color writes were dropped")
  local legacy_mixes = (reference.mesh_gets - ref_gets) / 102
  check(legacy_mixes == math.floor(legacy_mixes) and
        reference.mesh_sets - ref_sets == legacy_mixes * reference.existing_flower_count,
        label .. ": frozen old owner did not execute the expected102-flower loop")
  if baseline_calls.mixes == 0 then
    check(legacy_mixes == #dts, "frozen owner did not execute every first tween update")
    print(string.format("ES_VISIBILITY_OLD_MIX updates=%d flower_comp_get=%d flower_comp_set=%d",
                        legacy_mixes, reference.mesh_gets - ref_gets,
                        reference.mesh_sets - ref_sets))
  end
  baseline_calls.mixes = baseline_calls.mixes + 1
  baseline_calls.gets = baseline_calls.gets + reference.mesh_gets - ref_gets
  baseline_calls.sets = baseline_calls.sets + reference.mesh_sets - ref_sets
end

for _, source in ipairs(sources) do
  for _, options in ipairs({{}, {generation = true}, {missing = 50}}) do
    cases = cases + 1
    local label = source.name .. " case" .. cases
    local current = instance(source.source, options)
    local reference = instance(reference_source, options)
    local initial = visibility(reference)
    current:start(); reference:start()
    same_components(current, reference, label .. " first startup")
    check(current.mesh_gets <= 102, label .. ": cold component reads exceeded one per flower")
    check(current.mesh_sets == visibility_changes(reference, initial),
          label .. ": startup visibility was not changed-only")
    -- Same season, changed day/night, cross-season, reverse and repeated state.
    for _, state in ipairs({"spring_day", "winter_day", "spring_day", "winter_day",
                            "winter_day", "winter_night", "spring_night", "autumn_day",
                            "autumn_night", "rainy_day", "rainy_night"}) do
      transition(current, reference, state, label .. " " .. state)
    end
    -- Retarget before the one-second tween finishes. Its visibility policy is
    -- discrete even at dt=0; the old owner remains the independent oracle.
    local before = visibility(reference)
    local gets, writes = current.mesh_gets, current.mesh_sets
    current:state("spring_day"); reference:state("spring_day")
    current:update(0); reference:update(0)
    same_components(current, reference, label .. " retarget first frame")
    local expected_writes = visibility_changes(reference, before)
    before = visibility(reference)
    current:state("winter_day"); reference:state("winter_day")
    current:update(1 / 165); reference:update(1 / 165)
    same_components(current, reference, label .. " retarget reverse")
    expected_writes = expected_writes + visibility_changes(reference, before)
    before = visibility(reference)
    current:state("winter_day"); reference:state("winter_day")
    current:update(1 / 600); reference:update(1 / 600)
    same_components(current, reference, label .. " repeat while tweening")
    expected_writes = expected_writes + visibility_changes(reference, before)
    check(current.mesh_gets == gets, label .. ": retarget performed hot JSON reads")
    check(current.mesh_sets - writes == expected_writes,
          label .. ": retarget/repeat rewrote unchanged flower visibility")
    check(current.full_mesh_patches == 0, label .. ": visibility patch contains unrelated fields")
    check(current.created_mesh == 0, label .. ": missing MeshRenderer was created")
    -- A fresh complete Lua owner with generation-bearing handles represents
    -- Stop/recreate at this consumer seam. Actual Engine Play/Stop is separate.
    local again = instance(source.source, {generation = true})
    local old_again = instance(reference_source, {generation = true})
    again:start(); old_again:start()
    same_components(again, old_again, label .. " fresh owner after Stop")
    transition(again, old_again, "spring_day", label .. " fresh owner state")
    check(again.created_mesh == 0, label .. ": fresh owner created an absent mesh")
  end
end

-- A public host refusal must not become a cached success. Execute the complete
-- owner again with the same state while its tween remains at t=0; no private
-- cache inspection is needed. The frozen complete owner supplies target bits.
for _, source in ipairs(sources) do
  cases = cases + 1
  local label = source.name .. " rejected-write case" .. cases
  local current = instance(source.source, {track_mesh = true})
  local reference = instance(reference_source)
  current:start(); reference:start()
  same_components(current, reference, label .. " startup")
  local before = visibility(current)
  local partial = {components = {}}
  for e, kinds in pairs(current.components) do
    partial.components[e] = {}
    for kind, value in pairs(kinds) do partial.components[e][kind] = clone(value) end
  end
  reference:state("spring_day"); reference:update(0)
  local target = visibility(reference)
  local wanted = visibility_changes(reference, before)
  check(wanted > 3, label .. ": complete reference did not supply a multi-write transition")

  local gets, writes, offset = current.mesh_gets, current.mesh_sets, #current.mesh_journal
  current.reject_mesh_at = writes + 3
  local ok, message = pcall(function() current:state("spring_day") end)
  local first, second, refused = current.mesh_journal[offset + 1],
    current.mesh_journal[offset + 2], current.mesh_journal[offset + 3]
  local refused_entity = refused and refused.entity or current.flowers[1]
  check(not ok and tostring(message):find(
          "ES flower visibility write failed: " .. tostring(refused_entity), 1, true),
        label .. ": rejected public write did not raise the owned observable error")
  check(current.mesh_rejections == 1, label .. ": expected exactly one host refusal")
  check(#current.mesh_journal - offset == 3 and first and first.accepted and
        second and second.accepted and refused and not refused.accepted,
        label .. ": owner must stop after two accepted writes and the third refused write")
  check(current.mesh_gets == gets, label .. ": refusal performed hot component reads")
  check(current.components[refused_entity].MeshRenderer.visible == before[refused_entity] and
        target[refused_entity] ~= before[refused_entity],
        label .. ": refused host bit changed or did not require a state change")
  for _, entry in ipairs({first, second}) do
    if entry and entry.accepted then
      partial.components[entry.entity].MeshRenderer.visible = target[entry.entity]
    end
  end
  same_components(current, partial, label .. " refused full host state")

  ok, message = pcall(function() current:state("spring_day"); current:update(0) end)
  -- Both complete owners must consume the second state request: set_state
  -- snapshots its current preset again even when the requested state repeats.
  reference:state("spring_day"); reference:update(0)
  check(ok, label .. ": same-state retry did not recover: " .. tostring(message))
  check(current.mesh_gets == gets and current.full_mesh_patches == 0 and current.created_mesh == 0,
        label .. ": retry read hot JSON, widened the patch or created an absent component")
  check(current.mesh_sets - writes == wanted + 1 and
        #current.mesh_journal - offset == wanted + 1,
        label .. ": retry omitted a changed bit or repeated an already accepted write")
  local observed = {}
  for i = offset + 1, #current.mesh_journal do
    local entry = current.mesh_journal[i]
    local row = observed[entry.entity] or {attempts = 0, accepted = 0}
    row.attempts = row.attempts + 1
    row.accepted = row.accepted + (entry.accepted and 1 or 0)
    observed[entry.entity] = row
  end
  local exact = true
  for _, e in ipairs(current.flowers) do
    local row = observed[e] or {attempts = 0, accepted = 0}
    local changed = before[e] ~= target[e] and 1 or 0
    if row.attempts ~= changed + (e == refused_entity and 1 or 0) or
       row.accepted ~= changed then exact = false end
  end
  check(exact, label .. ": full102-flower attempt/accept counts do not match the reference target")
  check(first and second and observed[first.entity].attempts == 1 and
        observed[first.entity].accepted == 1 and observed[second.entity].attempts == 1 and
        observed[second.entity].accepted == 1,
        label .. ": retry rewrote either earlier accepted flower")
  same_components(current, reference, label .. " recovered full host state")

  local accepted_writes = current.mesh_sets
  current:state("spring_day"); current:update(1); reference:update(1)
  check(current.mesh_sets == accepted_writes and current.mesh_gets == gets and
        current.mesh_rejections == 1,
        label .. ": successful retry did not retain accepted cache through repeat/tween")
  same_components(current, reference, label .. " repeat and completed color tween")
end
print(string.format("ES_VISIBILITY_OLD_OWNER observed transitions=%d flower_comp_get=%d flower_comp_set=%d",
                    baseline_calls.mixes, baseline_calls.gets, baseline_calls.sets))
print(string.format("ES_VISIBILITY_CPU cases=%d assertions=%d failures=%d", cases, assertions, failures))
assert(failures == 0, "ES visibility hot-path / semantic assertions failed")
