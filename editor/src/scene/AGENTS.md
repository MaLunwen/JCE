# editor/src/scene/ — Editor Scene Rendering & Asset Cache

The selected TriggerVolume overlay uses the runtime registration coordinate
rule: world matrix translation plus the authored world-axis offset. Draw it
before mesh-only early returns. OBB axes describe the overlap slab normals;
a finite outline uses their dual basis. Entity-kind script parameters on an
enabled trigger's enabled script are authoring references in the active scene.
Resolve full live handles, skip stale/transformless references, and do not
derive them from runtime observer cookies or infer executed actions.

The shared viewport PostFX composite may skip its bridge copy only for D3D11
with a top-left origin, an independent valid output and no later bridge
consumer. An immediate public Canvas-component query is conservative: any
Canvas retains the copy, without duplicating enabled/layout/draw policy or
caching scene pointers. Scene scaled TSR/RCAS and Game physics debug retain it.
Canvas render, focus/input retirement and UI-event dispatch remain in place
even when the query is empty. Direct output stays owned by its existing PostFX
pipeline; the bridge stays owned and the current getter/screenshot helper
selects that borrowed output. Other backends keep the orientation-normalizing
copy. This does not change an authored effect or certify pixels/performance.
Scene uses its SceneRenderer-owned PostFX pipeline; Game creates its own
pipeline and mirrors only authorable parameters. Their shared SceneRenderer
does not imply shared PostFX output targets. Only these two Editor call sites
resize/apply their respective pipelines; later UI parameter edits do not redraw
or resize a queued output. Probe/impostor admission still owns its host frame.

Overview retains authored streaming-chunk bounds, including unloaded chunks.
Without chunks it unions finite, ordered bounds from the existing entity focus
owner for enabled registered world-renderable components. Pure transforms,
logic probes, lights, sky configuration and screen UI do not set that bound.
No finite geometry uses the existing camera reset, rather than a project-sized
world fallback. Keep the bounds/fit/animation/camera owners shared; component
focus approximations are not certified complete dynamic-renderer envelopes.


## Identity
- **Language**: C++20
- **Role**: editor-side scene drawing (overlays, helpers, picking) + asset cache mediating between editor and engine resource system.

## File map
- `jce_editor_scene_render.{h,cpp}` (29 KB) — top-level editor scene render (calls engine render then overlays).
- `jce_scene_render_camera.cpp`             — editor cameras (orbit / fly / focus-on-selection).
- `jce_scene_camera_focus.{h,cpp}`          — pure Unity-style Scene View focus math (AABB fit distance, local→world AABB transform, two-mode repeat-focus close/far toggle, far mode = 2x fit distance, smooth easing).
- `jce_scene_outline_policy.{h,cpp}`        — pure selection-outline fallback policy (visual renderers never use debug placeholder boxes when geometry is missing).
- `jce_scene_content_context.{h,cpp}`       — pure manifest/loose/isolated-Bundle path policy; Bundle Preview never inherits stale host project paths.
- `jce_scene_render_draw.cpp`               — overlay primitives (grid, gizmo, light icons).
- `jce_scene_render_internal.h`             — cross-TU state.
- `jce_editor_dynres_policy.h`              — private bounded adaptive-resolution policy for distinct available latched GPU observations.
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

SceneRender owns one current loose content base supplied by the existing scene
directory setter; its private borrowed getter is shared by GameRender and Play.
The same module's explicit SceneContentContext holds that selected base and the
last applied filesystem, policy and manifest-root witness. Content selection
outlives viewport GPU shutdown/reinitialization; do not put it in the cleared
SceneRenderState or reset it when releasing viewport resources. The directory
setter replaces/clears only the loose base, and content application updates only
the applied-context witness.
The stateless path helper chooses isolated VFS first, a real manifest's declared
asset directories second, then the loose base. A loose base is never installed
as a project/build root. Both viewports use the same Canvas/particle base; same
Canvas setters retain their existing no-change generation guards. This adds no
asset, negative-result or font cache. The setter clears virtual Bundle/Catalog
addresses, and file loaders bind it before deserialization. Render settings for
an explicit loose base omit unrelated project-quality overrides, while manifest,
isolated and packager calls retain their prior precedence.

## Don't
- Don't load assets synchronously on the UI thread — use `jce_thread_pool_*` (engine/include/jce/os/core/jce_thread.h) and show a placeholder.
- Don't keep a second copy of mesh CPU data (it's already in engine's resource layer).
- Don't bypass the cache; direct loads break hot-reload.

## Common tasks
- **Add an overlay** → new draw call in `jce_scene_render_draw.cpp`; toggle via `jce_editor_config`.
- **Support new asset type** → add cache module here + matching engine resource loader; register in `_resolve.cpp`.

## bgfx view ids: the tail band is NOT free real estate (2026-08-29)

`jce_editor_viewport_common.h` derives every viewport pass from a scene-renderer
BASE (`JCE_VIEW_EDITOR_SCENE` = 3, `JCE_EDITOR_GAME_VIEW_BASE` = 120). The engine
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

The current Scene and Game bases are separated: Scene starts at 3, Game at
120. `jce_editor_viewport_common.h` statically checks the point-cube tail against
the other viewport and the global reserved bands; runtime claims still detect
actual collisions. The former base-80 collision is historical, not a current
unfixed defect. This ownership check does not certify rendered shadow pixels.

Script cursors are owned by the runtime host: `JceScriptHost.cursor_set` loads
raw/cooked image assets, caches eight handles, and releases them after scene
script teardown. Native modules must not create cursors through their own SDL
copy. Editor Game View reapplies the selected host cursor only while its
pointer sample is valid, leaving cursor arbitration with the editor host.

Scene texture workers retain `JceTextureCpu` with move-only cancellation ownership.
Render-thread upload consumes the full authored mip chain; RGBA base-mip
decoding is reserved for image previews and never reconstructs scene mips.

Texture resolution may bypass material parsing only for recognized texture asset paths. An existing material JSON inside an isolated bundle is not an image; parse its texture references before CPU decoding. Keep failed CPU decode diagnostics tied to the actual path.

Screenshot submissions in `jce_editor_viewport_common.h` use the core's shared
file-slot lease. Scene/Game completion wrappers identify the submitting viewport;
never bypass this boundary with direct legacy submit/poll calls, because a
backbuffer PNG and a texture capture share the renderer's latest-file result.

Game View mouse capture queries the current public platform window flags before
acquiring or retaining the grab. Missing input focus, hidden/minimized state,
nonfocusable policy or a failed query releases capture through the existing
relative-mode/grab owner; dock focus and creation intent never substitute for
observed host input focus. Releasing capture still works without a flag query.

Asset-index clear, synchronous rebuild and successful asynchronous publication
consume public Canvas resource invalidation. Explicit browser Refresh requests an
index rebuild; its completion allows cached missing fonts to retry. The current
asset browser observes entry changes, not every same-path byte mutation across
an entire project: use explicit Refresh for an otherwise unobserved replacement.

Initialization failure cleanup uses the same Canvas and view-resource release
helpers as normal shutdown. Bridge, camera and scene-renderer failure exits
unwind owners acquired before the failed stage. Initialize every owned overlay
handle to UINT16_MAX before creation, including optional resources: handle zero
is valid. Clear each released handle/pointer; borrowed renderer, shader PAK and
colour program retain their external owners. CPU mutation controls validate
these exact production cleanup blocks; graphical failure paths need host proof.


GameRender owns GPU/view resources, not game-module init/update/exit. Selection
and the admitted module draw delegate to Play; render dt is the last actual Play
step, not a second wall clock. Hidden/clipped views cannot stall module release
or create a second stock ECS update. Script/UI runtime attachment and borrowed
renderer lifetime still follow the existing Stop-before-view-shutdown contract.


Content-context application reports `authored-file`, `quality-only` or `defaults`
through the existing logger after applying the resolved settings. This once per
context diagnostic is available in DIST and never equates the composition's
boolean result with a successfully loaded JSON file. Relative isolated content
is read from the actual admitted active VFS, without a host/CWD fallback; ordinary
HOST projects retain their source-first and cooked-fallback path policy.

Game View presentation extent is resolved through the public render-pipeline
owner before first Play creation and Canvas drawing. Its private extent adapter
is shared by Play admission and the panel; render_frame accepts resolved Canvas/
FBO pixels and must not apply render_scale again. ImGui pointer coordinates are
mapped from the displayed image rectangle into exactly those pixels for both
Canvas and Runtime pointer_position. Runtime viewport_size receives those same
pixels even with no hover or valid pointer; unavailable pointer samples remain
zero. A layout change may synchronously reflow optional on_viewport_resize
handlers through the existing runtime message owner without advancing simulation.
Cached drawable dimensions and ImGui display points are distinct host units;
actual multi-DPI acceptance requires corresponding native evidence.

## Explicit resource preparation

Scene/Game render entry uses the private shared CPU scope and viewport trace
adapter. Existing unavailable/extent/impostor/probe/missing-owner guards retain
their order and emit one final reason for every called owner; early-guard
admission is a separate sample. Target and preparation refusal can follow that
admission. Canonical B/E closure covers CPU return, not GPU completion or a
complete image. Keep per-frame diagnostics in the existing bounded ring, with
no extra frame log or allocation. Probe capture is pumped once before layout
by the admitted host draw through `jce_editor_probe_maintenance.h`. Scene panel
visibility/clipping does not own progress. Positive drawable, current focus and
the prior explicit Scene capture override remain authoritative; CPU services
never pump this GPU work. Impostor keeps priority. A current-UI-frame busy latch
survives completion/failure/cancellation so both views retain modal view ids
until the next pump. Layout-started capture waits for that next pump.

Cancel precedes Scene renderer teardown, whole-scene replacement and accepted
history clear. It changes intent without frame submission; the Engine's shared
readback owner retains pending CPU destinations through actual completion or
backend-stop retirement. Repeated idle cancellation is safe. The Editor adapter
owns CPU latch/request identity, never GPU/CPU readback buffers. CPU controls
do not replace Engine retirement or actual hidden-window capture acceptance.

The same private owner signs successful Editor capture begins with a monotonic
nonzero request token and binds the Scene/entity. Inspector and Reflection
Probes batch panel both enter through it; Editor code must not bypass that
identity authority with a direct Engine begin. A failed begin preserves the
current request. DONE/FAILED, observed inactive capture and owner cancellation
retire it; generation exhaustion refuses before Engine admission. Matching
requires token, Scene/entity and current Engine in-flight state, never merely a
component address. Cancellation/restart within one UI frame keeps its busy latch
but cannot reuse request identity. SDK callers that bypass this Editor owner
are outside its per-request identity observation.

Accepted Scene and Game viewport frames call the public
`jce_scene_renderer_prepare_resources` after their existing editor asset-cache
finalization. Resource preparation and later scene draws share the active-scene
pointer resolved after finalization; an empty scene skips both while preserving
target clearing and editor overlays. Both use the same engine upload owners; the renderer's legacy
fallback deduplicates the same admitted backend resource round and still admits
new decode/video arrivals. Full colour rendering, animation/water clocks, pick,
probe, streamer, camera and cache finalization remain continuous. The legacy
`live_preview` placeholder is not exposed as an effective 5 Hz control: resource
preparation alone is not complete simulation/render maintenance. Real UI/GPU and
performance acceptance remain separate from exact-owner CPU controls.

## Adaptive resolution observation

Scene adaptive resolution consumes the public bounded GPU timing observation,
not legacy converted zero values. The private policy accepts each exact device
context, backend source-frame, raw begin/end and frequency tuple only once;
changing read-time CPU frames does not make a latched sample new. Unavailable
observations clear sustained-load streaks without becoming a light zero-ms
sample. A context change also discards the prior identity. The existing 3/12
sample thresholds, 16/10.4 ms dead zone, 0.1 steps, adaptive 0.55–1 bounds and
fixed/off overrides retain their owners. Streak counters saturate at their
thresholds. The timer covers backend work, not an attributed Scene GPU frame,
presentation or a completion fence. Actual GPU/target-allocation and DWM
acceptance remain separate from exact-owner CPU controls.

## Game preview and Play camera ownership

GameRender retains its stopped preview camera and admits a separate Play camera
through public `jce_camera_clone`; an uninitialized preview uses the existing
default descriptor. Camera-less scenes therefore start from the complete current
preview navigation, while primary/virtual cameras and gameplay input mutate only
the Play object. The borrowed display-camera getter expires at Stop/shutdown.
Play admission, failed creation, Stop/retry and shutdown own allocation/release
independently of Game panel visibility. An active Play frame without an admitted
camera refuses drawing rather than mutating preview. Scene snapshot restoration
remains a separate history owner.

Camera cuts invalidate Game previous-camera matrices and keep zero TAA history
feedback pending until the existing legal PostFX apply call consumes it. Hidden
or skipped views cannot clear the pending cut. This is CPU invocation ownership,
not GPU completion or proof of resolved pixels; bridge/Scene renderer and other
effect settings retain their existing owners.

Scene/Game bridge screenshots query the current admitted allocation through
`jce_offscreen_target_get_extent` at the shared source-selection owner before
narrowing readback dimensions. A missing target refuses the bridge request;
an independent PostFX source retains the caller's existing extent. Projection,
picking, Canvas/input mapping, dynamic-resolution requests and capture-age
admission retain their existing size owners. The Scene/Game cached request
dimensions and Game prepare-time stamp are not complete-frame or GPU witnesses.
Direct PostFX and bridge screenshot requests use the same framebuffer-origin
flip as the panel: top-left sources stay unflipped and bottom-left sources flip.
Source selection and the separate Scene TSR/RCAS presentation target retain
their existing ownership; the screenshot helper does not select that target.
The exact shared screenshot-body CPU fixture covers actual bridge-size routing,
resize, invalid target/narrowing, PostFX separation and unchanged submission
results; its old-requested-extent contract must fail ordinary assertions.

## Scene TSR camera and history admission

SceneRenderState owns the Scene viewport's CPU jitter/previous-camera state;
PostFX remains the sole GPU ping-pong history owner. Reuse needs consecutive
CPU submission indices in the same renderer context, scene, Play ownership,
input/output extent and clean projection. Continuous view motion reprojects
normally. Scene replacement and explicit camera cuts invalidate admission;
closed gates, skipped frames and failed/missing resolves cannot retain it.
Only a valid returned resolve texture commits the next previous camera. A new
history uses the existing TSR feedback-zero and zero-motion path; later legal
continuous submissions keep feedback 0.9. This does not enable TSR, change the
default upscaler, borrow TAA state, own new GPU resources or prove GPU completion.
Camera bookmarks/Align, snap/reset and direct diagnostic vista are cuts; smooth
orbit and focus movement retain camera reprojection. Renderer and Editor CPU
fixtures do not establish dynamic image quality or a hardware TSR run.

The Scene state also owns one existing-Lifecycle registry token. Focus loss or
gain, pause/resume and device loss/reset invalidate CPU admission even if no
backend frame advanced while rendering was suspended. The callback performs
no GPU work. Shutdown takes and clears the token, then unregisters it before
probe cancellation or any owned renderer/resource release. Repeated init is
idempotent; a refused registration logs an error and uses the complete existing
shutdown transaction before returning false. Backend submission continuity is
still a CPU fact, not elapsed-time or GPU-completion evidence.

The shared entity focus-bounds owner restores the current full entity generation
through the existing bare-index bridge. A successful result guarantees finite,
ordered AABBs for both loaded-mesh and fallback branches; Overview consumes this
contract without a second validator. This preserves the existing 32-bit editor
API rather than adding a new identity surface. Recycled slots still follow that
API's documented current-occupant semantics, not stale-handle protection.

## Current Game frame observations

GameRender owns the private current-draw observation in
`core/jce_editor_frame_observation.h`. The core resets it before each layout;
Game policy/request, actual target preparation, resource refusal, real render
tail and Image invocation update separate facts. Only the actual public bridge
extent supplies allocation dimensions. A retained bridge may still be displayed
after a refusal. A PostFX texture with no public extent stays unknown. The
prepare-time screenshot stamp is never a complete-frame witness. Observations
are opt-in CPU facts, not GPU execution, retirement or presentation.

Scene View separates its original bounded wall delta from scene content time.
Navigation and the wall-clock stamp keep updating while Play is paused. The
reflection/main renderer, fullscreen effects and scene Canvas use the accepted
Play render delta outside STOPPED; STOPPED authoring retains wall preview.
Game and Scene therefore forward the same current content delta during Play,
including zero when paused or no step was admitted. This does not skip drawing,
change single-step policy, deduplicate every multi-view renderer subsystem or
certify GPU savings. The shared renderer's existing first-view owners remain.

## Game Stats committed target

GameRender owns the ordinary HUD extent witness separately from optional frame
observation. It requires current renderer-frame extent preparation and completed
CPU draw, resetting completion before every render guard and at init/shutdown.
The internal accessor reads the public offscreen target extent; it never enables
observation, queries GPU statistics or changes capture admission. Game Stats
requests it only for an admitted viewport while Stats is visible, distinguishes
content dimensions from the committed 3D target and labels ImGui cadence UI FPS.
Unavailable, refused and stale targets remain unavailable. CPU completion does
not certify GPU execution, presentation or the extent of an upscaled final image.

## Game View PostFX overlay target

Game View may omit its final PostFX-to-bridge copy for D3D11/top-left only.
`jce_editor_game_postfx_overlay_target` consumes the public versioned target
query plus actual bridge extent/HDR format. Both actual allocations must match
the current render extent, including when two retained stale targets agree.
The output must be current, owned and independent of the bridge texture/FBO;
zero handles are valid. Only screen-overlay Canvas components qualify, with
no active physics debug tail. Unknown, camera/world Canvas, incompatible
format/extent, alias and unsupported backends retain the legacy copy. Canvas
clear NONE and blended color/scissor do not need scene depth/stencil. The
complete Game tail routes Canvas into the admitted FBO and keeps its matching
texture for display/readback until next apply/resize/destruction. Scene View
policy, PostFX allocations, input/focus/event retirement and runtime present
remain unchanged. CPU routing tests are not pixel or performance evidence.
