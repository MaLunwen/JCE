-- Scientific Kerr black-hole laboratory controller.
--
-- The engine owns only a generic fullscreen-effect ABI.  This project script
-- validates CK's versioned scientific inputs, packs the sixteen project vec4
-- rows, maps the physical observer into scene units of M, and drives the same
-- scene components in editor Play and the shipped runtime.

local CONFIG_PATH = "black_hole/kerr_lab.json"
local REFERENCE_PATH = "black_hole/reference_summary.json"
local EFFECT_TEXTURES = {
  {
    path = "textures/black_hole/starfield_reference.jceasset",
    addressU = 0, addressV = 0,
    filterMin = 1, filterMag = 1, filterMip = 1,
  },
  {
    path = "textures/black_hole/disk_flux_lut.jceasset",
    addressU = 0, addressV = 0,
    filterMin = 1, filterMag = 1, filterMip = 0,
  },
  {
    path = "textures/black_hole/blackbody_rgb_lut.jceasset",
    addressU = 0, addressV = 0,
    filterMin = 1, filterMag = 1, filterMip = 0,
  },
  {
    path = "textures/black_hole/starfield_prefiltered.jceasset",
    addressU = 0, addressV = 0,
    filterMin = 1, filterMag = 1, filterMip = 0,
  },
}

local QUALITY_STEPS = {128, 192, 320, 640, 768, 1024}
local DIAGNOSTICS = {
  "Radiance", "Ray outcome", "Redshift g",
  "Image order", "Accepted steps", "Log residual",
}
local SPECTRUM_MODES = {bolometric_false_color = 0}

local PRESETS = {
  {name = "Schwarzschild baseline", spin = 0.0, inclination = 60.0, fov = 60.0},
  {name = "Face-on Kerr", spin = 0.8, inclination = 0.0, fov = 52.0},
  {name = "Scientific default", spin = 0.8, inclination = 60.0, fov = 60.0},
  {name = "Disk overview", spin = 0.8, inclination = 60.0, fov = 70.0},
  {name = "Near-edge-on", spin = 0.8, inclination = 85.0, fov = 52.0},
  {name = "Counter-aligned hole", spin = -0.8, inclination = 60.0, fov = 60.0},
  {name = "Near-extremal test", spin = 0.998, inclination = 60.0, fov = 52.0},
}

local D = {
  ids = {},
  active = false,
  config = nil,
  reference = nil,
  preset = 3,
  quality = 3,
  diagnostic = 0,
  paused = false,
  overlay = true,
  sample_counter = 0,
  sample_phase = -1.0,
  history_invalid_frames = 1,
  effect_dirty = false,
  camera_dirty = false,
  hud_dirty = false,
}

local json = {}

local function json_escape(value)
  return value:gsub("\\", "\\\\")
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
      error("non-finite JSON number")
    end
    return string.format("%.9g", value)
  end
  if kind == "string" then return '"' .. json_escape(value) .. '"' end
  if kind ~= "table" then error("unsupported JSON value") end

  local count = 0
  local is_array = true
  for key, _ in pairs(value) do
    count = count + 1
    if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then
      is_array = false
    end
  end
  if is_array then
    for i = 1, count do
      if value[i] == nil then is_array = false; break end
    end
  end

  local out = {}
  if is_array then
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

local function finite(value)
  return type(value) == "number" and value == value and
         value ~= math.huge and value ~= -math.huge
end

local function clamp(value, lo, hi)
  if value < lo then return lo end
  if value > hi then return hi end
  return value
end

local function round(value)
  if value >= 0.0 then return math.floor(value + 0.5) end
  return math.ceil(value - 0.5)
end

local function contract_is(value, name)
  local c = type(value) == "table" and value.contract or nil
  return type(c) == "table" and c.name == name and
         c.major == 1 and finite(c.minor) and c.minor >= 0
end

local function require_table(parent, key)
  local value = type(parent) == "table" and parent[key] or nil
  if type(value) ~= "table" then return nil, key end
  return value
end

local function hash_from_config(config)
  if type(config.identity) == "table" and
     type(config.identity.config_hash) == "string" then
    return config.identity.config_hash
  end
  if type(config.config_hash) == "string" then return config.config_hash end
  return nil
end

local function revision_from_config(config)
  if type(config.identity) == "table" and finite(config.identity.revision) then
    return config.identity.revision
  end
  if finite(config.config_revision) then return config.config_revision end
  return nil
end

local function optional_number(parent, key, default)
  local value = parent[key]
  if value == nil then return default end
  return value
end

local function kerr_horizon(spin)
  return 1.0 + math.sqrt(math.max(0.0, 1.0 - spin * spin))
end

local function cube_root(value)
  if value < 0.0 then return -((-value) ^ (1.0 / 3.0)) end
  return value ^ (1.0 / 3.0)
end

local function kerr_isco(spin)
  if math.abs(spin) < 1e-12 then return 6.0 end
  local a2 = spin * spin
  local z1 = 1.0 + cube_root(1.0 - a2) *
    (cube_root(1.0 + spin) + cube_root(1.0 - spin))
  local z2 = math.sqrt(3.0 * a2 + z1 * z1)
  local branch = math.sqrt(math.max(0.0,
    (3.0 - z1) * (3.0 + z1 + 2.0 * z2)))
  return 3.0 + z2 - (spin > 0.0 and branch or -branch)
end

local function quality_index(steps)
  for i, tier in ipairs(QUALITY_STEPS) do
    if steps == tier then return i end
  end
  return nil
end

local function validate_config(config)
  if not contract_is(config, "ck.kerr-lab") then
    return nil, "CONFIG_SCHEMA_MISMATCH"
  end
  local metric = require_table(config, "metric")
  local observer = require_table(config, "observer")
  local disk = require_table(config, "disk")
  local integration = require_table(config, "integration")
  local presentation = require_table(config, "presentation")
  if not metric or not observer or not disk or not integration or
     not presentation then
    return nil, "CONFIG_FIELDS_MISSING"
  end

  local config_hash = hash_from_config(config)
  local revision = revision_from_config(config)
  if type(config_hash) ~= "string" or #config_hash < 8 or
     not finite(revision) or revision < 1 or revision % 1 ~= 0 then
    return nil, "CONFIG_IDENTITY_INVALID"
  end

  local spin = metric.spin_chi
  local mass = metric.mass_solar
  local radius = observer.radius_over_m
  local inclination = observer.inclination_deg
  local azimuth = observer.azimuth_deg
  local fov = observer.fov_deg
  local outer = disk.outer_radius_over_m
  local luminosity = disk.luminosity_eddington_ratio
  local escape = integration.escape_radius_over_m
  local residual = integration.residual_limit
  local capture = optional_number(integration, "capture_epsilon", 0.002)
  local history = optional_number(integration, "history_weight", 0.75)
  local quality = quality_index(integration.max_steps)
  local spectrum_mode = SPECTRUM_MODES[disk.spectrum]

  if not finite(spin) or spin < -0.998 or spin > 0.998 or
     not finite(mass) or mass <= 0.0 or mass > 1e12 or
     observer.model ~= "zamo" or
     not finite(radius) or radius < 12.0 or radius > 1000000.0 or
     not finite(inclination) or inclination < 0.0 or inclination > 89.0 or
     not finite(azimuth) or
     not finite(fov) or fov < 5.0 or fov > 70.0 or
     disk.model ~= "page_thorne_thin" or
     spectrum_mode == nil or
     not finite(outer) or outer <= kerr_isco(spin) or outer > 100000.0 or
     not finite(luminosity) or luminosity <= 0.0 or luminosity > 1.0 or
     not quality or
     not finite(escape) or escape <= radius or escape > 10000000.0 or
     not finite(residual) or residual <= 0.0 or residual > 0.1 or
     not finite(capture) or capture <= 0.0 or capture > 0.1 or
     not finite(history) or history < 0.0 or history > 0.99 then
    return nil, "CONFIG_RANGE_INVALID"
  end

  local spectrum = type(config.spectrum) == "table" and config.spectrum or {}
  local band_min = optional_number(spectrum, "band_min_nm", 380.0)
  local band_max = optional_number(spectrum, "band_max_nm", 780.0)
  local temperature_min = optional_number(
    spectrum, "temperature_min_kelvin", 2200.0)
  local temperature_max = optional_number(
    spectrum, "temperature_max_kelvin", 18000.0)
  local limb = optional_number(disk, "limb_darkening", 0.5)
  local compare = optional_number(presentation, "compare_split", -1.0)
  if not finite(band_min) or not finite(band_max) or band_min < 100.0 or
     band_max <= band_min or band_max > 5000.0 or
     not finite(temperature_min) or temperature_min < 1000.0 or
     not finite(temperature_max) or temperature_max <= temperature_min or
     temperature_max > 40000.0 or
     not finite(limb) or limb < 0.0 or limb > 1.0 or
     not finite(compare) or not (compare == -1.0 or
       (compare >= 0.0 and compare <= 1.0)) then
    return nil, "CONFIG_RANGE_INVALID"
  end

  local background = type(config.celestial_background) == "table" and
    config.celestial_background or {}
  local quaternion = background.orientation or {0.0, 0.0, 0.0, 1.0}
  if type(quaternion) ~= "table" or #quaternion ~= 4 then
    return nil, "CONFIG_BACKGROUND_INVALID"
  end
  local q2 = 0.0
  for i = 1, 4 do
    if not finite(quaternion[i]) then return nil, "CONFIG_BACKGROUND_INVALID" end
    q2 = q2 + quaternion[i] * quaternion[i]
  end
  if q2 < 1e-12 then return nil, "CONFIG_BACKGROUND_INVALID" end
  local q_inv = 1.0 / math.sqrt(q2)

  local flux = type(config.flux_lut) == "table" and config.flux_lut or {}
  local spin_min = optional_number(flux, "spin_min", -0.998)
  local spin_step = optional_number(flux, "spin_step", 0.01584375)
  local radial_count = optional_number(flux, "radial_count", 512)
  local spin_count = optional_number(flux, "spin_count", 127)
  if not finite(spin_min) or spin_min < -0.998 or spin_min > 0.998 or
     not finite(spin_step) or spin_step <= 0.0 or
     not finite(radial_count) or radial_count < 2 or radial_count > 16384 or
     radial_count % 1 ~= 0 or not finite(spin_count) or spin_count < 2 or
     spin_count > 4096 or spin_count % 1 ~= 0 then
    return nil, "CONFIG_LUT_INVALID"
  end

  local step_r = optional_number(integration, "step_fraction_r", 0.04)
  local step_theta = optional_number(integration, "step_fraction_theta", 0.035)
  local step_phi = optional_number(integration, "step_fraction_phi", 0.035)
  local edge = optional_number(integration, "edge_threshold", 0.04)
  if not finite(step_r) or step_r <= 0.0 or step_r > 0.25 or
     not finite(step_theta) or step_theta <= 0.0 or step_theta > 0.25 or
     not finite(step_phi) or step_phi <= 0.0 or step_phi > 0.25 or
     not finite(edge) or edge <= 0.0 or edge > 1.0 then
    return nil, "CONFIG_INTEGRATION_INVALID"
  end

  local diagnostic_lookup = {
    radiance = 0, outcome = 1, redshift = 2,
    image_order = 3, steps = 4, residual = 5,
  }
  local diagnostic = diagnostic_lookup[presentation.diagnostic]
  if diagnostic == nil then return nil, "CONFIG_DIAGNOSTIC_INVALID" end

  return {
    config_hash = config_hash,
    revision = revision,
    mass = mass,
    spin = spin,
    radius = radius,
    inclination = inclination,
    azimuth = azimuth,
    fov = fov,
    disk_outer = outer,
    luminosity = luminosity,
    spectrum_mode = spectrum_mode,
    escape = escape,
    residual = residual,
    capture = capture,
    history = history,
    quality = quality,
    band_min = band_min,
    band_max = band_max,
    temperature_min = temperature_min,
    temperature_max = temperature_max,
    limb = limb,
    compare = compare,
    quaternion = {
      quaternion[1] * q_inv, quaternion[2] * q_inv,
      quaternion[3] * q_inv, quaternion[4] * q_inv,
    },
    spin_min = spin_min,
    spin_inv_step = 1.0 / spin_step,
    radial_count = radial_count,
    spin_count = spin_count,
    step_r = step_r,
    step_theta = step_theta,
    step_phi = step_phi,
    edge = edge,
    diagnostic = diagnostic,
    exposure_ev = optional_number(presentation, "exposure_ev", 0.0),
  }
end

local function validate_reference(reference, config)
  if not contract_is(reference, "ck.kerr-reference-summary") then
    return nil, "REFERENCE_SCHEMA_MISMATCH"
  end
  if reference.config_hash ~= config.config_hash then
    return nil, "REFERENCE_HASH_MISMATCH"
  end
  if type(reference.source_corpus_hash) ~= "string" or
     #reference.source_corpus_hash < 8 then
    return nil, "REFERENCE_IDENTITY_INVALID"
  end
  if type(reference.validation) ~= "table" or
     reference.validation.status ~= "passed" then
    return nil, "REFERENCE_VALIDATION_FAILED"
  end
  return reference
end

local function exact_entity(name)
  local entity, count = jce.find_by_name(name)
  if count ~= 1 or not entity then return nil end
  return entity
end

local function resolve_entities()
  local names = {
    root = "BlackHoleLabRoot",
    camera = "KerrObserverCamera",
    anchor = "KerrCoordinateAnchor",
    effect = "KerrFullscreenEffect",
    readout = "CriticalCurveOverlay",
    terminology = "KerrTerminology",
    reference = "ReferenceRayProbe",
  }
  for key, name in pairs(names) do
    D.ids[key] = exact_entity(name)
    if not D.ids[key] then return false, "SCENE_ENTITY_MISMATCH:" .. name end
  end
  return true
end

local function verify_unit_effect_chain()
  local entity = D.ids.effect
  local visited = {}
  for _ = 1, 16 do
    if not entity or entity == 0 then return true end
    if visited[entity] then return false end
    visited[entity] = true
    local sx, sy, sz = jce.get_scale(entity)
    if not sx or math.abs(sx - 1.0) > 1e-5 or
       math.abs(sy - 1.0) > 1e-5 or math.abs(sz - 1.0) > 1e-5 then
      return false
    end
    entity = jce.get_parent(entity)
  end
  return false
end

local function fail_closed(code, detail)
  D.active = false
  if D.ids.effect then
    jce.set_component_enabled(D.ids.effect, "FullscreenEffect", false)
  end
  local suffix = detail and (" " .. detail) or ""
  jce.log("black_hole_lab: ERROR " .. code .. suffix)
  if D.ids.reference then
    jce.ui_set_text(D.ids.reference,
      "LAB DISABLED | " .. code .. suffix)
  end
end

local function mark_changed(camera_changed)
  D.history_invalid_frames = 1
  D.effect_dirty = true
  D.hud_dirty = true
  if camera_changed then D.camera_dirty = true end
end

local function pack_params()
  local c = D.config
  local spin = c.spin
  local rows = {
    {spin, c.mass, c.luminosity, c.spectrum_mode},
    {c.disk_outer, c.escape, c.capture, c.residual},
    {QUALITY_STEPS[D.quality], D.diagnostic, D.sample_phase, c.history},
    {c.temperature_min, c.temperature_max, c.limb, c.compare},
    {c.quaternion[1], c.quaternion[2], c.quaternion[3], c.quaternion[4]},
    {c.spin_min, c.spin_inv_step, c.radial_count - 1, c.spin_count - 1},
    {c.step_r, c.step_theta, c.step_phi, c.edge},
    {kerr_horizon(spin), kerr_isco(spin), c.revision, 0.0},
  }
  for i = 1, 8 do
    rows[i + 8] = {rows[i][1], rows[i][2], rows[i][3], rows[i][4]}
  end
  return rows
end

local function write_effect()
  local use_history = D.config.history > 0.0 and
                      D.history_invalid_frames <= 0
  local descriptor = {
    enabled = true,
    required = true,
    useSceneColor = false,
    useSceneDepth = false,
    useHistory = use_history,
    order = 0,
    insertion = 0,
    blend = 0,
    outputFormat = 1,
    resolutionScale = 1.0,
    shader = "ck_kerr_lensing_" .. tostring(QUALITY_STEPS[D.quality]),
    textures = EFFECT_TEXTURES,
    params = pack_params(),
  }
  if not jce.comp_set(D.ids.effect, "FullscreenEffect", json.encode(descriptor)) then
    fail_closed("EFFECT_COMPONENT_REJECTED")
    return
  end
  if D.history_invalid_frames > 0 then
    D.history_invalid_frames = D.history_invalid_frames - 1
    D.effect_dirty = true
  else
    D.effect_dirty = false
  end
end

local function update_camera()
  local c = D.config
  local theta = math.rad(c.inclination)
  local phi = math.rad(c.azimuth)
  local oblate = math.sqrt(c.radius * c.radius + c.spin * c.spin)
  local x = oblate * math.sin(theta) * math.cos(phi)
  local y = c.radius * math.cos(theta)
  local z = oblate * math.sin(theta) * math.sin(phi)
  local length = math.sqrt(x * x + y * y + z * z)
  local fx, fy, fz = -x / length, -y / length, -z / length
  -- Scene Camera components look down local -Z.  Transform Euler signs are
  -- therefore the inverse of the FPS camera's yaw convention.
  local pitch = math.deg(math.asin(clamp(fy, -1.0, 1.0)))
  local yaw = math.deg(math.atan(-fx, -fz))

  jce.set_position(D.ids.camera, x, y, z)
  jce.set_rotation(D.ids.camera, pitch, yaw, 0.0)
  jce.comp_set(D.ids.camera, "Camera", json.encode({
    fov = c.fov,
    nearClip = 0.01,
    farClip = math.max(c.escape * 1.2, c.radius * 10.0),
    primary = true,
    orthographic = false,
  }))
  D.camera_dirty = false
end

local function update_hud()
  local c = D.config
  local spin = c.spin
  local preset = PRESETS[D.preset]
  jce.ui_set_text(D.ids.readout, string.format(
    "%s | chi=%+.3f | inclination=%.3f deg | observer=%.3fM | FOV=%.2f deg\n" ..
    "r_plus=%.6fM | r_ISCO=%.6fM | Page-Thorne disk=%.1fM | tier=%d | %s%s",
    preset.name, spin, c.inclination, c.radius, c.fov,
    kerr_horizon(spin), kerr_isco(spin), c.disk_outer,
    QUALITY_STEPS[D.quality], DIAGNOSTICS[D.diagnostic + 1],
    D.paused and " | temporal sampling paused" or ""))
  if D.overlay then
    jce.ui_set_text(D.ids.terminology,
      "Event horizon: causal boundary, not a surface.  " ..
      "Critical curve: ideal image-plane boundary of asymptotic photon orbits.\n" ..
      "Shadow: source-dependent brightness depression.  " ..
      "Photon ring and lensing ring are distinct higher-order image features.")
  else
    jce.ui_set_text(D.ids.terminology, "")
  end
  jce.ui_set_text(D.ids.reference, string.format(
    "REFERENCE_DATA | config=%s | corpus=%s | GPU validation: reports/black_hole",
    c.config_hash, D.reference.source_corpus_hash))
  D.hud_dirty = false
end

local function apply_preset(index)
  local preset = PRESETS[index]
  if not preset then return end
  D.preset = index
  D.config.spin = preset.spin
  D.config.inclination = preset.inclination
  D.config.fov = preset.fov
  D.config.radius = D.base.radius
  D.config.azimuth = D.base.azimuth
  D.diagnostic = 0
  mark_changed(true)
end

local function reset_preset()
  apply_preset(D.preset)
  D.quality = D.base.quality
  D.paused = false
  D.sample_phase = -1.0
  D.sample_counter = 0
  mark_changed(true)
end

local function set_quality(index)
  local next_quality = clamp(round(index), 1, #QUALITY_STEPS)
  if next_quality ~= D.quality then
    D.quality = next_quality
    mark_changed(false)
  end
end

local function set_diagnostic(index)
  local next_mode = math.floor(index) % #DIAGNOSTICS
  if next_mode < 0 then next_mode = next_mode + #DIAGNOSTICS end
  if next_mode ~= D.diagnostic then
    D.diagnostic = next_mode
    mark_changed(false)
  end
end

local function update_inputs(dt)
  for i = 1, 6 do
    if jce.is_action_pressed("black_hole.preset_" .. tostring(i)) then
      apply_preset(i)
      break
    end
  end
  if jce.is_action_pressed("black_hole.pause") then
    D.paused = not D.paused
    D.sample_phase = D.paused and D.sample_counter or -1.0
    mark_changed(false)
  end
  if jce.is_action_pressed("black_hole.reset") then reset_preset() end
  if jce.is_action_pressed("black_hole.diagnostic") then
    set_diagnostic(D.diagnostic + 1)
  end
  if jce.is_action_pressed("black_hole.overlay") then
    D.overlay = not D.overlay
    D.hud_dirty = true
  end
  if jce.is_action_pressed("black_hole.quality_up") then
    set_quality(D.quality + 1)
  end
  if jce.is_action_pressed("black_hole.quality_down") then
    set_quality(D.quality - 1)
  end

  local spin_axis = jce.get_axis("black_hole.spin")
  local inclination_axis = jce.get_axis("black_hole.inclination")
  local radius_axis = jce.get_axis("black_hole.radius")
  local fov_axis = jce.get_axis("black_hole.fov")
  if math.abs(spin_axis) > 1e-5 then
    D.config.spin = clamp(D.config.spin + spin_axis * 0.4 * dt, -0.998, 0.998)
    mark_changed(true)
  end
  if math.abs(inclination_axis) > 1e-5 then
    D.config.inclination = clamp(
      D.config.inclination + inclination_axis * 30.0 * dt, 0.0, 89.0)
    mark_changed(true)
  end
  if math.abs(radius_axis) > 1e-5 then
    D.config.radius = clamp(D.config.radius - radius_axis * 25.0 * dt,
                            12.0, 1000000.0)
    mark_changed(true)
  end
  if math.abs(fov_axis) > 1e-5 then
    D.config.fov = clamp(D.config.fov + fov_axis * 20.0 * dt, 5.0, 70.0)
    mark_changed(true)
  end

  local wheel = jce.get_pointer_wheel()
  if math.abs(wheel) > 1e-5 then
    if jce.is_action_down("black_hole.fov") then
      D.config.fov = clamp(D.config.fov - wheel * 2.0, 5.0, 70.0)
    else
      D.config.radius = clamp(D.config.radius - wheel * 5.0, 12.0, 1000000.0)
    end
    mark_changed(true)
  end
  if jce.is_action_down("black_hole.orbit") then
    local dx, dy = jce.get_pointer_delta()
    if math.abs(dx) > 1e-5 or math.abs(dy) > 1e-5 then
      D.config.azimuth = (D.config.azimuth + dx * 0.15) % 360.0
      D.config.inclination = clamp(D.config.inclination + dy * 0.12,
                                   0.0, 89.0)
      mark_changed(true)
    end
  end
end

function ck_black_hole_select_preset(_, value)
  if D.active then apply_preset(clamp(round(value), 1, #PRESETS)) end
end

for i = 1, #PRESETS do
  _G["ck_black_hole_preset_" .. tostring(i)] = function()
    if D.active then apply_preset(i) end
  end
end

function ck_black_hole_set_spin(_, value)
  if not D.active or not finite(value) then return end
  D.config.spin = clamp(value, -0.998, 0.998)
  mark_changed(true)
end

function ck_black_hole_set_inclination(_, value)
  if not D.active or not finite(value) then return end
  D.config.inclination = clamp(value, 0.0, 89.0)
  mark_changed(true)
end

function ck_black_hole_set_radius(_, value)
  if not D.active or not finite(value) then return end
  D.config.radius = clamp(value, 12.0, 1000000.0)
  mark_changed(true)
end

function ck_black_hole_set_fov(_, value)
  if not D.active or not finite(value) then return end
  D.config.fov = clamp(value, 5.0, 70.0)
  mark_changed(true)
end

function ck_black_hole_set_quality(_, value)
  if D.active and finite(value) then set_quality(value) end
end

function ck_black_hole_cycle_diagnostic()
  if D.active then set_diagnostic(D.diagnostic + 1) end
end

function ck_black_hole_toggle_pause()
  if not D.active then return end
  D.paused = not D.paused
  D.sample_phase = D.paused and D.sample_counter or -1.0
  mark_changed(false)
end

function ck_black_hole_toggle_overlay()
  if not D.active then return end
  D.overlay = not D.overlay
  D.hud_dirty = true
end

function ck_black_hole_reset()
  if D.active then reset_preset() end
end

local M = {}

function M:on_start()
  local ok, error_code = resolve_entities()
  if not ok then fail_closed("SCENE_ENTITY_MISMATCH", error_code); return end
  if not verify_unit_effect_chain() then
    fail_closed("EFFECT_WORLD_NON_UNIT")
    return
  end

  local raw_config, config_error = jce.asset_read_json(CONFIG_PATH)
  if not raw_config then
    fail_closed("CONFIG_LOAD_FAILED", config_error)
    return
  end
  local config, validate_error = validate_config(raw_config)
  if not config then fail_closed(validate_error); return end

  local raw_reference, reference_error = jce.asset_read_json(REFERENCE_PATH)
  if not raw_reference then
    fail_closed("REFERENCE_LOAD_FAILED", reference_error)
    return
  end
  local reference, reference_validate_error =
    validate_reference(raw_reference, config)
  if not reference then fail_closed(reference_validate_error); return end

  D.config = config
  D.reference = reference
  D.quality = config.quality
  D.diagnostic = config.diagnostic
  D.base = {
    radius = config.radius,
    azimuth = config.azimuth,
    quality = config.quality,
  }
  D.active = true
  D.effect_dirty = true
  D.camera_dirty = true
  D.hud_dirty = true
  D.history_invalid_frames = 1
  -- Only exposure is the director's to own.  This patch used to carry
  -- bloom = false, which silently overrode the scene's own postfx block on
  -- every start: rendering.postfx in black_hole_lab.scene.json was dead on
  -- arrival, and tuning bloomThreshold/bloomIntensity there did nothing at
  -- all.  It was not merely weak -- jce_postfx.c gates the whole bloom pass
  -- on the flag, so the passes were never submitted and A/B output was
  -- bit-identical, which is what made it look like bloom was unsupported.
  jce.render_set(json.encode({postfx = {
    exposure = 2.0 ^ config.exposure_ev,
    vignette = false, chromatic = false,
  }}))
  update_camera()
  update_hud()
  write_effect()
  jce.log("black_hole_lab: activated config=" .. config.config_hash)
end

function M:on_update(dt)
  if not D.active then return end
  dt = clamp(finite(dt) and dt or 0.0, 0.0, 0.1)
  if not D.paused then D.sample_counter = D.sample_counter + 1 end
  update_inputs(dt)
  if D.camera_dirty then update_camera() end
  if D.hud_dirty then update_hud() end
  if D.effect_dirty then write_effect() end
end

return M
