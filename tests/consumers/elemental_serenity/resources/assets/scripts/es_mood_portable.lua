-- Shared serialized Mood request/plan/apply/ack consumer for every entry.
-- This is authored preset selection, not a claim of generated content.
local script = {}
local M = { seq = 0, current = 1, elapsed = 0, pending = false }
local STATES = {"spring_day", "spring_night", "winter_day", "winter_night",
                "autumn_day", "autumn_night", "rainy_day", "rainy_night"}
local MAX_SEQ = 16777215 -- exact integer in serialized float positions
local function pos(e)
  if not e then return nil end
  return jce.get_position(e)
end
local function status(s)
  if M.label then jce.ui_set_text(M.label, s) end
  if M.last_status ~= s then jce.log("ES_MOOD " .. s); M.last_status = s end
end
local function fail(reason)
  M.pending = false
  if M.ack then jce.set_position(M.ack, M.seq, M.current, -1) end
  status("FAILED | " .. (M.provider or "unavailable") .. " | " .. reason)
end
local function apply(index, advance)
  if type(index) ~= "number" or index ~= math.floor(index) or index > 7 or
     index < (advance and -1 or 0) then
    fail("invalid plan state"); return
  end
  M.desired = index
  -- Public send_message has no success result. Only the director's ack after
  -- actual set_state means APPLIED; absent receiver/method times out.
  -- Next is resolved by the same owner that applies the top HUD buttons.
  jce.send_message(M.director, "es_mood_apply", M.seq,
                   index == -1 and "next" or STATES[index + 1])
end
local function request(index)
  if not M.ready then status("UNAVAILABLE | missing serialized Mood nodes"); return end
  if M.pending then return end
  if M.seq >= MAX_SEQ then status("FAILED | request sequence exhausted"); return end
  M.seq = M.seq + 1
  M.pending, M.elapsed, M.desired = true, 0, nil
  local available = pos(M.native)
  local use_native = index == -1 and available == 1
  M.provider = use_native and "native director (offline bootstrap)" or
    ((available == 1) and "authored preset" or "portable preset (native UNAVAILABLE)")
  jce.set_position(M.request, M.seq, index, use_native and 1 or 0)
  jce.set_position(M.ack, 0, M.current, 0)
  status("REQUESTED | " .. M.provider .. " | " .. tostring(M.seq))
  if not use_native then
    apply(index, index == -1)
  end
end
function es_mood_next() request(-1) end
function es_mood_spring_day() request(0) end
function es_mood_spring_night() request(1) end
function es_mood_winter_day() request(2) end
function es_mood_winter_night() request(3) end
function es_mood_autumn_day() request(4) end
function es_mood_autumn_night() request(5) end
function es_mood_rainy_day() request(6) end
function es_mood_rainy_night() request(7) end
function script.on_start(self)
  M.request = jce.find_by_name("MoodRequest")
  M.plan = jce.find_by_name("MoodPlan")
  M.ack = jce.find_by_name("MoodAck")
  M.native = jce.find_by_name("MoodProvider")
  M.label = jce.find_by_name("MoodStatus")
  M.director = jce.find_by_name("Director")
  M.ready = M.request and M.plan and M.ack and M.native and M.label and M.director
  if not M.ready then status("UNAVAILABLE | missing serialized Mood nodes"); return end
  jce.set_position(M.request, 0, -1, 0)
  jce.set_position(M.plan, 0, 0, 0)
  jce.set_position(M.ack, 0, M.current, 0)
  status("READY | portable preset; native availability checked on request")
end
function script.on_update(self, dt)
  if not M.pending then
    if M.ready and M.seq == 0 then
      local available = pos(M.native)
      if M.provider_seen ~= available then
        M.provider_seen = available
        status(available == 1 and "READY | native offline plans / authored presets" or
               "READY | portable preset (native UNAVAILABLE)")
      end
    end
    return
  end
  M.elapsed = M.elapsed + math.max(0, dt)
  local seq, state, result = pos(M.ack)
  if seq == M.seq and result == 1 then
    if type(state) ~= "number" or state ~= math.floor(state) or state < 0 or state > 7 or
       (M.desired ~= -1 and state ~= M.desired) then
      fail("mismatched apply acknowledgement"); return
    end
    M.pending, M.current = false, state
    status("APPLIED | " .. M.provider .. " | " .. STATES[state + 1])
    return
  elseif seq == M.seq and result < 0 then
    fail("director rejected apply"); return
  end
  if M.desired == nil then
    local plan_seq, plan_state, plan_result = pos(M.plan)
    if plan_seq == M.seq then
      if plan_result == 1 then apply(plan_state)
      elseif plan_result < 0 then fail("native planning error " .. tostring(plan_state)) end
    end
  end
  if M.pending and M.elapsed >= 5 then fail("plan/apply acknowledgement timeout") end
end
return script
