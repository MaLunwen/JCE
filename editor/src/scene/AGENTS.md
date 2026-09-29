# editor/src/scene/ — Editor Scene Rendering & Asset Cache

## Identity
- **Language**: C++17
- **Role**: editor-side scene drawing (overlays, helpers, picking) + asset cache mediating between editor and engine resource system.

## File map
- `jce_editor_scene_render.{h,cpp}` (29 KB) — top-level editor scene render (calls engine render then overlays).
- `jce_scene_render_camera.cpp`             — editor cameras (orbit / fly / focus-on-selection).
- `jce_scene_camera_focus.{h,cpp}`          — pure Unity-style Scene View focus math (AABB fit distance, local→world AABB transform, two-mode repeat-focus close/far toggle, far mode = 2x fit distance, smooth easing).
- `jce_scene_outline_policy.{h,cpp}`        — pure selection-outline fallback policy (visual renderers never use debug placeholder boxes when geometry is missing).
- `jce_scene_content_context.{h,cpp}`       — pure source-vs-isolated-Bundle content path policy; Bundle Preview never inherits stale host project paths.
- `jce_scene_render_draw.cpp`               — overlay primitives (grid, gizmo, light icons).
- `jce_scene_render_internal.h`             — cross-TU state.
- `jce_editor_game_render.{h,cpp}`          — Play-mode render path (engine-owned, editor wraps).
- `jce_editor_scene_asset_cache.{h,cpp}`    — facade over engine asset cache; tracks dirty-from-disk for hot-reload.
- `jce_asset_cache_internal.h`              — cross-TU asset cache state.
- `jce_asset_cache_resolve.cpp` (26 KB)     — GUID → loaded asset resolver.
- `jce_asset_cache_mesh.cpp`                — mesh cache (engine handle + editor metadata).
- `jce_asset_cache_material.cpp`            — material cache.
- `jce_asset_cache_texture.cpp`             — texture cache (CPU thumb + GPU handle).
- `jce_asset_path_index.{h,cpp}`            — path→GUID lookup, file watcher hook.
- `jce_model_loader_assimp.h`               — assimp import bridge (editor-only; runtime uses glTF).

## Rules
1. **Editor render layers on top of engine render** — never re-implement scene traversal; call `<jce/api_render.h>` then draw overlays.
2. **Asset cache here is editor-only** — adds dirty tracking, thumbnail generation, import-time metadata. Runtime uses engine's own cache.
3. **Assimp is import-only**: convert to glTF / engine-native at import time; never load `.fbx` at runtime.
4. **Hot reload**: file watcher fires → `jce_asset_path_index` invalidates → next frame re-resolves via `_resolve.cpp`.
5. **All GPU handles routed through engine** — never call bgfx directly.
6. **Camera math**: use `jce_math` quaternions; never raw Euler.
7. **Picking**: GPU readback through engine's render-target API; cache last-pick result for one frame.
8. **Bundle Preview is hermetic**: when the active VFS policy is isolated, render settings and component-relative assets use VFS-relative addresses only; never synthesize host project paths.

## Don't
- Don't load assets synchronously on the UI thread — use `jce_thread_pool_*` (engine/include/jce/os/core/jce_thread.h) and show a placeholder.
- Don't keep a second copy of mesh CPU data (it's already in engine's resource layer).
- Don't bypass the cache; direct loads break hot-reload.

## Common tasks
- **Add an overlay** → new draw call in `jce_scene_render_draw.cpp`; toggle via `jce_editor_config`.
- **Support new asset type** → add cache module here + matching engine resource loader; register in `_resolve.cpp`.

## bgfx view ids: the tail band is NOT free real estate (2026-08-29)

`jce_editor_viewport_common.h` derives every viewport pass from a scene-renderer
BASE (`JCE_VIEW_EDITOR_SCENE` = 3, `JCE_EDITOR_GAME_VIEW_BASE` = 80). The engine
also derives passes from that base, and it hands ITS ids to
`bgfx_set_view_order()` so they sort before the colour pass.

Consequence, and it is silent: an editor pass parked on an id the engine NAMES
inherits that early sort position, rasterises into the bridge, and is then
erased by the colour pass's clear. No error, no log, no pixels.

That is what happened: `JCE_EDITOR_VP_UI_OFFSET` was 52, byte for byte the
dynamic-CSM atlas band (`JCE_VIEW_SR_DYN_CSM_OFFSET` .. +4), so the ECS-UI
Canvas overlay drew nothing in EITHER viewport while the shipped runtime (which
uses the absolute `JCE_VIEW_UI` = 254) drew it correctly. The upscale, the
read-back capture and TSR were on the other four ids of the same band.

Rules now enforced, not remembered:

* `JCE_VIEW_SR_OFFSET_RESERVED()` in `<jce/renderer/jce_views.h>` is the engine's
  reservation table. Every editor offset is `static_assert`ed against it in
  `jce_editor_viewport_common.h`; reintroducing 52 is a BUILD FAILURE naming the
  offset. Verified by reverting it — the compiler said so.
* Both viewports call `jce_editor_viewport_claim_view_band()` every frame, so
  `jce_view_bands_claim` can report a cross-base overlap the static_assert
  cannot see. The count shows in the Profiler panel.
* Filler ids (in the remap window but NOT named by the engine) are safe: the
  order builder appends them after every named id, so they sort after the colour
  pass. That is why the postfx band at +30..+50 always worked.

KNOWN, UNFIXED, OPT-IN: with `r.point_shadows` / `JCE_POINT_CUBE_SHADOWS` on
(default OFF) the engine also names base+100..116, which for the Scene View is
103..119 — inside the Game View's range. Cross-BASE, so no static_assert can
reach it; the runtime claim reports it. The fix is re-spacing the bases, and the
256-id bgfx budget has no room.
