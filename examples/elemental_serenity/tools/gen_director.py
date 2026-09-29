#!/usr/bin/env python3
"""
gen_director.py — emit resources/assets/scripts/es_director.lua from
es_palettes.h (the final, night-dimmed numbers the C driver used).

The director is the scene-bound Lua port of es_runtime.c + es_lightning.c:
it runs identically in editor Play and the shipped runtime (both hosts step
the same JceRuntime + Script VM), which is what gives simulation parity.

Run:  python examples/elemental_serenity/tools/gen_director.py
"""
import os, re

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
SRC  = os.path.join(PROJ, "src", "es_palettes.h")
OUT  = os.path.join(PROJ, "resources", "assets", "scripts", "es_director.lua")

# field -> component count, in struct order (matches EsPreset)
FIELDS = [
    ("dome_zenith", 3), ("dome_mid", 3), ("dome_horizon", 3), ("dome_ground", 3),
    ("body_color", 3), ("glow_color", 3), ("disk_on", 1),
    ("fog_color", 3),
    ("ambient", 3), ("ambient_intensity", 1),
    ("key_color", 3), ("key_intensity", 1),
    ("fill_color", 3), ("fill_intensity", 1),
    ("lamp_intensity", 1),
    ("water_shallow", 3), ("water_deep", 3),
    ("shore_ripple", 1), ("ice_ratio", 1),
    ("grass_root", 3), ("grass_tip", 3),
    # rocks_tint STAYS IN THIS LIST and is NOT emitted into the Lua table.
    # FIELDS is the POSITIONAL parse order of es_palettes.h's EsPreset -- the
    # floats are pulled out of the header in this order and sliced by count --
    # so deleting the row here would silently shift leaf_tint and every field
    # after it by three floats.  The value's one consumer is now the C script,
    # which includes es_palettes.h directly.
    ("rocks_tint", 3), ("leaf_tint", 3),
]

src = open(SRC, encoding="utf-8").read()
body = src[src.index("ES_PRESETS[8]"):]
blocks = re.findall(r"\{\s*/\* (\w+) / (\w+) \*/(.*?)\.ground_tex\s*=\s*\"([^\"]+)\"",
                    body, re.S)
assert len(blocks) == 8, f"expected 8 preset blocks, got {len(blocks)}"

lua_presets = []
for season, tod, blob, ground in blocks:
    nums = [float(x) for x in re.findall(r"(-?\d+\.\d+)f", blob)]
    it = iter(nums)
    vals = {}
    for name, n in FIELDS:
        vals[name] = [next(it) for _ in range(n)]
    def v3(k): return "{%.6g, %.6g, %.6g}" % tuple(vals[k])
    def v1(k): return "%.6g" % vals[k][0]
    # The reference's sun/moon GlowColor tints ONLY the disc's halo (which our
    # dome drives from sunColor); feeding it into the dome's horizon glow band
    # washed the whole sky — lavender-orange by day, and 6-9x too bright at
    # night (the wash tracked each season's moonGlowColor exactly). Zero it
    # for BOTH; the disc halo stays via haloStrength.
    vals["glow_color"] = [0.0, 0.0, 0.0]
    lua_presets.append(
        "  { -- %s / %s\n"
        "    dome_zenith=%s, dome_mid=%s, dome_horizon=%s, dome_ground=%s,\n"
        "    body_color=%s, glow_color=%s, disk_on=%s,\n"
        "    fog_color=%s, ambient=%s, ambient_intensity=%s,\n"
        "    key_color=%s, key_intensity=%s, fill_color=%s, fill_intensity=%s,\n"
        "    lamp_intensity=%s,\n"
        "    water_shallow=%s, water_deep=%s, shore_ripple=%s, ice_ratio=%s,\n"
        "    grass_root=%s, grass_tip=%s, leaf_tint=%s,\n"
        "    ground_tex=\"%s\",\n"
        "  }," % (season, tod,
                  v3("dome_zenith"), v3("dome_mid"), v3("dome_horizon"), v3("dome_ground"),
                  v3("body_color"), v3("glow_color"), v1("disk_on"),
                  v3("fog_color"), v3("ambient"), v1("ambient_intensity"),
                  v3("key_color"), v1("key_intensity"), v3("fill_color"), v1("fill_intensity"),
                  v1("lamp_intensity"),
                  v3("water_shallow"), v3("water_deep"), v1("shore_ripple"), v1("ice_ratio"),
                  v3("grass_root"), v3("grass_tip"), v3("leaf_tint"),
                  ground))

PALETTES = "local PRESETS = {\n" + "\n".join(lua_presets) + "\n}\n"

# Foliage (bush/tree/birch) 3-tone ramps per season x tod, extracted from the
# reference SeasonManager (es_bush_palettes.json).
import json as _json
_BUSH = _json.load(open(os.path.join(HERE, "es_bush_palettes.json"), encoding="utf-8"))
# Per-state rim light + flower visibility, straight from the reference presets
# (es_palettes.json lighting.<tod>.rim / grass.<tod>.flowerVisibility).  The
# old rim was a day/night crossfade only — 6 of 8 states carried spring's rim.
_PAL = _json.load(open(os.path.join(HERE, "es_palettes.json"), encoding="utf-8"))
_rim_rows, _flower_rows = [], []
for _sn in ["spring", "winter", "autumn", "rainy"]:
    for _tn in ["day", "night"]:
        _rim = _PAL[_sn]["lighting"][_tn].get("rim") or {}
        _rc  = _rim.get("color", [1.0, 0.843, 0.639])
        _ri  = float(_rim.get("intensity", 0.3))
        if _tn == "night":
            _ri *= 0.22   # same NIGHT_DIM_LIGHT scale the key/fill get
        _rim_rows.append("  {%.4g, %.4g, %.4g, %.4g}," % (_rc[0], _rc[1], _rc[2], _ri))
        _fv = _PAL[_sn]["grass"][_tn].get("flowerVisibility", 1.0)
        _flower_rows.append("  %.2f," % float(_fv))
RIM_TABLE    = chr(10).join(_rim_rows)
FLOWER_TABLE = chr(10).join(_flower_rows)

_SEASONS = ["spring", "winter", "autumn", "rainy"]
_VAR = {"d": "", "t": "tree", "b": "birch"}
def _lv3(v): return "{%.6g, %.6g, %.6g}" % tuple(v)
_rows = []
for _season in _SEASONS:
    for _tod in ["day", "night"]:
        _c = _BUSH[_season][_tod]
        _parts = []
        for _k, _pfx in _VAR.items():
            def _key(base, _p=_pfx):
                return (_p + base[0].upper() + base[1:]) if _p else base
            _parts.append(
                '%s = { shadow=%s, mid=%s, high=%s, mult=%s }' % (
                    _k,
                    _lv3(_c[_key("shadowColor")]), _lv3(_c[_key("midColor")]),
                    _lv3(_c[_key("highlightColor")]), _lv3(_c[_key("colorMultiplier")])))
        _rows.append("  { %s }," % ", ".join(_parts))
PALETTES += "local BUSH_PRESETS = {" + chr(10) + (chr(10)).join(_rows) + chr(10) + "}" + chr(10)


DIRECTOR = r'''
-- ==========================================================================
-- Tiny JSON (decode/encode) — sandbox-safe (string/table/math only).
-- Sufficient for the scene component schemas (objects/arrays/num/str/bool).
-- ==========================================================================
local json = {}
do
  local function skip_ws(s, i)
    local _, j = s:find("^[ \n\r\t]*", i); return j + 1
  end
  local decode_value
  local function decode_string(s, i)
    local out, n = {}, 0
    i = i + 1
    while true do
      local c = s:sub(i, i)
      if c == '"' then return table.concat(out), i + 1 end
      if c == "\\" then
        local e = s:sub(i + 1, i + 1)
        local map = { n = "\n", r = "\r", t = "\t", b = "\b", f = "\f",
                      ['"'] = '"', ["\\"] = "\\", ["/"] = "/" }
        n = n + 1; out[n] = map[e] or e
        i = i + 2
      else
        n = n + 1; out[n] = c
        i = i + 1
      end
    end
  end
  local function decode_number(s, i)
    local j = s:find("[^%d%+%-%.eE]", i) or (#s + 1)
    return tonumber(s:sub(i, j - 1)), j
  end
  decode_value = function(s, i)
    i = skip_ws(s, i)
    local c = s:sub(i, i)
    if c == "{" then
      local t = {}
      i = skip_ws(s, i + 1)
      if s:sub(i, i) == "}" then return t, i + 1 end
      while true do
        local k; k, i = decode_string(s, skip_ws(s, i))
        i = skip_ws(s, i) + 1                       -- ':'
        t[k], i = decode_value(s, i)
        i = skip_ws(s, i)
        local d = s:sub(i, i)
        if d == "}" then return t, i + 1 end
        i = i + 1                                   -- ','
      end
    elseif c == "[" then
      local a = { __array = true }
      i = skip_ws(s, i + 1)
      if s:sub(i, i) == "]" then return a, i + 1 end
      local n = 0
      while true do
        local v; v, i = decode_value(s, i)
        n = n + 1; a[n] = v
        i = skip_ws(s, i)
        local d = s:sub(i, i)
        if d == "]" then return a, i + 1 end
        i = i + 1
      end
    elseif c == '"' then
      return decode_string(s, i)
    elseif s:sub(i, i + 3) == "true" then
      return true, i + 4
    elseif s:sub(i, i + 4) == "false" then
      return false, i + 5
    elseif s:sub(i, i + 3) == "null" then
      return nil, i + 4
    else
      return decode_number(s, i)
    end
  end
  function json.decode(s)
    local v = decode_value(s, 1); return v
  end
  local encode_value
  encode_value = function(v, out)
    local tv = type(v)
    if tv == "number" then
      out[#out + 1] = string.format("%.9g", v)
    elseif tv == "string" then
      out[#out + 1] = '"' .. v:gsub('[\\"]', "\\%0"):gsub("\n", "\\n") .. '"'
    elseif tv == "boolean" then
      out[#out + 1] = v and "true" or "false"
    elseif tv == "table" then
      if v.__array or v[1] ~= nil then
        out[#out + 1] = "["
        for i = 1, #v do
          if i > 1 then out[#out + 1] = "," end
          encode_value(v[i], out)
        end
        out[#out + 1] = "]"
      else
        out[#out + 1] = "{"
        local first = true
        for k, val in pairs(v) do
          if k ~= "__array" then
            if not first then out[#out + 1] = "," end
            first = false
            out[#out + 1] = '"' .. k .. '":'
            encode_value(val, out)
          end
        end
        out[#out + 1] = "}"
      end
    else
      out[#out + 1] = "null"
    end
  end
  function json.encode(v)
    local out = {}; encode_value(v, out); return table.concat(out)
  end
end

-- ==========================================================================
-- Entity helpers
-- ==========================================================================
local function comp(e, ty)          -- decoded component table or nil
  local s = e and jce.comp_get(e, ty)
  return s and json.decode(s) or nil
end
local function comp_write(e, ty, t) -- encode + set
  if e and t then jce.comp_set(e, ty, json.encode(t)) end
end
local function lerp(a, b, k) return a + (b - a) * k end
local function lerp3(a, b, k)
  return { lerp(a[1], b[1], k), lerp(a[2], b[2], k), lerp(a[3], b[3], k) }
end

-- ==========================================================================
-- State
-- ==========================================================================
local SEASON = { spring = 0, winter = 1, autumn = 2, rainy = 3 }
-- Wind-ribbon tint per season (reference SeasonManager windLines.color):
-- spring white, winter alice-blue, autumn warm cream, rainy cool white.
local WIND_COLOR = {
  { 1.0, 1.0, 1.0 },     -- spring 0xffffff
  { 0.94, 0.97, 1.0 },   -- winter 0xf0f8ff
  { 1.0, 0.918, 0.839 }, -- autumn 0xffead6
  { 0.94, 0.957, 1.0 },  -- rainy  0xf0f4ff
}
-- Falling-leaf/petal tint per season (reference SeasonManager fallingLeaves.color).
local RIM = {
{RIM_TABLE}
}
local FLOWER_VIS = {
{FLOWER_TABLE}
}
local LEAF_COLOR = {
  { 1.0,   0.435, 0.051 }, -- spring 0xff6f0d
  { 0.992, 0.584, 0.047 }, -- winter 0xfd950c
  { 1.0,   0.388, 0.278 }, -- autumn 0xff6347
  { 0.0,   0.349, 0.110 }, -- rainy  0x00591c
}
local script = {}
local D = {
  season = 0, tod = 1,          -- default look: spring night
  mix_t = 1.0, from = nil,
  ids = {},                     -- resolved entities
  water = nil,                  -- cached decoded components (get-modify-set)
  sun = nil, fill = nil, lamp = nil, fire = nil,
  grass = {},                   -- { {e=, comp=}, ... }
  flick_t = 0,
  wind_t = 0, wind_st = {},
  bolt_next = 14, bolt_t = -1, flash = 0,
  reveal_t = 0, reveal_done = false,
  audio_cur = { fire = 0, crickets = 0, birds = 0, rain = 0, waves = 0,
                owl_howl = 0, owl_hoot = 0, wolf = 0, thunder_d = 0 },
  music_on = true,
  audio_on = true,              -- master audio toggle (music button = mute-all)
  music_idx = 1, music_t = 0.0,
  -- Orbit camera state (ported from the app's es_camera.c so editor Play and the
  -- shipped runtime share ONE camera path — the director runs in both).
  cam = nil,                    -- resolved BeautyCam entity
  cam_ready = false,
  az = 0, pol = 0, dist = 0,    -- damped current spherical
  az_t = 0, pol_t = 0, dist_t = 0,  -- targets
}

local function preset(season, tod) return PRESETS[season * 2 + tod + 1] end

local function is_raining() return D.season == SEASON.rainy end

-- Snapshot the CURRENT visual state as a pseudo-preset (tween source).
local function snapshot()
  local p, c = preset(D.season, D.tod), {}
  for k, v in pairs(p) do
    if type(v) == "table" then c[k] = { v[1], v[2], v[3] } else c[k] = v end
  end
  return c
end

-- ==========================================================================
-- Apply the (possibly mid-tween) visual state to the engine
-- ==========================================================================
-- Dome disk geometry per time-of-day (reference Skydome uniforms).
-- Day: tiny hot sun disc (uSunSize 0.005 rad -> dot cos(0.005)) with a tight
-- warm halo; position (-0.846,-0.085,-1.0) sits just under world-horizontal,
-- which the pitched-down fov-25 diorama camera frames top-center like the
-- reference.  Night: keep the authored moon (validated in-frame).
-- Sun/moon dirs are the reference Skydome's AUTHORED positions verbatim:
-- the dome is origin-anchored (anchorRadius 150 = their SphereGeometry(150)),
-- so camera parallax happens in-shader; no per-camera precomputation.
local SUN_GEO = {
  -- size < reference cos(0.005 rad): their animeSun's inner/mid glow rings
  -- extend the bright core well past the disc, so our plain disc matches the
  -- READ better a bit larger, with a wider warm halo.
  [0] = { dir = {-0.846, -0.085, -1.0}, size = 0.99997,
          soft = 0.00001, halo_p = 1200.0, halo_s = 0.7,
          rays = 12.0, ray_len = 0.0352, ray_sharp = 8.0, ray_str = 0.55 },
  -- Moon: reference uMoonPosition verbatim; size = cos(uMoonSize 0.0269 rad).
  [1] = { dir = {-0.5, -0.085, -1.0}, size = 0.99964,
          soft = 0.0002, halo_p = 200.0, halo_s = 0.25,
          rays = 0.0, ray_len = 0.0352, ray_sharp = 8.0, ray_str = 0.0 },
}

local function apply_mix(k)
  local a, b = D.from, preset(D.season, D.tod)
  local function m3(f) return lerp3(a[f], b[f], k) end
  local function m1(f) return lerp(a[f], b[f], k) end

  -- Disk gating (reference: sun only spring/autumn day; moon every night):
  -- disk_on=0 kills the disc + halo via black color / zero halo strength.
  local geo = SUN_GEO[D.tod]
  local body = m3("body_color")
  local halo_s = geo.halo_s
  if b.disk_on < 0.5 then
    body = { 0.0, 0.0, 0.0 }
    halo_s = 0.0
  end

  -- Scene rendering settings: dome + fog + ambient (merge patch).
  jce.render_set(json.encode({
    ambient = { color = { __array = true, table.unpack and nil or nil } },
  }) and json.encode({
    -- Slightly lower the ambient fill so the sun's directional shading + SSAO
    -- contact shadows read (the reference gets this contrast from IBL + real
    -- shadows; our flat ambient was washing it out).
    ambient = { color = m3("ambient"), intensity = m1("ambient_intensity") },
    fog = { color = m3("fog_color") },
    environment = { sky = { dome = {
      zenith = m3("dome_zenith"), mid = m3("dome_mid"),
      horizon = m3("dome_horizon"), ground = m3("dome_ground"),
      glow = m3("glow_color"), sunColor = body,
      sunDir = geo.dir, sunSize = geo.size, sunSoftness = geo.soft,
      haloPower = geo.halo_p, haloStrength = halo_s,
      -- reference animeSun petal rays; body=black (disk_on gate) also
      -- blacks the rays, so winter/rainy day need no extra gating
      sunRayCount = geo.rays, sunRayLength = geo.ray_len,
      sunRaySharpness = geo.ray_sharp, sunRayStrength = geo.ray_str,
    } } },
  }))

  -- Key (sun/moon) + fill directional lights.
  if D.sun then
    local c = m3("key_color")
    D.sun.colorR, D.sun.colorG, D.sun.colorB = c[1], c[2], c[3]
    D.sun.intensity = m1("key_intensity")
    comp_write(D.ids.sun, "Light", D.sun)
  end
  if D.fill then
    local c = m3("fill_color")
    D.fill.colorR, D.fill.colorG, D.fill.colorB = c[1], c[2], c[3]
    D.fill.intensity = m1("fill_intensity")
    comp_write(D.ids.fill, "Light", D.fill)
  end
  if D.lamp then
    D.lamp.intensity = m1("lamp_intensity")
    comp_write(D.ids.lamp, "Light", D.lamp)
  end
  -- Rim light: PER-STATE color+intensity from the reference presets (the
  -- old day/night crossfade left 6 of 8 states on spring's rim).
  if D.rim then
    local rr = RIM[D.season * 2 + D.tod + 1]
    D.rim.colorR, D.rim.colorG, D.rim.colorB = rr[1], rr[2], rr[3]
    D.rim.intensity = rr[4]
    comp_write(D.ids.rim, "Light", D.rim)
  end
  -- Flower visibility per state (reference grass.flowerVisibility): a stable
  -- per-flower hash keeps the same SUBSET visible for a given fraction.
  if D.ids.flowers then
    local fv = FLOWER_VIS[D.season * 2 + D.tod + 1]
    for i, fe in ipairs(D.ids.flowers) do
      local mr = comp(fe, "MeshRenderer")
      if mr then
        mr.visible = (((i * 61) % 100) / 100.0) < fv
        comp_write(fe, "MeshRenderer", mr)
      end
    end
  end

  -- Water colors + shore ripple + ice.
  if D.water then
    local sh, dp = m3("water_shallow"), m3("water_deep")
    D.water.shallowR, D.water.shallowG, D.water.shallowB = sh[1], sh[2], sh[3]
    D.water.deepR, D.water.deepG, D.water.deepB = dp[1], dp[2], dp[3]
    D.water.shoreRipple = m1("shore_ripple")
    D.water.iceRatio    = m1("ice_ratio")
    D.water.splashRatio = is_raining() and 0.6 or 0.0   -- sparser splashes (ref reads scattered, not a carpet)
    comp_write(D.ids.pond, "Water", D.water)
  end

  -- Foliage cluster ramps (bush/tree/birch variants).
  local ba = BUSH_PRESETS[D.from_bush_idx or (D.season * 2 + D.tod + 1)]
  local bb = BUSH_PRESETS[D.season * 2 + D.tod + 1]
  if D.from_bush then ba = D.from_bush end
  for key, group in pairs(D.foliage or {}) do
    local va, vb = ba[key], bb[key]
    local sh, mi, hg, mu = lerp3(va.shadow, vb.shadow, k), lerp3(va.mid, vb.mid, k),
                           lerp3(va.high, vb.high, k), lerp3(va.mult, vb.mult, k)
    for _, g in ipairs(group) do
      local fcp = g.comp
      fcp.shadowR, fcp.shadowG, fcp.shadowB = sh[1], sh[2], sh[3]
      fcp.midR, fcp.midG, fcp.midB = mi[1], mi[2], mi[3]
      fcp.highR, fcp.highG, fcp.highB = hg[1], hg[2], hg[3]
      fcp.multR, fcp.multG, fcp.multB = mu[1], mu[2], mu[3]
      comp_write(g.e, "FoliageCluster", fcp)
    end
  end

  -- Grass strips root/tip.
  local gr, gt = m3("grass_root"), m3("grass_tip")
  for i = 1, #D.grass do
    local g = D.grass[i]
    g.comp.rootR, g.comp.rootG, g.comp.rootB = gr[1], gr[2], gr[3]
    g.comp.tipR,  g.comp.tipG,  g.comp.tipB  = gt[1], gt[2], gt[3]
    comp_write(g.e, "GrassField", g.comp)
  end
end

-- One-shot (non-tweened) look pieces: leaf/rock tint, baked ground, weather.
local function retint_leaves()
  local lc = LEAF_COLOR[D.season + 1]
  if D.ids.leaves  then jce.particle_set_color(D.ids.leaves,  lc[1], lc[2], lc[3]) end
  if D.ids.leaves2 then jce.particle_set_color(D.ids.leaves2, lc[1], lc[2], lc[3]) end
end

local function apply_state_discrete()
  retint_leaves()
  local p = preset(D.season, D.tod)

  -- THE ROCKS RETINT USED TO BE HERE AND IT NEVER RAN ONCE.  It iterated
  -- `jce.find_by_prefix("rock")` and gen_scene.py names the entity "Rocks";
  -- the host's prefix match is a strncmp (jce_rt_script.c rt_find_prefix_cb),
  -- so the list was always empty and every state's authored rocks_tint went
  -- nowhere -- visible as identical grey rocks in shot_spring_day.png,
  -- shot_winter_day.png and shot_rainy_day.png, three states apart.  It now
  -- belongs to the C script (src/es_prop_surface.c, EsPropSurface.jcec),
  -- which reads the tint out of src/es_palettes.h -- the same header this
  -- generator parses -- so the value is not transcribed anywhere.  It is no
  -- longer emitted into the preset table below for the same reason: a value
  -- with no consumer is the next reader's trap.

  -- Baked ground albedo swap.
  if D.ids.ground then
    local mr = comp(D.ids.ground, "MeshRenderer")
    if mr then mr.albedoTex = p.ground_tex; comp_write(D.ids.ground, "MeshRenderer", mr) end
  end

  -- Weather emitters.
  local night = (D.tod == 1)
  local raining = is_raining()
  if D.ids.rain      then jce.particle_set_emitting(D.ids.rain,      raining) end
  if D.ids.snow      then jce.particle_set_emitting(D.ids.snow,      D.season == SEASON.winter) end
  for _, fe in ipairs(D.ids.fireflies_ring or {}) do
    jce.particle_set_emitting(fe, night)  -- reference: night-only, ALL seasons (rain included)
  end
  local fire_on = not raining
  if D.ids.flame  then jce.particle_set_emitting(D.ids.flame,  fire_on) end
  if D.ids.embers then jce.particle_set_emitting(D.ids.embers, fire_on) end
  -- reference: rain kills flame+embers but the SMOKE stays (douse wisps),
  -- dropping to the pit (y 1.9 -> 0.6) and graying out while raining.
  if D.ids.smoke then
    jce.particle_set_emitting(D.ids.smoke, true)
    jce.set_position(D.ids.smoke, -5.4, raining and 0.6 or 1.9, -6.9)
    if raining then jce.particle_set_color(D.ids.smoke, 0.45, 0.47, 0.50)
    else            jce.particle_set_color(D.ids.smoke, 0.55, 0.52, 0.48) end
  end
end

-- Per-state looping-audio targets (slewed toward each frame).
-- Reference AmbientSoundManager: only fire / crickets / rain / lake-waves are
-- CONTINUOUS loops; every volume is effectively baseVolume * masterVolume=0.5
-- (crickets/rain 0.56*0.5=0.28, fire ~0.28, waves distance-attenuated ~0.20).
-- Birds/owl/wolf/distant-thunder are SPACED ONE-SHOTS (see update_oneshots) —
-- their loop entities stay parked at 0 (born-gated: zero mixer cost).
-- Master volume — the whole mix was too loud, so a single scalar trims every
-- continuous loop, music track and one-shot at its sink (reference already
-- folded a 0.5 master in; this is on top).  The music/mute button drops it to 0.
local MASTER = 0.55
local function audio_targets()
  if not D.audio_on then
    return { fire = 0, crickets = 0, birds = 0, rain = 0, waves = 0,
             owl_howl = 0, owl_hoot = 0, wolf = 0, thunder_d = 0 }
  end
  local night, raining = (D.tod == 1), is_raining()
  return {
    fire     = (raining and 0.0 or 0.28) * MASTER,
    crickets = ((night and not raining) and 0.28 or 0.0) * MASTER,
    birds    = 0.0,
    rain     = (raining and 0.28 or 0.0) * MASTER,
    waves    = 0.20 * MASTER,
    owl_howl = 0.0, owl_hoot = 0.0, wolf = 0.0, thunder_d = 0.0,
  }
end

-- ==========================================================================
-- Spaced ambience one-shots (reference AmbientSoundManager): each voice fires
-- on an 8..10s random timer WHEN its season/tod/weather condition holds
-- (re-check every 2s otherwise).  Volumes are effective (base * 0.5 master):
-- birds 0.8->0.4 (random clip birds_1..4, day, spring/autumn/winter, no
-- rain), owl-howl 0.4 (spring/autumn/rainy nights), owl-hoot 0.4 (winter
-- nights), wolf 0.56->0.28 (any night), distant thunder 0.72->0.36 (rainy).
-- ==========================================================================
local ONESHOTS = {
  { key = "birds", vol = 0.4,
    cond = function(night, raining) return (not night) and (not raining) end,
    clip = function()
      return "audio/sounds/birds/birds_" .. math.random(1, 4) .. ".mp3"
    end },
  { key = "owl_howl", vol = 0.4,
    cond = function(night, raining)
      return night and (D.season == 0 or D.season == 2 or D.season == 3)
    end,
    clip = function() return "audio/sounds/owl/owl_howling.mp3" end },
  { key = "owl_hoot", vol = 0.4,
    cond = function(night, raining) return night and D.season == 1 end,
    clip = function() return "audio/sounds/owl/owl_hooting.mp3" end },
  { key = "wolf", vol = 0.28,
    cond = function(night, raining) return night end,
    clip = function() return "audio/sounds/wolf/wolf_howling.mp3" end },
  { key = "thunder_d", vol = 0.36,
    cond = function(night, raining) return raining end,
    clip = function() return "audio/sounds/thunder/distant/thunder_distant.mp3" end },
}

local function update_oneshots(dt)
  if not D.audio_on then return end          -- master mute silences one-shots too
  local night, raining = (D.tod == 1), is_raining()
  D.osc = D.osc or {}
  for _, s in ipairs(ONESHOTS) do
    local t = (D.osc[s.key] or (2.0 + math.random() * 6.0)) - dt
    if t <= 0 then
      if s.cond(night, raining) then
        jce.play_sound(s.clip(), s.vol * MASTER)
        t = 8.0 + math.random() * 2.0        -- reference gap: 8000..10000 ms
      else
        t = 2.0                               -- condition false: re-check soon
      end
    end
    D.osc[s.key] = t
  end
end

local function set_state(season, tod)
  if season == D.season and tod == D.tod and D.mix_t >= 1.0 then return end
  D.from  = snapshot()
  D.from_bush = BUSH_PRESETS[D.season * 2 + D.tod + 1]
  D.season, D.tod = season, tod
  D.mix_t = 0.0
  apply_state_discrete()
end

-- ==========================================================================
-- Lightning (rainy): jagged LineRenderer arc + flash + shake + thunder
-- ==========================================================================
local function strike()
  local e = D.ids.bolt
  if not e then return end
  local lr = comp(e, "LineRenderer")
  if not lr then return end
  -- Reference Lightning.class: strike near the pond (bounds ~+/-5.5), a
  -- 15-pt jagged bolt from ground y=0 up to y=15 with small +/-0.5 jitter.
  local x0 = (math.random() - 0.5) * 11.0
  local z0 = (math.random() - 0.5) * 11.0
  local n = 15
  lr.positionCount = n
  local x, z = x0, z0
  for i = 0, n - 1 do
    local t = i / (n - 1)
    lr["px" .. i] = x
    lr["py" .. i] = t * 15.0            -- rises from ground -> 15
    lr["pz" .. i] = z
    x = x + (math.random() - 0.5) * 1.0 -- +/-0.5 lateral jitter per step
    z = z + (math.random() - 0.5) * 1.0
  end
  lr.colorStartA, lr.colorEndA = 1.0, 1.0
  comp_write(e, "LineRenderer", lr)
  D.bolt_t = 0.0
  D.flash  = 1.2
  jce.shake_camera(0.7)
  -- Explosion burst at the strike point (reference createExplosionParticles).
  if D.ids.bolt_burst then
    jce.set_position(D.ids.bolt_burst, x0, 0.0, z0)
    jce.particle_burst(D.ids.bolt_burst, 100)
  end
  if D.audio_on then
    jce.play_sound("audio/sounds/thunder/near/thunder_strike.mp3", 0.36 * MASTER)
  end
end

local function update_lightning(dt)
  if is_raining() then
    D.bolt_next = D.bolt_next - dt
    if D.bolt_next <= 0 then
      strike()
      D.bolt_next = 10 + math.random() * 10
    end
  end
  -- Arc fade (3s) via alpha.
  if D.bolt_t >= 0 then
    D.bolt_t = D.bolt_t + dt
    local a = 1.0 - D.bolt_t / 3.0
    local e = D.ids.bolt
    if e then
      local lr = comp(e, "LineRenderer")
      if lr then
        lr.colorStartA = math.max(a, 0)
        lr.colorEndA   = math.max(a, 0)
        if a <= 0 then lr.positionCount = 0; D.bolt_t = -1 end
        comp_write(e, "LineRenderer", lr)
      end
    end
  end
  -- Exposure flash decay.
  if D.flash > 0 then
    D.flash = math.max(0, D.flash - dt / 0.35 * 1.6)
    jce.render_set(json.encode({ postfx = { exposure = 1.05 + D.flash } }))
  end
end

-- ==========================================================================
-- Reveal dissolve (intro): custom postfx progress 0->1 over 4.5s after 6.6s
-- ==========================================================================
local function update_reveal(dt)
  if D.reveal_done then return end
  D.reveal_t = D.reveal_t + dt
  local t = (D.reveal_t - 1.2) / 4.5
  if t >= 1.0 then
    D.reveal_done = true
    jce.render_set(json.encode({ postfx = { custom = false } }))
    if D.ids.intro then jce.ui_set_text(D.ids.intro, "") end
    return
  end
  if t >= 0 then
    local k = 1 - (1 - t) ^ 3
    jce.render_set(json.encode({ postfx = {
      custom = true, customPostShader = "reveal_dissolve",
      -- nscale 4.0 (reference base freq) + reference cream fill; the shader
      -- aspect-corrects and runs 5 fbm octaves so cells never read as blocks.
      customPostParams = { __array = true,
        k, 4.0, 0, 0,  0.929, 0.910, 0.894, 0 },
    } }))
  end
end

-- ==========================================================================
-- HUD button handlers (UIButton onClickHandler globals; also reachable from
-- C via jce_runtime_dispatch_ui_click — the ES_AUTOSHOOT harness uses them)
-- ==========================================================================
-- HUD click feedback (reference ui_interactions/click.mp3, vol*master=0.25)
local function ui_click()
  jce.play_sound("audio/sounds/ui_interactions/click.mp3", 0.25)
end
function es_btn_spring() ui_click()  set_state(SEASON.spring, D.tod) end
function es_btn_winter() ui_click()  set_state(SEASON.winter, D.tod) end
function es_btn_autumn() ui_click()  set_state(SEASON.autumn, D.tod) end
function es_btn_rain() ui_click()    set_state(SEASON.rainy,  D.tod) end
function es_btn_daynight() ui_click() set_state(D.season, 1 - D.tod) end
function es_btn_strike() ui_click()  strike() end
function es_btn_music()
  ui_click()
  -- The "Music" button is the scene's MASTER mute: the user expects it to
  -- silence the BGM *and* every ambient effect (fire/waves/crickets/rain/
  -- one-shots), not just the music.  Toggle both flags together.
  D.audio_on = not D.audio_on
  D.music_on = D.audio_on
  if not D.audio_on then
    -- Silence everything immediately; the slew/update loops keep it at 0 while
    -- muted and fade back in once unmuted.
    for _, e in ipairs(D.ids.music_tracks or {}) do jce.audio_set_volume(e, 0.0) end
    for _, e in pairs(D.ids.audio or {}) do if e then jce.audio_set_volume(e, 0.0) end end
    for k, _ in pairs(D.audio_cur) do D.audio_cur[k] = 0.0 end
  end
  set_now_playing()
end

-- Skip to the next track immediately (reference MusicControlUI skip button).
function es_btn_skip()
  ui_click()
  local tracks = D.ids.music_tracks or {}
  if #tracks == 0 then return end
  D.music_idx = (D.music_idx % #tracks) + 1
  D.music_t = 0.0
  D.music_on = true
  D.audio_on = true            -- skipping implies audio is on again
  set_now_playing()
end

-- Music playlist: crossfade between the 3 looping tracks, advancing every
-- MUSIC_TRACK_SECS (reference MusicManager rotates tracks with a fade).
local MUSIC_NAMES = { "Forest Dreams", "Morning Petals", "Window Light" }
function set_now_playing()
  if not D.ids.now_playing then return end
  local np = comp(D.ids.now_playing, "UIText")
  if np then
    local prefix = (not D.audio_on) and "Muted: "
                or (D.music_on and "Now Playing: " or "Paused: ")
    np.text = prefix .. (MUSIC_NAMES[D.music_idx] or "")
    comp_write(D.ids.now_playing, "UIText", np)
  end
end
local MUSIC_TRACK_SECS = 110.0
local MUSIC_FADE       = 2.0
local MUSIC_VOL        = 0.25   -- reference musicVolume(0.5) * masterVolume(0.5)
local function update_music(dt)
  local tracks = D.ids.music_tracks or {}
  if #tracks == 0 then return end
  if not D.audio_on or not D.music_on then return end
  D.music_t = (D.music_t or 0.0) + dt
  -- Advance to the next track when the current one's time is up.
  if D.music_t >= MUSIC_TRACK_SECS then
    D.music_t = 0.0
    -- reference MusicManager: random next track, never the same twice
    if #tracks > 1 then
      local nxt = D.music_idx
      while nxt == D.music_idx do nxt = math.random(1, #tracks) end
      D.music_idx = nxt
    end
    set_now_playing()
  end
  -- Crossfade: current track fades 0->MUSIC_VOL over MUSIC_FADE at the start of
  -- its window and MUSIC_VOL->0 over the last MUSIC_FADE; others stay silent.
  local up   = math.min(D.music_t / MUSIC_FADE, 1.0)
  local down = math.min((MUSIC_TRACK_SECS - D.music_t) / MUSIC_FADE, 1.0)
  local vol  = MUSIC_VOL * MASTER * math.min(up, down)
  for i, e in ipairs(tracks) do
    jce.audio_set_volume(e, (i == D.music_idx) and vol or 0.0)
  end
end
-- Direct state setters (autoshoot verification harness).
function es_state_spring_day()  set_state(0, 0) end
function es_state_spring_night() set_state(0, 1) end
function es_state_winter_day()  set_state(1, 0) end
function es_state_winter_night() set_state(1, 1) end
function es_state_autumn_day()  set_state(2, 0) end
function es_state_autumn_night() set_state(2, 1) end
function es_state_rainy_day()   set_state(3, 0) end
function es_state_rainy_night() set_state(3, 1) end

-- ==========================================================================
-- Orbit camera (reference OrbitControls): left-drag orbit, wheel zoom, no pan.
-- Ported verbatim from the app's es_camera.c into the director so EDITOR PLAY
-- and the shipped runtime drive the BeautyCam identically — both feed raw
-- pointer input to the Script VM (jce.is_pointer_down / get_pointer_delta /
-- get_pointer_wheel) and both resolve the highest-priority active VirtualCamera
-- for their view.  Polar clamp [45°, 81.8°], distance [8, 35], damped chase.
-- ==========================================================================
local CAM_POLAR_MIN = 45.0 * 0.01745329
local CAM_POLAR_MAX = 81.8 * 0.01745329
local CAM_DIST_MIN  = 8.0
local CAM_DIST_MAX  = 35.0
local CAM_DAMP      = 10.0     -- target chase rate (1/s)
local CAM_ORBIT     = 0.006    -- radians per pixel dragged
local CAM_ZOOM      = 1.8      -- units per wheel notch

local function camera_init()
  D.cam = jce.find_by_name("BeautyCam")
  if not D.cam then return end
  -- Spherical of the authored beauty pose (18.25, 10.69, 27.32).
  local x, y, z = 18.25, 10.69, 27.32
  D.dist = math.sqrt(x * x + y * y + z * z)
  D.pol  = math.acos(y / D.dist)
  D.az   = math.atan(x / z)         -- x,z both > 0 (1st quadrant): atan2==atan(x/z)
  D.az_t, D.pol_t, D.dist_t = D.az, D.pol, D.dist
  D.cam_ready = true
end

local function update_camera(dt)
  if not D.cam_ready then return end
  -- Left button (platform numbering: 1 = left) drags the orbit; wheel zooms.
  if jce.is_pointer_down(1) then
    local dx, dy = jce.get_pointer_delta()
    D.az_t  = D.az_t  - dx * CAM_ORBIT
    D.pol_t = D.pol_t - dy * CAM_ORBIT
  end
  local wheel = jce.get_pointer_wheel()
  if wheel ~= 0.0 then D.dist_t = D.dist_t - wheel * CAM_ZOOM end

  if D.pol_t  < CAM_POLAR_MIN then D.pol_t  = CAM_POLAR_MIN end
  if D.pol_t  > CAM_POLAR_MAX then D.pol_t  = CAM_POLAR_MAX end
  if D.dist_t < CAM_DIST_MIN  then D.dist_t = CAM_DIST_MIN  end
  if D.dist_t > CAM_DIST_MAX  then D.dist_t = CAM_DIST_MAX  end

  local k = 1.0 - math.exp(-CAM_DAMP * dt)
  D.az   = D.az   + (D.az_t   - D.az)   * k
  D.pol  = D.pol  + (D.pol_t  - D.pol)  * k
  D.dist = D.dist + (D.dist_t - D.dist) * k

  local vc = comp(D.cam, "VirtualCamera")
  if not vc then return end
  local sp, cp = math.sin(D.pol), math.cos(D.pol)
  vc.posX = D.dist * sp * math.sin(D.az)
  vc.posY = D.dist * cp
  vc.posZ = D.dist * sp * math.cos(D.az)
  vc.lookX, vc.lookY, vc.lookZ = 0.0, 0.0, 0.0
  comp_write(D.cam, "VirtualCamera", vc)
end

-- ==========================================================================
-- Lifecycle
-- ==========================================================================
function script.on_start(self)
  local ids = D.ids
  ids.sun    = jce.find_by_name("Sun")
  ids.fill   = jce.find_by_name("Fill")
  ids.rim    = jce.find_by_name("Rim")
  ids.lamp   = jce.find_by_name("TentLamp")
  ids.fire   = jce.find_by_name("CampfireLight")
  ids.pond   = jce.find_by_name("Pond")
  ids.ground = jce.find_by_name("Ground")
  ids.rain      = jce.find_by_name("Rain")
  ids.snow      = jce.find_by_name("Snow")
  ids.fireflies_ring = jce.find_by_prefix("Firefly_")
  ids.flowers = jce.find_by_prefix("flower_")   -- per-state visibility gating
  ids.leaves  = jce.find_by_name("FallingLeaves")
  ids.leaves2 = jce.find_by_name("FallingLeaves2")
  ids.flame     = jce.find_by_name("FireFlame")
  ids.embers    = jce.find_by_name("FireEmbers")
  ids.smoke     = jce.find_by_name("FireSmoke")
  ids.bolt      = jce.find_by_name("LightningArc")
  ids.bolt_burst= jce.find_by_name("LightningBurst")
  ids.intro     = jce.find_by_name("IntroTitle")
  ids.music_tracks = jce.find_by_prefix("AudioMusic")
  ids.now_playing  = jce.find_by_name("NowPlaying")
  ids.audio = {
    fire     = jce.find_by_name("AudioFire"),
    crickets = jce.find_by_name("AudioCrickets"),
    birds    = jce.find_by_name("AudioBirds"),
    rain     = jce.find_by_name("AudioRain"),
    waves    = jce.find_by_name("AudioWaves"),
    owl_howl = jce.find_by_name("AudioOwlHowl"),
    owl_hoot = jce.find_by_name("AudioOwlHoot"),
    wolf     = jce.find_by_name("AudioWolf"),
    thunder_d= jce.find_by_name("AudioThunderD"),
  }
  -- Foliage cluster groups (per-variant retint): cache decoded comps once.
  D.foliage = {}
  for key, prefix in pairs({ d = "BushC", t = "TreeC", b = "BirchC" }) do
    local group = {}
    for _, fe in ipairs(jce.find_by_prefix(prefix)) do
      local fcomp = comp(fe, "FoliageCluster")
      if fcomp then group[#group + 1] = { e = fe, comp = fcomp } end
    end
    D.foliage[key] = group
  end
  ids.wind   = jce.find_by_prefix("WindLine")

  -- Cache mutable components once (get-modify-set each frame).
  D.sun   = comp(ids.sun,  "Light")
  D.fill  = comp(ids.fill, "Light")
  D.rim   = comp(ids.rim, "Light")
  D.lamp  = comp(ids.lamp, "Light")
  D.fire_l = comp(ids.fire, "Light")
  D.water = comp(ids.pond, "Water")
  for _, e in ipairs(jce.find_by_prefix("Grass")) do
    local g = comp(e, "GrassField")
    if g then D.grass[#D.grass + 1] = { e = e, comp = g } end
  end

  D.from = snapshot()
  apply_mix(1.0)
  apply_state_discrete()
  camera_init()      -- resolve BeautyCam + seed the orbit spherical pose
  local nfc = 0
  set_now_playing()  -- seed the music label
  for _, g in pairs(D.foliage) do nfc = nfc + #g end
  jce.log("es_director online: " .. tostring(nfc) .. " foliage clusters, "
          .. tostring(#D.grass) .. " grass strips")

  -- Liveness probe (see src/es_script_probe.c).  The Lua row of the same
  -- five-language table the python / java / cpp / c scripts write:
  -- x = on_start count, y = on_update count, z = named entities resolved.
  -- Lua is the ONLY one of the five that could have reported through jce.log
  -- alone, and it writes the transform anyway — a report where four rows
  -- come from one channel and the fifth from another is a report that can
  -- disagree with itself.
  D.probe  = jce.find_by_name("EsLuaProbe")
  D.starts = (D.starts or 0) + 1
  local nres = 0
  for _, e in pairs(D.ids) do
    if type(e) == "number" then nres = nres + 1
    elseif type(e) == "table" then nres = nres + #e end
  end
  D.resolved = nres
  if D.probe then jce.set_position(D.probe, D.starts, 0, nres) end
end

function script.on_update(self, dt)
  D.frames = (D.frames or 0) + 1
  if D.probe then
    jce.set_position(D.probe, D.starts or 1, D.frames, D.resolved or 0)
  end

  -- ── The five-language probe table, READ OUT THROUGH THE LOG ──────────
  --
  -- src/es_script_probe.c prints this table, and it is compiled into
  -- elemental_serenity.exe.  EDITOR PLAY IS A DIFFERENT PROCESS: it loads
  -- this scene, runs all five scripts, and has no es_script_probe.c in it —
  -- so the one place the five counters exist in an editor Play session is
  -- the probe entities' transforms, and nothing was reading them back.
  --
  -- Lua is the right reader for exactly one reason: it is the only one of the
  -- five backends that reaches JceScriptHost::log AND is guaranteed present
  -- in any host that runs this scene at all (the engine registers it itself).
  -- Reading a value another language WROTE is also the stronger check: this
  -- line cannot report a C++ script as live unless the C++ script really did
  -- write that transform.
  if (D.frames % 600) == 0 then
    for _, row in ipairs({{"lua",    "EsLuaProbe"},
                          {"python", "EsPyProbe"},
                          {"java",   "EsJavaProbe"},
                          {"cpp",    "EsCppProbe"},
                          {"c",      "EsCProbe"}}) do
      local e = jce.find_by_name(row[2])
      if e then
        local x, y, z = jce.get_position(e)
        jce.log(string.format("ES probe %-6s on_start=%d on_update=%d resolved=%d",
                              row[1], math.floor((x or 0) + 0.5),
                              math.floor((y or 0) + 0.5),
                              math.floor((z or 0) + 0.5)))
      else
        jce.log("ES probe " .. row[1] .. ": probe entity " .. row[2]
                .. " NOT IN SCENE -- this run can prove nothing about it")
      end
    end

    -- AND WHETHER THE C++ SCRIPT'S WRITES ACTUALLY LAND.  on_update climbing
    -- proves the callback is delivered; it does not prove set_rotation did
    -- anything.  flower_0's rotation is authored ONCE by gen_scene.py and
    -- touched by nothing but EsFlowerSway::on_update, so two samples that
    -- differ is the sway, and this reader is a different language from the
    -- writer.
    local f0 = jce.find_by_name("flower_0")
    if f0 then
      local rx, ry, rz = jce.get_rotation(f0)
      jce.log(string.format("ES probe cpp    flower_0 rot = %.3f %.3f %.3f",
                            rx or 0, ry or 0, rz or 0))
    else
      jce.log("ES probe cpp    flower_0 NOT IN SCENE")
    end

    -- THE SAME CHECK FOR C, AND IT DOES NOT NEED THE VALUE TO MOVE.
    -- gen_scene.py authors Rocks with baseColor {0.68, 0.57, 0.31} and every
    -- one of the eight ES_PRESETS[].rocks_tint differs from it in every
    -- channel, so a single sample that is NOT 0.68 is proof EsPropSurface's
    -- comp_set landed -- read here by a different language from the writer.
    -- (A steady scene never changes state, so waiting for movement would
    -- prove nothing in an editor Play session that presses no buttons.)
    local rk = jce.find_by_name("Rocks")
    if rk then
      local mr = comp(rk, "MeshRenderer")
      if mr then
        jce.log(string.format(
          "ES probe c      Rocks baseColor = %.4f %.4f %.4f  rough = %.3f"
          .. "   (authored 0.6800 0.5700 0.3100 / 1.000)",
          mr.baseColorR or -1, mr.baseColorG or -1, mr.baseColorB or -1,
          mr.roughness or -1))
      else
        jce.log("ES probe c      Rocks has no MeshRenderer")
      end
    else
      jce.log("ES probe c      Rocks NOT IN SCENE")
    end
  end

  -- First-frame audio priming: the scene's AudioSource components auto-play at
  -- their authored volume the moment the scene loads — LOUD, before the slew
  -- takes over.  Zero every looping voice once here (audio is live by the first
  -- update); the born-silent gate stops them, and the slew/update loops below
  -- fade them back in to the MASTER-adjusted targets.  Without this the first
  -- session blasts until the mute button is toggled (which took this same path).
  if not D.audio_primed then
    D.audio_primed = true
    for _, e in ipairs(D.ids.music_tracks or {}) do
      if e then jce.audio_set_volume(e, 0.0) end
    end
    for _, e in pairs(D.ids.audio or {}) do
      if e then jce.audio_set_volume(e, 0.0) end
    end
  end

  -- Season/day-night tween (1s, power2.out).
  if D.mix_t < 1.0 then
    D.mix_t = math.min(D.mix_t + dt, 1.0)
    local t = D.mix_t
    apply_mix(1 - (1 - t) * (1 - t))
  end

  -- Campfire flicker (skipped while raining — the fire is out).
  if D.fire_l then
    D.flick_t = D.flick_t + dt
    local base = is_raining() and 0.0 or 10.0
    local f = math.sin(D.flick_t * 10.0) * 0.5 + math.sin(D.flick_t * 23.0) * 0.3
            + math.sin(D.flick_t * 41.0) * 0.2
    D.fire_l.intensity = base * (1.0 + 0.4 * f)
    comp_write(D.ids.fire, "Light", D.fire_l)
  end

  -- Wind streaks (reference WindLines.class): each pooled line is a SHORT
  -- 11-unit wavy curve (CatmullRom through alternating ±0.5 y handles) at a
  -- random spot within radius 25 of the origin, y≈3, running along world Z.
  -- It barely moves (1-unit drift over the 4s life); the "sweep" is a reveal
  -- WINDOW (uProgress*3-1 ±1 in ratio space × a centre taper) travelling
  -- along the line, then a 0.3-2s gap before respawning elsewhere.  The old
  -- port swept a segment 36 units across the whole frame at constant alpha —
  -- thick straight lines crossing the tent.
  for i, e in ipairs(D.ids.wind or {}) do
    local st = D.wind_st[i]
    if not st then
      st = { t = 4.0 + i * 0.8, gap = 0.6, x = 0, y = 3, z = 0, seed = i * 2.1 }
      D.wind_st[i] = st
    end
    st.t = st.t + dt
    local lr = comp(e, "LineRenderer")
    if lr then
      if st.t >= 4.0 + st.gap then
        st.t    = 0.0
        st.gap  = 0.2 + math.random() * 0.8   -- reference effective refill
        st.x    = (math.random() - 0.5) * 25.0
        st.z    = (math.random() - 0.5) * 25.0
        st.y    = 3.0                          -- reference fixed height
        st.seed = math.random() * 6.28
      end
      local p = st.t / 4.0
      if p >= 1.0 then
        if lr.positionCount ~= 0 then
          lr.positionCount = 0
          comp_write(e, "LineRenderer", lr)
        end
      else
        local n = 14
        lr.positionCount = n
        local drift = p          -- reference translation = 1 along -Z
        for j = 0, n - 1 do
          local s = j / (n - 1)
          lr["px" .. j] = st.x
          -- reference CatmullRom through alternating +/-0.5 handles ~=
          -- y = -0.5*cos(3*pi*s) (1.5 periods, full amplitude at the ends)
          lr["py" .. j] = st.y - 0.5 * math.cos(3.0 * 3.14159 * s)
          lr["pz" .. j] = st.z + (s - 0.5) * 11.0 - drift
        end
        -- Reveal window alpha at the head/tail sample points (our line lerps
        -- alpha start->end; the reference computes it per vertex).
        local remap = p * 3.0 - 1.0
        local function wnd(r)
          local v = 1.0 - math.abs(r - remap)
          if v < 0.0 then v = 0.0 elseif v > 1.0 then v = 1.0 end
          v = v * v * (3.0 - 2.0 * v)
          local base = 1.0 - math.abs(r - 0.5) * 2.0
          if base < 0.0 then base = 0.0 end
          base = base * base * (3.0 - 2.0 * base)
          return v * base
        end
        lr.colorStartA = wnd(0.2)
        lr.colorEndA   = wnd(0.8)
        -- Season-tinted (reference SeasonManager windLines.color).
        local wc = WIND_COLOR[D.season + 1]
        lr.colorStartR, lr.colorStartG, lr.colorStartB = wc[1], wc[2], wc[3]
        lr.colorEndR,   lr.colorEndG,   lr.colorEndB   = wc[1], wc[2], wc[3]
        comp_write(e, "LineRenderer", lr)
      end
    end
  end

  update_camera(dt)  -- orbit/zoom (editor Play + runtime share this path)
  update_lightning(dt)
  update_reveal(dt)
  update_music(dt)   -- P0: without this the crossfade never advances → music silent
  update_oneshots(dt) -- spaced birds/owl/wolf/distant-thunder (reference model)

  -- Looping-audio slew (0.8/s toward per-state targets).
  local tgt = audio_targets()
  for k, target in pairs(tgt) do
    local cur = D.audio_cur[k]
    if cur ~= target then
      local step = 0.8 * dt
      if cur < target then cur = math.min(cur + step, target)
      else cur = math.max(cur - step, target) end
      D.audio_cur[k] = cur
      local e = D.ids.audio and D.ids.audio[k]
      if e then jce.audio_set_volume(e, cur) end
    end
  end
end

return script
'''

# strip the accidental no-op line from apply_mix head (kept template simple)
DIRECTOR = DIRECTOR.replace(
    "  jce.render_set(json.encode({\n"
    "    ambient = { color = { __array = true, table.unpack and nil or nil } },\n"
    "  }) and json.encode({",
    "  jce.render_set(json.encode({")

lua = ("-- GENERATED by build/gen_director.py — do not edit by hand.\n"
       "-- Scene-bound director: seasons x day/night, weather, lightning,\n"
       "-- wind, audio, reveal + HUD handlers. Runs in editor Play AND the\n"
       "-- shipped runtime (same JceRuntime + Script VM).\n\n"
       + PALETTES + DIRECTOR)

# Interpolate the per-state rim/flower tables (plain-text template).
lua = lua.replace("{RIM_TABLE}", RIM_TABLE).replace("{FLOWER_TABLE}", FLOWER_TABLE)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write(lua)
print(f"wrote {OUT}: {len(lua)} bytes, {len(lua_presets)} presets")
