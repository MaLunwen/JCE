# engine/include/jce/renderer/ — Renderer Public API

The public `JCE_GRAPHICS_API_TIER_COMPATIBILITY` policy is appended at value 4.
Stable, modern, current and the legacy COUNT sentinel retain values 0/1/2/3.
COUNT remains invalid for name and floor lookup. `JceRendererApiInfo` retains
its layout and reports the public policy value.
This explicit GL 2.1 / GLES 2.0 floor is separate from GPU quality tiers and does
not imply every stock shader, extension, render target or Editor feature works.
The version snapshot continues to distinguish observed values from unverified
fallbacks. Vulkan 1.0 is policy data, not a new compiled-backend certificate.

The pure C99 inline `jce_graphics_api_is_compatibility_gl_es` shares explicit
API-tier/backend policy without exporting a new symbol or changing ABI layout.
It is a policy predicate, not hardware capability or shader availability. The
private renderer adapter still maps the actual backend before consuming it;
middleware uses the public API snapshot and keeps custom loaders unrestricted.

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

The offscreen target's get_extent observes its current live allocation and
owner-admitted size without allocation or backend queries. Failure clears
supplied outputs. A successful query cannot establish current-frame content,
render submission, GPU completion or presentation. Editor bridge readbacks use
this extent; PostFX output dimensions remain a separate source authority.

The additive `jce_scene_renderer_render_linear` symbol preserves the existing
render signature and unversioned `JceSceneRenderConfig` layout. It scopes a
private per-renderer PBR output override around the ordinary render and
restores both output flags after all returns. It skips PBR display gamma and
dither, while other passes still follow config and callers own composition.
Reflection-probe capture consumes this entry with reduced-pass config; enabling
authored PostFX is not a substitute for choosing a linear capture output.

## Caps tiering (`jce_renderer_caps.h`)

Optional compacted model survivor snapshots export the actual GPU counters,
with dispatch identity and borrowed RGBA32F texture lifetime. Counts use exact
low/high 16-bit float words. Consumers reuse typed asynchronous readback with
explicit cull/snapshot/blit view ordering; fixed-count paths are unavailable.

`jce_gpu_scene_set_indirect_enabled` selects compacted indirect or fixed-count
model submission before a batch begins. Hardware/program capability remains
separate from policy and actual dispatch readiness. Foliage owns its submission
path independently; disabling model indirect must not claim missing support.
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

## Typed readback and impostor shutdown ownership

`jce_render_readback_*` is an asynchronous renderer-owner-thread contract.
The queue owns its staging texture and CPU transfer destination. Cancel/release
can return true while that storage remains borrowed by the backend; repeated
calls do not imply immediate reclamation. Observe actual completion through
poll and `ready_frame`, then release the ticket. Destroy queues while their
renderer is still alive: destruction drains submitted completion frames and
releases owned storage. Layout validation and CPU ownership controls are not
proof of successful GPU transfer or correct source pixels.

`jce_impostor_shutdown` is renderer lifecycle cleanup, not a public completion
fence. It retires GPU resources before backend shutdown and closes the pending
bake's admission. The renderer's private post-join hook releases any already
submitted CPU destination and resets the single bake slot for a later session.
An application must not restart rendering between these shutdown boundaries or
call the private hook itself. Public API/ABI and the one-bake policy are unchanged.

JceSceneRendererCallbacks.load_model transfers an independently owned successful
result to the calling scene/pick cache. It must not return a shared borrowed
model. The current callback-mode VRAM accounting/eviction exclusion is a policy
limitation, not a borrowing contract; explicit invalidation and teardown destroy
callback models. Attached pick passes borrow scene-renderer results only for the
current ID render and resolve again on the next request.

## Scene preparation API

`jce_scene_renderer_prepare_resources(sr, scene)` is an additive SDK render-thread
resource stage. Its bool reports device/context admission, not asset readiness.
It permits existing uploads/retirements, submits no scene colour/shadow/compute
and takes no dt. Backend CPU frame/context facts are preparation identity, never
a GPU fence or simulation clock. Legacy rendering keeps its automatic fallback;
this API is not full scene maintenance or permission to stop time-dependent
render calls. No public render-config struct layout changes for this slice.

## Camera clone ownership

The public camera clone returns an independent CPU value object, preserving
pose/roll, perspective and fixed-width or aspect-derived orthographic state.
NULL source or allocation failure returns NULL. A clone borrows nothing from
its source and remains valid after source destruction; callers release it with
`jce_camera_destroy`. No camera layout or renderer-private type is exposed.

`jce_graphics_api_is_compatibility_gl_es` is a pure C99 static-inline policy
helper over existing public enums. It adds no exported symbol or ABI field and
certifies no hardware capability. Middleware consumes observed ApiInfo; the
private renderer adapter delegates after mapping its actual bgfx backend.
