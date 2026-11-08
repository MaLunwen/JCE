-- Execute the complete authored/portable owners with counted public host ports.
-- The ports model flat LineRenderer JSON merge and float storage; no wind math
-- is reproduced here. This is a CPU consumer contract, not an SDK/GPU witness.
local root = assert(arg[1], "expected resources/assets/scripts directory")
local mode = arg[2] or "normal"
assert(mode == "normal" or mode == "exact" or mode == "old-json" or
       mode == "unchecked-return" or mode == "shifted-points" or mode == "missing-seed")
local function read(name)
  local f = assert(io.open(root .. "/" .. name, "rb"))
  local text = f:read("a"); f:close(); return text
end
local portable = read("es_director_portable.lua")
local legacy = read("es_director.lua")
if mode == "unchecked-return" then
  local guard = 'if jce.line_set_points(e, xyz) ~= n then'
  local at = assert(portable:find(guard, 1, true), "unique batch guard missing")
  assert(not portable:find(guard, at + #guard, true), "batch guard duplicated")
  portable = portable:sub(1, at - 1) ..
    'if jce.line_set_points(e, xyz) == -1 then' .. portable:sub(at + #guard)
elseif mode == "shifted-points" then
  local wire = 'xyz[k + 2] = st.z + (s - 0.5) * 11.0 - drift'
  local at = assert(portable:find(wire, 1, true), "unique point wire missing")
  assert(not portable:find(wire, at + #wire, true), "point wire duplicated")
  portable = portable:sub(1, at - 1) .. wire .. ' + 0.001' .. portable:sub(at + #wire)
elseif mode == "missing-seed" then
  local wire = '  math.randomseed(seed_a, seed_b)'
  local at = assert(portable:find(wire, 1, true), "unique seed call missing")
  assert(not portable:find(wire, at + #wire, true), "seed call duplicated")
  portable = portable:sub(1, at - 1) .. portable:sub(at + #wire)
end
local function f32(n) return (string.unpack("f", string.pack("f", n))) end
local function encode_flat(t)
  local fields = {}
  for k, v in pairs(t) do
    local value = type(v) == "number" and string.format("%.17g", v)
      or type(v) == "boolean" and tostring(v) or string.format("%q", v)
    fields[#fields + 1] = string.format('"%s":%s', k, value)
  end
  table.sort(fields); return "{" .. table.concat(fields, ",") .. "}"
end
local function decode_flat(text)
  local t = {}
  for k, value in text:gmatch('"([^"]+)"%s*:%s*([^,}]+)') do
    if value == "true" then t[k] = true
    elseif value == "false" then t[k] = false
    elseif value:sub(1, 1) == '"' then t[k] = value:sub(2, -2)
    else t[k] = assert(tonumber(value), "invalid numeric port payload") end
  end
  return t
end
local function instance(source, options)
  options = options or {}
  local p = {lines = {}, gets = 0, patches = 0, point_json = 0,
             batches = 0, hides = 0, randoms = 0, seed_calls = 0, seed_logs = 0,
             buffers = {}, last_patch = {}, formatted = {}}
  for e = 1, 3 do
    p.lines[e] = {type = "LineRenderer", materialPath = "wind_material",
      positionCount = 0, widthStart = f32(.09), widthEnd = f32(.09),
      colorStartR = 1, colorStartG = 1, colorStartB = 1, colorStartA = 0,
      colorEndR = 1, colorEndG = 1, colorEndB = 1, colorEndA = 0,
      useWorldSpace = true, loop = false}
  end
  local host = {
    find_by_name = function() return nil end,
    find_by_prefix = function(prefix) return prefix == "WindLine" and {1, 2, 3} or {} end,
    log = function(text)
      if text:find("ES_CAPTURE_SEED ", 1, true) then p.seed_logs = p.seed_logs + 1 end
    end,
    get_param = function(e, name)
      assert(e == 186, "Script parameter read used the wrong owner")
      local param = options.params and options.params[name]
      if param then return param.kind, param.number, param.entity end
    end,
    render_set = function() return true end,
    play_sound = function() end,
    comp_get = function(e, kind)
      assert(kind == "LineRenderer")
      p.gets = p.gets + 1
      if options.missing then return nil end
      return encode_flat(assert(p.lines[e]))
    end,
    comp_set = function(e, kind, text)
      assert(kind == "LineRenderer")
      p.patches = p.patches + 1
      p.last_patch[e] = text
      local patch, line = decode_flat(text), assert(p.lines[e])
      if patch.px0 ~= nil then p.point_json = p.point_json + 1 end
      if patch.positionCount == 0 then
        assert(line.positionCount ~= 0, "hidden count written repeatedly")
        p.hides = p.hides + 1
      end
      if options.patch_fail or (options.hide_fail and patch.positionCount == 0) then return false end
      for k, v in pairs(patch) do line[k] = type(v) == "number" and f32(v) or v end
      -- Scene JSON serializes only positionCount points; parse clears the rest.
      for j = line.positionCount, 13 do
        line["px" .. j], line["py" .. j], line["pz" .. j] = nil, nil, nil
      end
      return true
    end,
    line_set_points = function(e, xyz)
      assert(#xyz == 42, "expected one 14-point flat table")
      if p.buffers[e] then assert(p.buffers[e] == xyz, "point buffer reallocated")
      else p.buffers[e] = xyz end
      for i = 1, 42 do assert(type(xyz[i]) == "number") end
      p.batches = p.batches + 1
      if options.invalid then return options.result end
      local line = assert(p.lines[e]); line.positionCount = 14
      for j = 0, 13 do
        local k = j * 3 + 1
        line["px" .. j], line["py" .. j], line["pz" .. j] =
          f32(xyz[k]), f32(xyz[k + 1]), f32(xyz[k + 2])
      end
      return 14
    end,
  }
  local random_math = {}; for k, v in pairs(math) do random_math[k] = v end
  random_math.random = function(...)
    p.randoms = p.randoms + 1; return math.random(...)
  end
  random_math.randomseed = function(a, b)
    assert(p.randoms == 0, "capture seed applied after first random use")
    p.seed_calls = p.seed_calls + 1; p.seed_a, p.seed_b = a, b
    return math.randomseed(a, b)
  end
  local counted_string = {}; for k, v in pairs(string) do counted_string[k] = v end
  counted_string.format = function(fmt, ...)
    local result = string.format(fmt, ...)
    if fmt == "%.9g" then p.formatted[result] = (...) end
    return result
  end
  local env = setmetatable({jce = host, math = random_math, string = counted_string}, {__index = _G})
  p.script = assert(load(source, "@actual-director", "t", env))()
  p.self = setmetatable({entity = 186}, {__index = p.script})
  p.env = env
  function p:start() self.script.on_start(self.self) end
  function p:update(dt) self.formatted = {}; self.script.on_update(self.self, dt) end
  return p
end
local states = {"spring_day", "spring_night", "winter_day", "winter_night",
                "autumn_day", "autumn_night", "rainy_day", "rainy_night"}
local function signature(p)
  local fields = {string.pack("I4", p.randoms)}
  for e = 1, 3 do
    local line = p.lines[e]
    assert(line.materialPath == "wind_material" and line.useWorldSpace and not line.loop)
    assert(line.widthStart == f32(.09) and line.widthEnd == f32(.09))
    fields[#fields + 1] = string.pack("I4", line.positionCount)
    for _, key in ipairs({"colorStartR", "colorStartG", "colorStartB", "colorStartA",
                          "colorEndR", "colorEndG", "colorEndB", "colorEndA"}) do
      fields[#fields + 1] = string.pack("f", line[key])
    end
    for j = 0, line.positionCount - 1 do
      for _, axis in ipairs({"x", "y", "z"}) do
        local value = line["p" .. axis .. j]
        assert(value == value and math.abs(value) < math.huge)
        fields[#fields + 1] = string.pack("f", value)
      end
    end
  end
  return table.concat(fields)
end
local changed_coordinates, max_ulp = 0, 0
local function rank(bits)
  -- Finite IEEE float32 ordering, with both signed zeros at the same rank.
  return bits >= 0x80000000 and 0x80000000 - (bits - 0x80000000) or 0x80000000 + bits
end
local function compare(actual, expected, frame, new, old)
  local at = 1
  local function exact(bytes, label)
    assert(actual:sub(at, at + bytes - 1) == expected:sub(at, at + bytes - 1),
           label .. " mismatch at frame " .. frame)
    at = at + bytes
  end
  exact(4, "random consumption")
  for e = 1, 3 do
    local count = string.unpack("I4", actual, at)
    exact(4, "position count"); exact(32, "style float32")
    for j = 0, count - 1 do
      for axis, label in ipairs({"x", "y", "z"}) do
        local a, b = string.unpack("I4", actual, at), string.unpack("I4", expected, at)
        local ulp = math.abs(rank(a) - rank(b))
        if ulp > 0 then
          changed_coordinates = changed_coordinates + 1; max_ulp = math.max(max_ulp, ulp)
        end
        if ulp > 1 or (mode == "exact" and ulp > 0) then
          local value = new.buffers[e][j * 3 + axis]
          local decimal = old and old.last_patch[e]:match('"p' .. label .. j .. '":([^,}]+)')
          local input = decimal and old.formatted[decimal]
          error(string.format("float32 coordinate mismatch frame %d WindLine%d p%s%d: " ..
            "new_double=%.17g old_input_double=%s legacy_decimal=%s " ..
            "new_f32=%.17g old_f32=%.17g ulp=%d", frame, e - 1, label, j,
            value, input and string.format("%.17g", input) or "not-retained",
            decimal or "not-retained", string.unpack("f", actual, at),
            string.unpack("f", expected, at), ulp))
        end
        at = at + 4
      end
    end
  end
  assert(at == #actual + 1 and #actual == #expected, "unconsumed trajectory fields")
end
local function exercise(source, dts, frames, reference, old)
  math.randomseed(317, 91)
  local p = instance(source); p:start()
  assert(p.seed_calls == 0 and p.seed_logs == 0, "normal startup unexpectedly reseeded Lua")
  local samples = {}
  for frame = 1, frames do
    if (frame - 1) % 200 == 0 then
      p.env["es_state_" .. states[(math.floor((frame - 1) / 200) % 8) + 1]]()
    end
    p:update(dts[(frame - 1) % #dts + 1])
    local actual = signature(p)
    if reference then
      compare(actual, reference[frame], frame, p, old)
    else samples[frame] = actual end
  end
  return p, samples
end
local total_batches, total_hides = 0, 0
if mode == "exact" then
  local old, expected = exercise(legacy, {1 / 60}, 3)
  exercise(portable, {1 / 60}, 3, expected, old)
  error("expected retained legacy decimal rounding difference")
end
for _, schedule in ipairs({
  {frames = 1800, dts = {1 / 60}},
  {frames = 2400, dts = {1 / 165, 1 / 400, 1 / 600, 0, .034, .2, .5, 1, 4, .6}},
  {frames = 1800, dts = {0, 4, 0, .2, .8, 1e-6, 3.999999, .6, .01}},
}) do
  local old, expected = exercise(legacy, schedule.dts, schedule.frames)
  assert(old.gets == schedule.frames * 3 and old.batches == 0 and old.point_json > 0)
  local new = exercise(mode == "old-json" and legacy or portable,
                       schedule.dts, schedule.frames, expected)
  assert(new.gets == 3, "wind still reads component JSON in the hot path")
  assert(new.point_json == 0, "wind still serializes points as JSON")
  assert(new.batches > 0 and new.hides == old.hides, "active/hidden transitions differ")
  total_batches, total_hides = total_batches + new.batches, total_hides + new.hides
end
-- A rejected/partial/unavailable batch must be observable, without JSON fallback.
for _, result in ipairs({0, 13, 15, false, "14"}) do
  local p = instance(portable, {invalid = true, result = result}); p:start()
  local ok, err = pcall(function() p:update(1 / 60) end)
  assert(not ok and tostring(err):find("ES wind point write failed", 1, true),
         "invalid batch return was ignored: " .. tostring(result))
  assert(p.batches == 1 and p.point_json == 0)
end
local unavailable = instance(portable, {invalid = true}); unavailable:start()
local ok, err = pcall(function() unavailable:update(1 / 60) end)
assert(not ok and tostring(err):find("ES wind point write failed", 1, true))
local patch_fail = instance(portable, {patch_fail = true}); patch_fail:start()
ok, err = pcall(function() patch_fail:update(1 / 60) end)
assert(not ok and tostring(err):find("ES wind style failed", 1, true) and patch_fail.batches == 0)
local missing = instance(portable, {missing = true})
ok, err = pcall(function() missing:start() end)
assert(not ok and tostring(err):find("ES wind missing LineRenderer", 1, true))
local hide_fail = instance(portable, {hide_fail = true}); hide_fail:start(); hide_fail:update(1 / 60)
ok, err = pcall(function() hide_fail:update(4) end)
assert(not ok and tostring(err):find("ES wind hide failed", 1, true) and hide_fail.batches == 3)
assert(changed_coordinates > 0 and max_ulp == 1, "legacy rounding boundary not exercised")
assert(total_hides > 0, "hidden boundary not exercised")
-- Actual Director replay with authored public Script params. No random equations
-- are copied: compare every captured point/style/count from separate owner runs.
local function seed_params(a, b)
  return {capture_seed_a = {kind = 0, number = a}, capture_seed_b = {kind = 0, number = b}}
end
local function capture(a, b, prior)
  math.randomseed(prior, 9)
  local p = instance(portable, {params = seed_params(a, b)}); p:start()
  local samples = {}
  for frame = 1, 800 do
    if (frame - 1) % 100 == 0 then p.env["es_state_" .. states[(frame - 1) // 100 + 1]]() end
    p:update(frame % 3 == 0 and .034 or 1 / 165)
    samples[#samples + 1] = signature(p)
  end
  assert(p.seed_calls == 1 and p.seed_logs == 1, "capture seed was not applied exactly once")
  assert(p.seed_a == a and p.seed_b == b, "capture did not consume both seed words")
  return table.concat(samples)
end
local first = capture(317, 91, 1)
assert(first == capture(317, 91, 999), "capture seed failed to replay the actual Director")
assert(first ~= capture(317, 92, 1), "second capture seed word had no effect")
for _, pair in ipairs({{0, 0}, {16777215, 16777215}}) do
  local p = instance(portable, {params = seed_params(pair[1], pair[2])}); p:start()
  assert(p.seed_calls == 1 and p.seed_a == pair[1] and p.seed_b == pair[2])
end
local invalid_params = {seed_params(nil, 1), seed_params(1, nil),
  seed_params(-1, 0), seed_params(0, 16777216), seed_params(0.5, 1),
  seed_params("1", 2), seed_params(false, 2), seed_params(0 / 0, 2), seed_params(math.huge, 2)}
local wrong_kind = seed_params(1, 2); wrong_kind.capture_seed_a.kind = 1
invalid_params[#invalid_params + 1] = wrong_kind
invalid_params[#invalid_params + 1] = {capture_seed_a = {kind = 0, number = 1}}
for _, params in ipairs(invalid_params) do
  local p = instance(portable, {params = params})
  local accepted, message = pcall(function() p:start() end)
  assert(not accepted and tostring(message):find("ES capture seeds require two numeric integers", 1, true),
         "invalid capture seed was accepted")
  assert(p.randoms == 0 and p.seed_calls == 0 and p.gets == 0 and p.patches == 0)
end
print("ES_CAPTURE_SEED_CPU PASS same-seed 800-frame replay, changed second word, normal absence, bounds and rejected params")
print(string.format("ES_WIND_CPU PASS 6000 whole-owner frames, " ..
  "exact random/count/style and <=1 coordinate float32 ULP; %d adjacent differences; " ..
  "%d reused batches, %d hidden boundaries, rejected host returns",
  changed_coordinates, total_batches, total_hides))
