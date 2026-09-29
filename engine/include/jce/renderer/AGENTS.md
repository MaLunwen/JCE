# engine/include/jce/renderer/ — Renderer Public API

## Identity
- **Layer**: L3 (Renderer). Pure C99 headers; consumers must not include
  bgfx headers directly.
- **Role**: every public renderer call (caps, render graph, scene
  renderer, material, lighting, shadows, post-FX, particles, primitives,
  text, sprite batch, IBL, decals, debug draw, …).

## Rules
1. **C99 only** — no `<vector>` / `<string>` / classes. Use `JceXxx` types.
2. **Stable ABI** — adding a field to a public struct or reordering enum
   members is a breaking change. Append-only.
3. **No bgfx in headers** — keep the bgfx handle types behind opaque
   `JceXxx*` pointers or wrapper enums (e.g. `JceRendererBackend`).
4. **Must be re-exported** through `engine/include/jce/api_render.h` or
   `api_graphics.h`.
5. **Symmetric C/C++** — every header wraps declarations in
   `JCE_EXTERN_C_BEGIN / _END`.

## Caps tiering (`jce_renderer_caps.h`)
- `JceGpuTier { LOW, MEDIUM, HIGH, ULTRA }` drives auto-quality scaling
  for shadows, post-FX, PBR, SSR, TAA, volumetric fog, GPU particles.
- `jce_renderer_get_tier()` returns the **effective** tier after applying
  any active editor override.
- `jce_renderer_set_tier_override(tier)` /
  `jce_renderer_clear_tier_override()` /
  `jce_renderer_tier_is_overridden()` — editor/test knob to preview the
  512 MB / no-GPU baseline behaviour on developer hardware.
  Volatile-int storage, no persistence; never call from shipping game
  code paths.
- `jce_renderer_get_recommendation()` reads the **effective** tier so
  override automatically propagates to recommended settings.
- ULTRA currently mirrors HIGH gating; reserved for future explicit
  high-end discrete profiles (RT, mesh shaders, …).

## Don't
- Don't add a new caps query without adding the bgfx-side probe in
  `engine/src/renderer/jce_renderer_caps.c`.
- Don't expose bgfx caps bits directly — translate into `JCE_CAP_*`.

## Reflection Probe Bake (`jce_reflection_probe_bake.h`) — P3-E.3
- Unity parity Bake flow for `JceReflectionProbeComponent`.
  `jce_reflection_probe_bake_submit(&desc)` returns a `JceBakeHandle`
  (uint32, 0 = sentinel/failure); poll with `_poll_status()` /
  `_poll_progress()`; cancel with `_cancel()`.
- **Single-slot worker**: only one bake runs at a time engine-wide;
  resubmit while busy returns 0. Status enum `JCE_BAKE_IDLE / RUNNING /
  DONE / FAILED / CANCELLED`.
- **Encoder (P3-E.3 follow-up)**: real KTX1 cubemap via bimg
  (`engine/src/renderer/jce_ktx2_writer.cpp`). The legacy `JCEC` /
  `.cube` placeholder is gone. An irradiance sidecar lands next to the
  specular file as `<stem>.irr.ktx` (placeholder source = same RGBA8
  faces until analytic SH convolution lands with live capture).
  Caller-supplied `.cube` or `.ktx2` extensions are rewritten to `.ktx`
  in place — the `output_path_ktx2` field name is preserved for ABI
  stability.
- **Pending limitations**: CPU procedural sky source (no live scene
  capture — bgfx is not thread-safe); single-mip KTX (specular mip
  chain bake is the next increment). KTX2 / supercompression awaits a
  bimg upgrade.
- Consumed by editor inspector (`inspector_lighting.cpp`) and Reflection
  Probes panel (`jce_panel_reflection_probes.cpp`); both write `.ktx`
  paths into `baked_cubemap_path`.

## Render Pipeline Asset (`jce_render_pipeline.h`) — P3-E.4
- Unity URP/HDRP parity: a single `.rp.json` descriptor binds at engine
  boot via `jce_render_pipeline_apply_boot("Settings/RenderPipeline.rp.json")`.
- 4 built-in presets (`_preset_low/_mid/_high/_ultra`) — LOW honours
  the 512 MB / no-discrete-GPU baseline (CSM off, shadow 512, no SSAO/
  SSR/TAA/MSAA, RGBA8, no Z-prepass).
- `_preset_for_current_tier()` returns the preset matching the
  effective GPU tier; `_apply_boot()` falls back to it when the asset
  file is missing.
- Render-graph code gates passes via `jce_render_pipeline_is_feature_enabled("csm"|"ssao"|...)`.
- Stable ABI — fields are append-only; bump `$schema` (`jce.rp.v1`)
  on breaking changes.

## P3-E.5 — Light Cookies + IES Profiles

Spot lights ship end-to-end (cookie texture + IES LM-63 photometric LUT) via new `JceSpotLight` fields (`cookie_texture` / `ies_lut_texture` / `cookie_strength` / `cookie_path` / `ies_path`) and matching `JceSpotLightDesc` fields. Directional cookie is data-side scaffolded (component, serializer, inspector, uniform upload) with the shader projection guarded `#if 0` until a CSM-aligned VP is wired. Sampler slots **13 = `s_cookie`** and **14 = `s_iesLut`**; v1 binds the FIRST spot light that has a cookie / IES (multi-cookie atlas deferred to P3-E.5b). New public header: `<jce/renderer/jce_ies_profile.h>` (`jce_ies_parse`, `jce_ies_bake_lut_from_file`, `jce_ies_bake_lut_from_memory`). **Rebuild shaders**: `fs_pbr.sc` adds samplers + `u_cookieParams` / `u_cookieSpotVP` uniforms — run shaderc against `engine/shaders/pbr/fs_pbr.sc` to refresh `_cooked/*/shaders/*/fs_pbr.bin`.

## P3-E.5b — Directional cookie projection + cookie atlas

`<jce/renderer/jce_lighting_system.h>` exposes `JCE_COOKIE_ATLAS_CAPACITY` (16) and `jce_light_env_register_cookie(env, JceTexture) -> int slot`. Slot 0 is reserved for the white "no cookie" default; slots 1..15 are LRU-managed (smallest `last_used` counter evicted). The per-light atlas slot is uploaded inside `u_dirLights[i*2+1].w` and `u_spotLights[i*4+3].y`, ready to drive `SAMPLER2DARRAY` sampling once `s_cookie` is promoted. v1 keeps the single-bind path from P3-E.5 (one cookie per draw on sampler 13) — spot cookies win the bind, directional cookies fall back when no spot cookie is present. Directional VP is a **world-aligned ortho box** (±50 m, near 0.1 m, far 200 m) centred on the camera position, aimed along the chosen directional light's forward — see `engine/src/renderer/AGENTS.md` "P3-E.5b" for the v2 CSM-cascade-0-fit path.
