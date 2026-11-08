The private `jce_sr_ssgi_lifetime` owner retains SSGI through reduced
reflection/probe views. Reduced calls neither advance idle accounting nor
clear any ordinary view's request. Ordinary views OR requests within the same
renderer CPU frame; only a change to the next adjacent frame finalizes the
previous complete observed frame. After 120 consecutive finalized unused
frames, a current unwanted call retires the module; current demand wins first.
Gaps, reversal and uint32 wrap conservatively reset the observation proof.
This is the renderer's CPU/backend submission index, not an Engine timestep,
GPU completion or residency fence. Entity-cache reset clears idle, activity
and result identity while retaining the non-entity-keyed module; renderer
destruction and idle retirement clear before the destroy call. Failed create
keeps activity invalid and resize retains the existing module. SSAO/SSR retain their separate
legacy owners. CPU dependency controls do not prove ES activation, pixels,
driver compilation, physical memory release or whole-frame performance.

The world-cache hit consumers may read cached renderer metadata only while
`sr_ecull_metadata_trusted` proves the current Scene/list/structure/enable/data
cohort; an untrusted full build replays real misses and writes current values.
Both serial hits and the additive parallel static-hit partial fold full AND
static caster keys. Full-build seeds include the existing combined enable/data
cohort so geometry-only component writes invalidate retained shadow content.
`sr_ecull_repair_dirty` owns the existing dirty-ring drain/index repair. Any
old or new static caster conservatively falls through to the original full
rebuild; only dynamic/noncaster repairs may restore frozen static identity.
Pure noncasters do not perturb the frozen full key. Structural, metadata,
layout, scene, overflow and failed index admission retain the full path.
The shadow grid keys the actual static-only/full population mode and exact
frame list generation as well as its matching caster key, structure and count.
Static-only mode uses the static caster key; full mode keeps its any-dynamic
veto. An unsuccessful insert never publishes a valid partial index and uses
the unchanged full-scan fallback. `JCE_DISABLE_SHADOWSPACECACHE` remains an
explicit bypass. Existing `sh_space` phase timing remains the observation
owner; no per-caster log/clock or competing telemetry registry is introduced.
CPU regression ports do not establish ES whole-frame, GPU or pixel gains.
Shadow participation preserves the MeshRenderer cast veto first, then admits
skeletal/terrain geometry and opaque registry membership. Known legacy-only
nongeometry does not enter the caster key, no-AABB list or dynamic caster set.
Zero-flag/post-64 and aliased legacy rows remain conservative; the existing
unified Light serializer alias is represented by its concrete nongeometry
light rows. Registry growth during a presence callback cannot certify absence.
Existing ecull metadata cohorts retain this classification across frozen and
transform-only repairs; do not add a second membership registry or per-cascade
classification. Grass/scatter retain their independent preparation/key owners.

FoliageCluster folds its actual placement/alpha identity and admitted leaf
water-clock time into both pre-CSM caster keys. Hidden, vetoed, zero-leaf and
proxy states do not age only with time. The existing placement hash stays with
its producer and is privately shared; no new transform-dynamic membership or
lower-resolution dual-atlas route is introduced. Local atlas is uncached and
currently lacks a FoliageCluster producer; this change covers CSM only.

# engine/src/middleware/scene — Scene / ECS (L4)

`jce_scene_point_light_write_internal.h` classifies PointLight cull invalidation
before the existing typed set. An existing independent value may change only
intensity without changing the entity-cull generation: selection, gathering and
shadow producers read current parameters. The real ECS set and OnSet still run,
including equal writes and deferred ECS admission. Every other authored field,
non-intensity NaN, mutable storage alias and absent component conservatively keep
the historical bump; membership add/remove retains the central invalidation.
Compare named fields exactly, with signed zero preserved, never struct padding
or epsilon. Only PointLight enables the private prepare/apply hooks; restore
their no-extra-query defaults before declaring other setters. Cull generation
is cache identity, not a setter/OnSet notification counter. No mirror or heap
allocation is introduced. CPU controls do not certify whole-frame/GPU gains.

`jce_sr_colour_special_internal.h` owns only deferred colour submission of
scatter/foliage/grass and custom Line/Trail/Water. The existing main dispatch
captures membership after its unchanged priority/config/component/enable/layer
gates into one private SrEntityCull.colour_special byte. Each late stage clears
only its own bits before producer calls, retaining the other stage's bits.
Every complete first-party colour call consumes all captured marks to zero;
the only early return precedes capture (empty list). Later views cannot replay
marks on FAST/NO_DRAW/layer/enable early returns. The allocator already zeros
newly grown complete cull entries; the main loop adds no preclear read/store and
preserves its invisible FAST two-byte-stream skip. Arbitrary asset-callback
same-renderer recursion or nonlocal longjmp is not a supported recovery path;
the existing renderer already shares mutable queues/caches within that call.
No queue, allocation, new clock, density, shader, cell/range order or world/AABB
policy is introduced. Actual opaque model/primitive/impostor/RQ flushes precede
deferred vegetation, then grass-gate publication, then custom blended producers
and the existing transparent RQ. Each admitted special producer runs once;
admission/resource preparation is not replayed on the later walks. Empty phase
flags return before traversing any entity. A scene with staged producers still
pays the bounded late walks; this is not a claim of zero CPU cost. The new byte's
actual native sizeof/offset/padding is checked and printed by the owner fixture.
Custom transparent producers preserve entity/Line/Trail/Water order, not a global
depth sort. Model-internal transparent primitives and other renderer owners are
outside this change. This fixes submission-stage correctness; pixel equivalence
and performance require independent overlap/motion/GPU and whole-frame evidence.

The empty entity-list return closes SceneDraw::Prepare before returning, just
as the nonempty path does before SubmitLoop. With JCE_TRACY_ENABLED=1 this is
an explicit profile/trace-stack owner boundary, not an automatic C scope exit.
The compiled-off option remains a no-op; this fix is not evidence that a
compiled-off product previously reproduced an unclosed profiler scope.

Probe capture uses the canonical renderer readback queue for matching-format
staging and borrowed CPU destinations. It owns only the completed six-face
output, a queue/ticket and borrowed Scene/renderer identities. Cancel, owner
loss and failure transfer the queue to its existing non-submitting retirement
path; no private bgfx read destination or second completion comparison remains.
Actual source format is observed after offscreen prepare. Authored HDR keeps
RGBA16F when available; RGBA8 fallback remains LDR. Authored LDR from an HDR
source uses the shared half decoder, maps NaN/negative/-Inf to 0, +Inf/values
at least 1 to 255 and rounds finite (0,1) with value*255+0.5. In-place narrowing
has bounded extra face capacity; asynchronous writes never target that output.
The canonical retiring-count admission bounds cancel/restart pressure to four
outstanding retired destinations before normal central progress or backend stop.
This Engine safety contract is independent of Editor visibility/poll scheduling.
Capture uses the ABI-compatible render_linear entry and reduced-pass config.
Its per-call private output override bypasses PBR display gamma/dither, even
when authored tonemap is disabled. The wrapper restores both private output
flags on normal and early returns; it does not mutate Scene/PostFX authoring
or the unversioned public render-config layout.

The private `jce_scene_json_patch_internal.h` owns component patch construction
and the checked recursive merge used by both component and rendering settings.
Registered serializers append independently owned JSON trees; move the first
object out before deleting its array instead of cloning it. Explicit array,
first-object, duplicate, recursive and attachment failures reject/discard the
temporary tree before any ECS parse/set. An absent component still parses its
object with defaults. No production cJSON allocator hook is installed here.
The merge retains an existing same-type primitive when null/bool semantics,
string bytes or numeric integer/double representation match exactly. Signed
zero and adjacent doubles remain distinct; no tolerance comparison is used.
Objects still recurse and arrays still replace. This only elides disposable
leaf duplication/replacement: every accepted generic component patch still runs
its registered parser and ordinary typed setter, including equal-value writes.
The existing void serializer can omit fields on internal allocation failure;
the void parser can fail after entry. These are separate remaining limits,
not covered by the pre-parse rejection guarantee or a transactional-OOM claim.


`jce_scene_line_colors_internal.h` shares the eight existing LineRenderer color
fields between the full parser and the bounded existing-component patch. The
owner uses the public JSON facade for numeric lookup and duplicate-key iteration;
it never includes cJSON or depends on its node layout. Only
unique finite numeric color keys with canonical finite active storage may skip
disposable point JSON construction; unknown/duplicate/empty/nonnumber keys,
invalid point counts and active nonfinite values retain the generic branch.
Registry aliases resolve before admission. Copy the per-call borrowed value,
clear material tail/inactive positions as the full parser does, then invoke the
ordinary typed setter even for equal colors. No component pointer, allocation,
cache, public API or consumer-specific policy is added. Missing components keep
registered parse defaults; inactive nonfinite points are discarded normally.

The pull-based intermediate-target inspector includes SSR only while the
current-frame reflection result is active and its handle is valid. It uses
the existing source texture dimensions and adds no per-frame registry.
RGBA8 intermediate targets need a matching typed readback or public sampling
into a compatible capture target; legacy float capture is not a conversion.

`jce_sr_depth_history.c` shares retained-depth identity with hardware queries.
Hi-Z uses the depth producer's matrix independently of TAA velocity. Camera,
target, scene, component, transform and asset changes reject prior depth for
that render; animated/morphed scene palettes conservatively disable reuse.
The same identity guards static depth-prepass caching and cross-viewport reuse.
The retained prepass also keys whether normal/albedo output was requested:
depth-only instancing leaves those attachments at their clear values. Enabling
reflection/GI after a depth-only frame must rebuild the MRT output, even with
unchanged camera, scene, dimensions and velocity requirements. Primitive
G-buffer submissions bind both roughness and base colour per draw.
Imported normal/albedo prepasses reuse the model velocity traversal with normal
fragment programs and the current camera matrix. The existing skinned vertex
program supplies the skin deformation; depth-only shadow shortcuts are retained
only when no normal consumer is requested. Missing skin shaders retain the
conservative depth fallback and must not be reported as normal fidelity.

Animation instance records use `jce_sr_anim_storage.c/h`: a growing dense index
over separately owned payloads, keyed by full entity generations. Growth keeps
payload pointers valid for deferred parallel sampling. `jce_sr_anim_lifecycle.c`
retires deleted/removed animators and scene swaps, with one complete teardown
before borrowed model skeletons are freed. The sampling request buffer grows
before jobs are dispatched and is reused; no fixed 256-character limit remains.

> The ECS heart: components, prefabs, terrain, virtual cameras, sequencer, LOD, space partition.

## Identity

- **Layer**: L4. C99. Backed by **flecs** (private link).
- **Public umbrella**: `<jce/api_scene.h>` → `<jce/middleware/scene/jce_*.h>`

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_scene.h` | `jce_scene.c` | Scene container, world, root ECS components (Transform, Tag, etc.) |
| `jce_scene_camera.h` | `jce_scene_camera.c` | Strict single-primary camera resolution and world-pose/projection binding to the generic render camera. |
| `jce_scene_components_json.h` | `jce_scene_components_json.c` | JSON (de)serializer for every public component |
| private camera JSON owner | `jce_scene_components_camera.c` | Camera parse/write bodies; registration remains in the central component table |
| `jce_material_override.h` | `jce_material_override.c` | Per-renderer material overrides.  `jce_mesh_renderer_apply_material_pbr()` is the ONLY place the "skip what this renderer claims" rule lives -- runtime loader, editor path-repair and the inspector's material load/reload all call it.  Do not re-inline it: the two loaders each had their own copy and both overwrote all six factors unconditionally, so a per-instance tint on a shared material was discarded on the next open. |
| `jce_scene_fullscreen_effect.h` | `jce_scene_fullscreen_effect.c` + render serializer | Generic full-screen shader component. It is appended to the dense component registry with no legacy flag; renderer consumption stays generic and project equations remain in consumer shaders/scripts. |
| `jce_scene_async.h` | `jce_scene_async.c` | **P3-A.3** Async prefab/scene instantiate. Worker thread (jce_thread) does I/O + JSON parse; main-thread `dispatch_main` (wired to `JCE_PHASE_EARLY_UPDATE`) commits ≤ 1 parsed payload per frame via the same `jce_scene_load_json` helper used by the sync path. Cooperative cancel via atomic flag. |
| `jce_scene_recipe.h` | `jce_scene_recipe.c` | Strict semantic recipe/catalog validation and JSON ingestion. Schema v2 validates stable parent role/instance references and rejects missing, nonguaranteed, self, or cyclic hierarchy before compilation. |
| `jce_scene_compiler.h` | `jce_scene_compiler.c` | Pure deterministic compiler: canonical role ordering, independently derived per-role PCG32 streams, integer placement/quantization, stable entity/parent IDs, graph validation/topology, canonical hash, and FrozenPlan v2 wire format. |
| `jce_scene_transaction.h` | `jce_scene_transaction.c` | Explicit staging ownership and bounded build slices; creates all entities before attaching the existing flecs hierarchy, reads relations back against the plan, validates/prewarms before commit, retains the previous scene through a health frame, and rolls back without mutating prior ownership. |
| `jce_prefab.h` | `jce_prefab.c` | Prefab instantiate / override (sync) |
| `jce_lod.h` | `jce_lod.c` | LOD selection based on screen-space metrics |
| `jce_space_partition.h` | `jce_space_partition.c` | Spatial accel (grid/octree) used for culling + queries |
| `jce_scene_renderer_view_order.h` | `jce_scene_renderer_view_order.c` | Pure helper ordering camera-depth/shadow/compute producer views before the scene color consumer, with finite native view windows and named/filler ownership |
| `jce_sr_depth_prepass.h` | `jce_sr_depth_prepass.c` | Pure policy: who wants the camera depth pre-pass this frame.  Four scene reasons (SSAO / SSR / TAA velocity / water) plus the active pipeline's `depth_prepass` flag, which nothing read until this existed.  Needs no `JceSceneRenderer` and no bgfx, which is what makes it testable; the LOW-tier floor still clears the pipeline flag. |
| `jce_scene_probe_capture.h` | `jce_scene_probe_capture.c` | Captures six 90-degree faces through the ordinary scene renderer and offscreen bridge. The canonical typed readback queue owns staging, borrowed destinations and ready/stop retirement. Cancellation never pumps frames. Prepared source format selects RGBA8/RGBA16F readback; authored output controls bounded LDR narrowing or HDR retention. Modal view-band ownership and host render-admitted polling remain separate from CPU destination lifetime. |
| `jce_scene_component_normalise.h` | `jce_scene_component_normalise.c` | The invariants a CALLER's component must meet before it enters the scene. Today: JceMeshRenderer's seven interned strings are never NULL. Every consumer assumed that and nothing enforced it, so `JceMeshRenderer mr = {0}` through the public setter segfaulted `jce_scene_serial_save` -- twice over (`strlen` on the pointer, and `mr->albedo_tex[0]` guards that dereference before they can decide). Established at the SETTER, not at the serialiser: the same NULL was reachable from a draw. |
| *(in `jce_scene.h`)* | `jce_scene_particles.c` | **P2-particle-vfx-runtime** ParticleEmitter component runtime: lazily owns one `JceParticleSystem` on the scene, loads each component's `*.particles.json` (`asset_path`) into a real emitter (legacy quick-tune fields drive a default when no asset), syncs emitter origin to entity world pos, steps the sim, mark-and-sweeps orphaned emitters. Driven from `jce_runtime.c` after `jce_scene_video_update`. Rendering is owned by `jce_scene_renderer.c` (`sr_draw_particles`, debug-line billboards) so editor + shipping share one render path. `jce_scene_internal.h` exposes the system slot accessor (private). |
| `jce_terrain.h` | `jce_terrain.c` + `jce_scene_terrain_cache.c` | Heightfield terrain algorithms plus the scene-owned loose-file cache bridge; cook tools link only the algorithm TU. |
| `jce_sequencer.h` | `jce_sequencer.c` | Cinematic / cutscene track sequencer |
| `jce_virtual_camera.h` / `jce_vcam_system.h` | `jce_virtual_camera.c` / `jce_vcam_system.c` | Virtual-camera value types compatibility TU + ECS-driven Cinemachine-style resolver |
| `jce_ui_canvas.h` | `jce_ui_canvas.c` | **P1-ecs-ui-canvas** ECS-UI (Unity-UGUI) renderer: layout (RectTransform anchors/pivot/sizeDelta + LayoutGroup H/V/grid) + render (UIImage simple/9-slice + UIText via FreeType/HarfBuzz atlas) + graphic raycaster (UIButton hover/press/click) for Screen-Space Overlay canvases. Draws transient quads into a caller-given bgfx view/FBO. NOT RmlUI (separate shipping HTML/CSS path). RectTransform is embedded in UIImage/UIText (no free component-flag bit). |

## Shared stress fixtures

`jce_sr_gpu_diagnostics.c` exposes the shared color model batch's optional
survivor snapshot through the public renderer wrapper. Shadow/foliage/meshlet
counters remain separate. `jce_sr_batch.c` resolves the static-member culling
diagnostic policy once per color view; no per-instance environment search.
The draw pass clears old color-frustum validity before resolving a new view.

`r.gpu_driven.indirect` selects the existing model GPU batch's indirect or
fixed-count compute submission via the public GPUScene configuration API.
It defaults on and remains independent of hardware/program support. The
renderer reads it before each color batch begins; shadows and foliage retain
their own submission paths. Consumers configure the public console authority,
without benchmark branches in engine code.

`jce_sr_occlusion_history.h` reuses the entity metadata identity and adds
transform/content generations, camera matrices, mask and target identity.
Hardware query history resets through the existing pooled culler before a
changed arrangement is drawn. Unchanged scenes retain query results. This is
conservative: continuously notified movement prevents cross-frame query
rejection, trading savings for immediately correct reveal/camera behavior.
The SDK consumer must not reset the culler itself to conceal a history defect.
`jce_sr_occlusion.c` owns shared portal/query preparation, camera/target binding
and history invalidation. It reads the scene renderer's resolved camera mask;
only the frame authority resolves the authored mask. This extraction keeps the
legacy draw file shrinking without increasing its existing size allowance.

Imported model color submits reuse the ordinary mesh frame-state binder after
their own material bind. Light, IBL, gamma, fog and shadow state must accompany
every primitive, including frames with Forward+ and shadows disabled. Model
instance/crowd groups currently key by model, not receiver light layer; layer 0
can batch, while other layers use the per-entity path to preserve filtering.

`jce_ui_canvas_storage.c/h` owns growing raycast records and hashed per-widget
interaction state. Small canvases allocate on demand; records and state are not
silently truncated at 256 elements. Widget deletion uses the common backward
shift-delete arithmetic and scene changes clear prior interaction state. An
allocation failure logs an error and invalidates input records for that frame.

`jce_scene_stress.c` implements the reusable C99 diagnostic fixture API in
`jce_scene_stress.h`. Editor and SDK consumers share the same primitive/material/
collider definitions and actual-count validation. `jce_scene_stress_update.c`
owns bounded motion with scene-local root, phase and cached query state. Motion
uses unit quaternions and per-entity transform invalidation; it does not pretend
that moving an entity changes the scene's structure. Hosts own cameras, quality,
runtime creation, UI, export and the returned subtree's lifetime.
Scatter diagnostics above one component's limit use independently seeded,
bounded components and report the exact sum. The renderer's component limit
is unchanged; grid side lengths use integer products at perfect-power bounds.

## Benchmark scaling diagnostics

World and focus bounds cache hits require the full JceEntity identity.
Bucket selection can still use the low slot bits. Recycled IDs with the
same low bits cannot consume or overwrite an older generation's bounds.
The table owner preserves probe chains through stale or untouched pruning.
An explicit entity-cache reset clears both tables while retaining capacity.
The opt-in JCE_MODEL_BOUNDS_REPAIR=1 comparison uses a scene-instance
scope and render-thread model events. It updates only the subscribed
bounds rows, preserving the existing transform and caster-key paths.
A geometry event retires retained colour, shadow and depth keys before
the next view. Cache-owner changes do not free queued GPU resources.
An invalid roster falls back to the ordinary full cull path. This route
requires native image and timing qualification before becoming a default.

`jce_sr_ecull.c` owns serial world/bounds cache-miss replay. Its cache bridges
remain private renderer interfaces. `JCE_PERF_ECULL_DETAIL=1` subdivides this
path for diagnosis; leave it off for steady benchmark comparisons because
per-entity timer reads add instrumentation overhead.
Dynamic shadow membership uses a renderer-owned epoch and an ecull-slot stamp:
full builds advance the epoch, incremental repairs preserve it, and allocation
failure disables partial-list shadow optimizations. No scene/project exception
or upstream change is involved.
World-matrix memo lookup precedes local composition and includes shared roots.
Explicit physics/socket writeback notifications invalidate that memo immediately;
callers must not rely on stale same-frame parent/descendant matrices.
World-matrix memo hash chains treat stale epochs as empty. Invalidating a
generation resets its live count in constant time; growth rehashes only current
entries, so streaming/recycled ids do not accumulate and per-body notifications
do not trigger full-table wipes. Epoch wrap alone requires a physical reset.

`jce_sr_ecull.c` owns render-kind classification and dynamic-ancestor detection.
Its transform-independent metadata cache requires an exact scene/list/structure/
enable/component key. Transform writes still rebuild world matrices and bounds;
`JCE_DISABLE_ECULL_METADATA=1` provides the uncached control. Component accessor
membership changes pass through `jce_scene_component_membership.h`: first add
and actual removal advance the structural epoch, while existing value edits
retain their appropriate world/material/cull-data signals. Never cache a
component-existence predicate without verifying these mutation paths.

## Renderer modules (`jce_scene_renderer.c` split, 2026-06-23)

The scene renderer was decomposed from an 11.6k-line god file into cohesive
modules that all share `jce_sr_internal.h` (the `JceSceneRenderer` struct +
`Sr*` helper types + a `Shared internal renderer helpers (cross-module)`
prototype section). Pure move + `static`→external linkage — no behavior change.

| TU | Role |
|----|------|
| `jce_scene_renderer.c` | Core: model/texture/shadergraph caches, mesh resolution, light-dir helpers, material registry, public API, `jce_scene_renderer_render` frame orchestration |
| `jce_sr_internal.h` | Shared renderer state (struct + `Sr*` types + cross-module helper prototypes). **Internal, never installed.** |
| `jce_sr_forwardplus_storage.c/h` | Lazy transactional ownership of the scene's clustered CPU module and parallel light inputs; first requested color view admits a complete owner before collection, OFF retains it, and failed admission rolls back for retry. |
| `jce_sr_batch_tracker.h` | Pure, unit-tested single draw-group detector used to bypass redundant batch sorting |
| `jce_sr_gpu_policy.h` | Pure, unit-tested workload crossover policy for adaptive GPU Scene / MDI dispatch |
| `jce_sr_prim_material_memo.h` | Pure signature contract for the frame-local primitive-instancing material memo |
| `jce_sr_draw.c` | Colour/shadow instancing batches, GPU-driven draw, per-entity model/material draw, entity-render pass |
| `jce_sr_anim.c` | Sprite animator, avatar mask, frame events, IK (two-bone/foot/full-body), ragdoll override, retargeting, skinned-anim eval, and the per-instance morph-VB lifecycle. Owns the animator-SELECTION memo (`sr_asel_n`/`sr_asel_i`), which is why the blendshape resolve LOOP stayed here when the rest of that pass left |
| `jce_sr_morph.c` | The blendshape pass: sample the clip's weight track, deform each morph-bearing primitive, hand the draw path the deformed VB (`sr_morph_vb_cb`). TWO evaluators, one formula -- a compute dispatch where the backend has compute (`middleware/animation/jce_morph_gpu.h`), `jce_morph_apply` on this thread where it does not. The choice is made PER PRIMITIVE and BEFORE the output buffer exists, because a COMPUTE_WRITE buffer cannot be written from the CPU -- see `morph_vb_gpu_mask` |
| `jce_sr_shadow.c` | CSM helpers, directional + local shadow passes, caster cull, shadow spatial grid |
| `jce_sr_shadow_targets.c/h` | Deferred depth-target ownership, resolved cascade growth, transactional texture/FBO pairs and bounded same-size reuse; private immutable provider seam supports actual-owner failure controls. |
| `jce_sr_environment.c` | Sky submission, async IBL bake, water, vegetation scatter, tilemap, cloth, ToD/weather/decals |
| `jce_sr_toon_hair.c` | Per-viewport camera-space hair target, view ordering and scoped direct toon draw |
| `jce_sr_cloud_atlas.c` | Cancellable low-priority cloud-density CPU bake and render-thread GPU publication; keeps the prior resident atlas until replacement succeeds. |
| `jce_sr_cloud_temporal.c` | Physical sky full/half-resolution MRT, per-viewport ping-pong history, opaque cloud composition and current-frame cloud publication for a matching scene and camera. |
| `jce_sr_water_clouds.h` | Private water/cloud binding through stage 8 only when planar reflection is inactive. No texture allocation or independent history. |
| `jce_sr_foliage_cooked.{c,h}` | Validated cooked foliage placement loading from loose assets or PAK, with deterministic scatter fallback. |
| `jce_sr_foliage_buffer.{c,h}` | Private static-roots persistence policy and transactional GPU-buffer ownership, shared by ordinary primitive and non-tiled compute-LOD paths. |
| `jce_sr_fcluster_submit_internal.h` | Whole-cluster transient admission and checked instance copying, sharing the grass receipt shape without changing cluster batching. |
| `jce_sr_foliage_identity.{c,h}` | Pure authored placement and geometry fingerprints used by preparation and shadow cache content identity. |
| `jce_sr_foliage_shadow.c` / `jce_sr_foliage_shadow_key.{c,h}` | Current-root preparation before CSM admission, provider-tested content identity and bounded per-cascade authored-field streaming. |
| `jce_sr_cull.c` | SSAO/velocity depth prepass, light selection, uniform-grid frustum cull, baked-GI consume |
| `jce_sr_velocity_world.c` | Private full-entity world-history table, host-generation snapshots, scene/reset invalidation and probe-chain-preserving retirement. |
| `jce_sr_prepass_targets.{c,h}` + `jce_sr_prepass_target_adapter.c` | Bounded per-view MRT ownership, transactional admission, typed backend ports and current-render borrowed aliases. |
| `jce_sr_terrain.c` | Terrain LOD cache + chunk draw |
| `jce_sr_particles.c` | CPU particle vis + GPU particles |
| `jce_sr_ribbon.{c,h}` | LineRenderer / TrailRenderer camera-facing triangle ribbon, and the `material_path` -> base-colour image resolution it samples. The header holds the two DECIDING halves (material -> image, arc-length U) with no bgfx include, so they are asserted headlessly; the submit half is in the `.c`. |

Shadow target creation follows resolved cfg, live-pipeline and scene cascade
precedence. Construction retains tier defaults and all sampler uniforms without
allocating depth surfaces. Same-size count reductions retain at most four
cascades for shared-viewport reuse; failed pairs leave invalid handles and retry
without orphan textures. A failed directional allocation keeps sampling disabled.
Local tiles publish sampler slots only after successful target configuration;
cube lights require all six faces before publishing their reserved slot.

Static primitive vegetation roots use a per-scatter resident instance buffer
by default, including CPU-culling tiers. `JCE_PERSIST_FOLIAGE=0` preserves the
explicit transient diagnostic; compute culling still requires resident roots.
The existing bounded scatter cache owns these buffers. Identity includes count,
placement hash and compute-read capability. Replacements publish only after CPU
upload acceptance; failed candidates are destroyed once, and retained prior
contents are not bound as the new identity. This is not a GPU completion fence.
Non-tiled CPU count/hash publication follows complete matrix allocation and
fill. Allocation failures invalidate the current count and retry without leaking
temporary masks. Scatter shadow submission requires matching resident identity;
authored `cast_shadow` uses the existing depth path on any tier with valid roots.
Camera-dependent LOD partitions, compute survivor outputs and tiled residency
retain their existing per-view policies. Renderer destruction delegates vegetation
cleanup to the same slot owner used for eviction and scene resets. Meshlet
indirect resources are retired across their own full cache extent, independent
of the smaller vegetation-cache capacity. First-party provider controls verify
ownership and policy; actual two-view GPU images verify capacity and fidelity.
Static roots for visible enabled scatter shadow casters are prepared before CSM
cache admission. Cache identity includes current placement, geometry, count and
preparation readiness; parameter writes cannot reuse an older depth map merely
because the transform generation is unchanged. Each rendered cascade streams
its authored fields through the bounded cache before slot eviction. Failed
preparation/submission prevents that cascade from becoming reusable depth.
Non-tiled model depth uses the renderer's CPU-root traversal so node-local
transforms and all supported primitives survive. Tiled fields keep their existing
view-streamed resident subset and raw tile depth path; resident-subset changes
invalidate retained depth, while tiled node fidelity remains separately gated.
Same-path external placement/mask replacements still require supported explicit
asset/cache invalidation, without runtime file polling.

New renderer code goes in the relevant module; cross-module statics are promoted
into the shared-helpers section of `jce_sr_internal.h`.

## Rules

1. **flecs is private.** Never include `<flecs.h>` from a public `<jce/...>` header. Components in public headers are POD structs; registration is internal.
2. **Add a component checklist**:
   - Define POD struct in `<jce/middleware/scene/jce_scene.h>` (or a focused sub-header).
   - Register in `jce_scene.c`.
   - Add JSON (de)serializer in `jce_scene_components_json.c`.
   - Add editor inspector drawer in `editor/src/panels/jce_panel_inspector.cpp` + reflection in `editor/src/core/jce_reflect_builtin.cpp`.
3. **No game logic.** Components are *data*. Behaviors that act on them live in other middleware (physics, animation, ai) or in the consumer game.
4. **Querying**: provide convenience iterators in public headers if needed; do NOT expose `ecs_query_t*`.
5. **Math** = `jce_math` (column-major mat4 in Transform).
6. **Allocations** via `jce_alloc`.
7. **vcam blending** uses `jce_camera` from renderer for the final transform.
8. **Baked GI consumption (P1-baked-gi-consume)** lives in `jce_scene_renderer.c`. Each frame `sr_gather_baked_gi()` picks the camera-nearest baked `ReflectionProbe` (`baked_cubemap_path` loaded once via `jce__ktx_load_cubemap`, cached on `sr`) and `LightProbeGroup` (`sh9`). `sr_bind_baked_gi()` overrides the IBL sampler stages 6/7 with the probe cube (+ `.irr.ktx` sidecar for diffuse, scaled by `u_giParams.y` intensity) and uploads `u_sh9` (9 vec4) + `u_giParams` to `fs_pbr.sc`. SH9 supplies diffuse ambient; the probe supplies specular. No new sampler stage — it reuses the runtime-IBL bind path. Shader treats unset `u_giParams.y` (bgfx zero-clear) as 1.0 so non-GI draws are unaffected.
9. **Generated-scene determinism** is local and versioned. Never use request
   arrival order, wall-clock time, pointer values, host byte order, or a shared
   cross-role RNG stream as layout entropy.
10. **Generated hierarchy is two-pass and verified.** Create every candidate
    entity first, resolve parents by stable ID second, use only the existing
    scene parent API, and reject any readback mismatch before activation.

## Don't

- Don't bake gameplay rules into terrain/sequencer/vcam — keep them generic.
- Don't add a second ECS or scene graph.
- Don't expose flecs handles to consumers.

## jce_ui_canvas.c: the RectTransform input space is Unity UGUI (2026-08-29)

`uc_resolve_rect` takes UGUI in and produces draw space out, and they are not the
same space:

* IN  — anchors are fractions of the parent from its BOTTOM-left, anchoredPosition
  is pixels with **+Y UP**, pivot names the point of the element's own box that
  anchoredPosition addresses.
* OUT — pixels, origin TOP-left, **+Y DOWN** (the `JCE_VIEW_UI` ortho).

The flip between them was missing: anchor.y and anchoredPosition.y went straight
into the top-left space, so `anchorMin/Max.y = 1` (Unity's TOP) resolved to the
BOTTOM of the screen. All 844 UI rects authored in this tree use a UGUI idiom, so
every authored HUD landed on the wrong edge — in the editor AND in the shipped
runtime. Nothing was authored in the old reading, which is why the repair needed
no content migration.

The stretch branch was wrong for the same reason: Unity's relation is
`size = anchorSpan*parentSize + sizeDelta` with
`rect.min = anchorMin*parentSize + anchoredPosition - sizeDelta*pivot`, so a
negative sizeDelta INSETS. The old code subtracted sizeDelta and ignored the
pivot, growing what Unity shrinks.

Do NOT flip anything downstream. `uc_apply_layout_group`, the ScrollView offset
and `uc_draw_text` all operate on the already-resolved screen rect and are
correct in +Y-down. There is exactly ONE flip and it is the last line of the Y
solve in `uc_resolve_rect`.

`tests/middleware/scene/test_jce_ui_rect_transform.c` pins all of this through
the headless raycaster. Before it, every UI test pinned anchor/pivot to {0,0}
with a full-height rect — the one configuration where both readings agree — so
the suite endorsed the bug instead of catching it.

Sprite lookup (`uc_get_texture`) uses the same three-tier ladder as `uc_get_font`:
project asset root → active VFS → canvas pak. Pak-only meant a project sprite was
looked up in `editor_assets.pak`, missed silently, and drew a flat quad — or, on a
path collision, drew the EDITOR's picture.

## uc_is_ui_element decides who EXISTS, not who draws (2026-08-29)

`uc_layout_draw` only walks INTO children that pass `uc_is_ui_element`.  An
entity that fails it is not merely undrawn: it is not laid out, not raycast,
and NOT RECURSED INTO — its whole subtree of real UI disappears with it.

The predicate listed the eight graphic/widget components and nothing else, so
three shapes were silently deleted:

* an empty node carrying only a **CanvasGroup** — Unity's fade/gate container;
* an empty node carrying only a **LayoutGroup** — Unity's "empty GameObject
  with a Vertical Layout Group holding a column of buttons"; the group never
  ran AND every child vanished;
* an entity carrying only a **UIButton**, which was also the one widget with no
  RectTransform of its own.

All three are offered unrestricted by Add Component and drawn in full by the
Inspector, so they were authorable and inert.  A container with no graphic
resolves a NULL rect and `uc_resolve_rect` substitutes a zeroed one = full
stretch of the parent, which is the container semantic; UIButton now has its
own rect, resolved LAST in `uc_entity_rect` so a sibling UIImage still wins and
no already-authored scene moves.

`uc_collect_canvas_cb` took EVERY Canvas and rendered each against the whole
framebuffer.  A Canvas nested under a UI element therefore escaped its parent's
rect, and if it also carried a graphic it was drawn and raycast TWICE.  It now
skips a Canvas with a Canvas ancestor, which is Unity's rule.

`tests/middleware/scene/test_jce_ui_rect_transform.c` pins both.  Negative
control run: reverting the predicate makes the container test read
`Expected 458 Was 0` — the button under the container is wholly unreachable.

## The reservation table cannot yet catch a MOVED band (2026-08-29)

`JCE_VIEW_SR_OFFSET_RESERVED()` is assembled from the SAME live constants the
order builder uses, so the two cannot disagree about a band's WIDTH — a
negative control that changed `JCE_VIEW_SR_DYN_CSM_COUNT` changed both and the
test stayed green.  `test_jce_view_reservation_table.c` therefore catches a
band added to the builder as a LITERAL (verified: injecting `base+70u` makes it
fail by name) and nothing else.

The fix, unanimous across three independent design reviews, is a FROZEN
append-only reservation table whose constants are independent of the builder's.
Not done.

## The raycast and the draw must derive from the same rect (2026-08-29)

Three defects in `uc_layout_draw` / `uc_update_*` were all the same mistake:
the DRAW side honoured something the RAYCAST side did not.

* A ScrollView clipped its descendants with a view-level bgfx scissor, but the
  hit record was the child's own rect with no intersection against any ancestor
  viewport — a widget scrolled out of view stayed clickable where it is not
  drawn.  The clip is now threaded through the recursion and a fully-clipped
  widget records NO hit.
* An expanded dropdown's popup rows were drawn on top and handled by
  `uc_update_widgets`, but were never in the hit list — and `uc_update_buttons`
  resolves from that list and runs FIRST, so picking an option also pressed
  whatever was underneath.  The popup is now published as a frame-level modal
  rect BEFORE the renderer gate (a headless canvas must block too).
* `UIButton`'s state colours were applied by re-drawing the sibling UIImage, so
  a button whose entity has no image ran the whole click machine and showed no
  feedback.  It tints its own resolved rect when there is no image.

`sv` was also fetched twice with two different enable-gates, so a DISABLED
ScrollView stopped drawing and kept offsetting and clipping.

METHOD NOTE, worth more than any of the fixes: the first version of the popup
test used a click point that was ABOVE the button entirely, and it PASSED with
the modal gate disabled — it asserted nothing.  Only running the negative
control caught it.  The point is now derived from the rects in a comment.  Any
test written against this file should state where its coordinates come from.

## CORRECTION: the ScrollView draw was never clipped (2026-08-29)

The previous entry said the DRAW side "has always clipped" and only the raycast
did not.  That was wrong, and an adversarial sweep of the vendored bgfx proved
it: `bgfx_set_view_scissor` writes a single per-VIEW value that bgfx copies once
per frame at `Context::swap`; the renderer reads one scissor per view and it is
NOT recorded per submit.  This file set it before a ScrollView's subtree and
RESET it to (0,0,0,0) after, in the same frame — so the reset is what reached
the renderer, every frame, and nothing was ever clipped.

`bgfx_set_scissor` is the per-DRAW primitive: consumed by the next submit,
discarded with the rest of the draw state, so it must be re-armed before EVERY
submit.  There is now a file-static current clip and two wrapper macros around
the only two renderer calls the canvas's quads go through, so a new draw site
cannot forget it.  11 call sites route through them.

TEXT IS STILL NOT CLIPPED.  Glyph runs go through `jce_text_draw_*`, which
submits per glyph inside the renderer where this file cannot re-arm the
scissor.  A text row scrolled out of a ScrollView still draws.  The RAYCAST is
clipped either way, so it is not clickable — the half that silently does the
wrong thing is correct, and the half that is merely ugly is written down.

## CORRECTION: base+57 was a new two-owner id (2026-08-29)

Moving the GPU-cull counter reset off base+3 (SSAO's blur) put it on base+57,
and 3 + 57 is 60 — `JCE_VIEW_EDITOR_PREVIEW`.  The commit that removed one
two-owner view created another, against an ABSOLUTE id, which is the one axis
no per-base static_assert reaches.  Now 63 (free at every live base), with 57
kept in the frozen set: append-only earning its keep the first time a band
actually moved.

`tools/audit/check_view_reservations.py` gained the rule that catches it, and
two rules it had been missing — it parsed only two of the contract's four
sections and tested three of the seven fixed ids it records, so half the
contract was decorative.  Negative control: putting the reset back on 57 makes
it fail by name.

## `jce_scene_environment.c` — THE environment driver (2026-09-01)

The optional celestial sidecar belongs to `jce_scene_sky_celestial.c` and
round-trips under scene.rendering.environment.sky.celestial. It is cleared
with scene rendering settings and does not replace the time-of-day directions.

The fog sidecar in jce_scene_fog_look.c round-trips under
scene.rendering.fog.volumeDetail through the existing scene and runtime JSON
bridges. The scene environment tick owns its double-precision wind offset.
The renderer reads the offset and supplies a per-draw default or authored look.

`JceEnvironmentState` was already scene-owned; what was not was the thing that
makes it tick. `jce_environment_advance` had **exactly one caller in the
repository** and it sat in the scene RENDERER (`sr_advance_environment_state`).
A headless build creates no renderer, so on a dedicated server the clock never
moved, `global_wetness` / `snow_amount` — integrators — integrated nothing, and
nothing placed the sun, so `jce_environment_is_daytime()` answered **true
forever**.

Time of day additionally had **two** clocks: the renderer's private
`tod_clock_hour`, and the editor advancing the AUTHORED `tod_hour` in place from
ImGui's frame delta (which also marked the scene dirty every frame, so Ctrl+S
baked the current hour into the saved scene).

Now: this file syncs the authored settings into the environment, unifies the day
clock onto the `world_time_seconds` / `day_fraction` / `seconds_per_day` the
state already carried, places the sun, and advances. `jce_scene_update()` calls
it for the runtime and headless; the editor calls it in edit mode. The renderer
only reads. Guarded by `tools/lint/check_environment_authority.py`.

## 2026-09-01 — reparent 到一个已占用同名的作用域，观察到进程死亡（机制未明）

`jce_scene_create_entity` 只在**创建时的作用域**里去重（`jce_scene.c:1176` 自己的注释写明
「同作用域重名会 ABORT 进程」）。`jce_scene_reparent` 把实体挪进**另一个**作用域时，
不再检查名字。

**已确立的观察**：`tests/application/test_jce_runtime_vehicle_bridge.c` 里四个都叫 "wheel"
的实体挂到同一个底盘上，进程在**第二次** `jce_scene_set_parent` 时死亡（exit 127，无输出）；
改成 wheel0..wheel3 即通过。两个方向各复现两次。

**机制未确立**：`tests/middleware/scene/test_jce_scene_reparent_name_clash.c` 依次加了
①两个同名子 ②四个 ③reparent 后各带 WheelCollider ④父带 BoxCollider + 启用的
VehicleComponent ⑤先建静态 ground，并改为链接完整 `JCE`——**五种都通过**。
所以那个文件不是这个崩溃的回归测试，它是一道**行为守卫**（同名兄弟必须能共享父实体、
两者都仍可寻址、授权名必须保住）。否定结果列在它的头注释里，不要重跑。

下一步应当是拿一个 debug 构建接住那次 abort 读栈，或在 `scene_set_parent_unchecked`
的 `ecs_add_pair` 前加一次 `ecs_lookup_child` 探测把条件缩小。

## 2026-09-01 — 下拉框与输入框已移出 `jce_ui_canvas.c`

该文件曾 3102 行（超 3000 上限、被体量门冻结，以致当天一处七行的注释更正必须压回五行）。
移出 242 行后是 **2864 行——低于上限**。

新增 `jce_ui_canvas_widgets.c` / `.h`。**边界刻意做窄**：3 个绘制原语进、7 个函数出、
1 个矩形类型，外加两个访问器 `uc_renderer()` / `uc_caret()`——
`struct JceUICanvas` 仍然私有，为了两个字段把四十个字段公开出去，
是把边界拓宽而不是收窄。

**没有选那个更大的块**：交互状态机（按钮/滑块/开关）有 555 行、更诱人，
但它需要 `UCFrame`——画布的整个逐帧状态——边界会切在文件内部而不是接缝上。

## 2026-09-06 — 给一个镜头起名字：`jce_vcam_system_set_active_by_name()`

`JceVirtualCameraComponent.vcam_name` 授权、序列化、显示在 VCam Manager 里，而
**`engine/src` 下没有任何东西读它**——选择按 priority，所以名字只是面板上的一个标签。
「切到叫 BossIntro 的那台相机」——过场、触发器、以及**经 SDK 授权场景的 AI CLI**
最需要说的那一句——根本无法表达。

**覆盖住在系统里，不写回组件。** 显然的实现（把选中相机的 `priority` 抬高）会把玩法状态
写进授权场景：编辑器标脏、Ctrl+S 把过场的镜头选择烤进关卡、结束过场还得从别处记起旧数字。
所以它在 `VcamState` **里面**（不是旁边——那会让本文件的文件级可变计数 +1，dedup 棘齿会红）。

三条不变量，改这里之前先读：
1. **被点名的相机不会被复活**：inactive 或组件被禁用的相机不会因为被点名就赢——
   作者取消勾选 Active 是一句话。选择回落到 priority。
2. **记录是无条件的，返回值才是「此刻是否解析得到」**——一个还没流式加载进来的分块里的
   相机，不能因为「现在找不到」就静默变成「按 priority 来」。
3. **`jce_vcam_system_reset()` 必须清掉它**（正因为第 2 条允许点名一台还不存在的相机，
   它绝不能跨场景存活，否则上一关请求的切换会劫持下一关同名的第一台相机）。

## 2026-09-20 — 脚本公开参数：**每一行都把三个值全写出来**，哪怕 kind 只认其中一个

`JceScriptComponent` 现在带 `params[JCE_SCRIPT_PARAM_MAX]`（Unity 的
`[SerializeField]` / Godot 的 `@export`）。一行参数 = 名字 + kind
（number / bool / text / entity）+ **每种 kind 各自的值槽**。

**`ser_script` 无条件写 `number`、`entity`、`text` 三者。**
只写 kind 点名的那一个会通过一次朴素的往返测试（每种 kind 各自留住自己的值），
但会在**唯一要紧的那次编辑**上丢数据：作者在 Inspector 里把一行从 Number 改成 Text
再改回来，原来打的数字就没了。结构体把三个槽分开正是为了这件事不会发生，
序列化器不许把它合回去。

**空数组整个不写。** 没有参数的脚本序列化出的字节与这个特性存在之前**逐字节相同**——
否则全仓库既有场景会集体显示为已修改。`parse_script` 缺 `"params"` 键就是 `param_count = 0`，
而 memset 已经给了这个值，所以「没有」不花任何代价也不表示别的意思。

**ENTITY kind 的那一行是跨实体引用**，走 `jce_scene_components_json.c` 里
loader 的 `src_id -> new_id` 映射，和这里每一条引用一样；解析不到就变 0
（把炮塔指向一块石头，比指向虚无更糟）。它**没有**走 `refs64[]`：那个数组是按
「一组固定的单引用」定尺寸的，而这里的条数是**逐实体数据**。

**两端都夹 `param_count`。** 组件可经 Automation API、经脚本、经手改场景文件到达，
所以序列化端的夹取是必须的——没有它就是从一个八元素数组外面读。
测试断言的是**存盘文本里的行数**（数 `"number"` 键，它是这五个键里唯一没有别的组件在用的），
因为回读端也夹，会诚实地报回一个看上去合法的 8。

**测 `src_id -> new_id` 映射时，不要用 load 路径。** `jce_scene_serial_load` 是**替换**语义
（先销毁调用方的全部实体），所以目标场景在加载开始时是空的、按序列化顺序发 id，
**必然重现源场景的 id**。⇒ 在这条路径上「引用回来仍指向 Hero」这句断言，
**在有没有 remap 两种情况下都成立**，它不判别任何东西——源场景里先造再销毁一批实体去「错开 id」
也没用（实测：Hero 回来还是原 id）。

判别它的是同一份 map 的**另一半**：`jce_prefab_instantiate` 是 **append** 路径，
目标场景里已经有实体，id 从构造上无法对齐。把一个 prefab 实例化**两次**、
断言每个实例的引用指向**它自己那份**拷贝——「map 建一次反复用」与「按源 id 解析」
这两种错法都会让两个实例共用第一份，而这在 load 路径上永远看不见。
（`tests/middleware/scene/test_jce_script_params_roundtrip.c` 两种都有；
这条是变异对照照出来的，不是设计时想到的。）


## 2026-09-21 — 缺的从来不是机器，是**词汇**；以及**链接器报的是设计问题**

`JceSeqPropId` 枚举了十五个属性，**没有一个能指向 UI 元素**，于是
`jce_seq_prop_from_name` 把每一条 UI 轨道都解析成 `JCE_SEQ_PROP_NONE`，
一个菜单淡入只能靠脚本逐帧驱动。而 sequencer 一直在跑、缓动库一直接着、
编辑器的属性选择器**是遍历枚举自己长出来的**（`jce_panel_sequencer.cpp:905`）——
所以补这十一个属性，编辑器**一行都不用改**。

**这一类缺陷不留任何痕迹**：没有红测试、没有红门、没有错的数字。
它只是「这件事说不出来」，于是全树的 UI 动画都是脚本，而没人写下为什么。

**每一个属性的读者都是在加它之前查的，不是之后。**
`cg->alpha` 在 `jce_ui_canvas.c:1489` 被乘进子树继承 alpha；
`uc_resolve_rect` 读 `anchored_position`；`uc_elem_xform` 读 `rotation_deg`/`scale`；
`uc_color` 读两个 tint。**没有读者的属性 = 一条在动一个数字的轨道。**

**`uc_entity_rect` 搬进了 `jce_ui_rect_lookup.c`，是链接器逼的，而它逼得对。**
从 sequencer 调它，把整个 `jce_ui_canvas.c.obj` 拖进了每个链 `jce_scene` 的目标，
于是 `uc_draw_text` 里的 `jce_loc_t` 无法解析：

```
jce_scene.lib(jce_ui_canvas.c.obj) : error LNK2019: unresolved external
    symbol jce_loc_t referenced in function uc_draw_text
```

**静态库按 object 链接**，所以一个五行的查询让调用方付了一整个子系统的钱。
「这个实体带的是哪个 RectTransform」是**关于场景**的问题，却只能从渲染文字的
模块里问出来——修法不是多链一点，是**把问题放到它答案来的地方**。
顺带：那份**组件顺序只有一份**（UIButton 排最后是有代价换来的知识），
抄第二份不会报错，只会**让轨道去动一个没人画的 rect**。

**`transform.scale.uniform` 的不对称在这里重演**：读 x、写两个轴。
写一半不会失败，会把一次等比 pop 播成横向拉伸——看起来像作者键错了轨道。

## 2026-09-21 — `JceRectTransform.scale` 的 0 = **未缩放**，sequencer 不为它开特例

每一个在该字段存在之前序列化的 rect 都把它读成 0，字面 0 会画不出东西——
那看起来像元素消失而不像默认值。所以 **0 表示「未缩放」**。
代价落在作者身上：**键 0 → 1 的 pop 在 t=0 播的是原尺寸**，要从 0.01 起键。

**不在 applier 里翻译它**：一个字段按写它的人不同而有两种含义，
正是这棵树反复付账的那类缺陷。`get_float` 也返回**原始存储值**——
它的职责是编辑器预览的快照/还原，而一个把 0 写回成 1 的还原**屏幕上一模一样、
却悄悄改了场景文件**。**会改变字节的往返不是往返。**


## 2026-09-21 — 检视器：**它要找的缺陷和它自己最坏的缺陷是同一幅画**

场景渲染器手里有十几张中间纹理（深度预通道、normal/albedo/velocity G-buffer、
阴影图与级联图集、天空 LUT），**没有任何东西能看其中任何一张**——
于是一张错的 G-buffer 只能表现为一张错的最终画面，而那是最难查的位置：
下游每一个 pass 都是嫌疑人。

**用「拉」不用「注册表」。** 渲染器自己拥有每一个句柄，所以它直接回答；
推式注册表要每帧付费（不管有没有人在看），而且它是**同一批事实的第二份清单**——
这棵树里任何第二份清单最终都会和第一份不一致。

**有效位就是全部设计。** 每张纹理旁边都已经有一个「这一帧里它是不是真的有像素」的
标志，全部被遵守。**一个过期的目标不会显示空白，它会显示上一帧的内容**，
或者一块充满看起来很合理的噪声的未初始化分配。
⇒ 因此这张表**逐帧改变长度**，这是设计：某一项出现 = 那个特性打开了。

**`ssao_valid` 是陷阱本身。** 它只说渲染目标**已分配**，而该目标是**跨帧保留**的，
所以 SSAO / SSR / TAA 全关之后那个句柄永远有效——`jce_sr_internal.h` 自己的注释
记着只测句柄的代价：**水体对着早已移动过的几何做吸收，而在切换的那一刻毫无症状**。
真正的判据是 `depth_prepass_frame`。

**我自己的测试第一次跑就抓到我自己的缺陷。** velocity 那一行当时没有要求
`depth_prepass_frame`，而是依赖 `jce_sr_cull.c` 里 :656 先于 :710 的顺序——
**那是调用图的性质，不是标志的性质**，在这两行之间加一个 early return
就会让 velocity 宣称一个从未完成的帧。现在那一行**说出它的意思**，而不是依赖别处。

**表可以在无 GPU 下测**，因为 `jce_sr_debug_targets_build` 收的是一个普通 state 结构体
而不是 `JceSceneRenderer`（后者持有 bgfx 资源、单测里造不出来）。
那个 state 是**参数对象，不是第二份目标清单**：表只存在一份。
唯一可能漏的是 `state_from_renderer` 里的逐字段拷贝——所以测试钉了一份
**手写的全量名字清单**：加了一行却没把句柄接出来的目标会停在 `UINT16_MAX`
被静默丢掉，**在编辑器里永远不出现而没有任何东西报错**，金名单就是发现它的那个。

**每条断言都是关于「不该出现」的。** 只测 happy path 的测试，在一张**无视每一个
标志**的表上照样全绿。

## 2026-09-21 — 文件 clip 缓存：**两个生命周期点都要释放，两个解析器都要回退**

`SrAnimInstance` 新增 `file_clips[SR_FILE_CLIP_MAX]`（`<骨架目录>/<名字>.animclip.json`
解析出来的 clip）。这是**唯一真正的风险点**：文件 clip **没有别的 owner**
（模型 clip 归模型所有），所以它必须在**两处**实例生命周期点释放——
`:184` 模型切换 与 `:238` 槽位回收，**就是 retarget map 与 aoc 已经在释放的那两处**。
**往这个结构体里加资源，两处都得加**，否则会在作者想不到的那条路径上泄漏。

**两个解析器都要回退，不能只改一个**：`sr_sm_state_model_clip` 与
`sr_find_model_clip` 都要在模型里找不到名字时落到文件 clip。
只改一个 = 作者在两处填同一个名字、只有一处生效，比完全没有更糟。

**读取必须 PAK 优先、再走 host 路径**，与 `sr_anim_load_sm` 同序。
理由就写在同一个文件里、相隔三百行：只走 `resolve_path->host` 的读法
在单 exe 构建里**什么都读不到**（没有散装 cooked 树）。
第一版就是 host-only，那会做出一个「编辑器里对、出货游戏里静默返回 NULL」的特性。

**miss 也要缓存**：两边都没有的名字否则每帧每实体 stat 一次文件系统。

Normal-only skinned prepasses reuse the existing skin palette walker and shared
current-palette shader body. Their minimal vertex variant uses the view camera
and no previous-pose uniforms or unused clip varyings; velocity retains its
current/previous projection path. Static and skinned normal/albedo fragments
share the encoding. A missing skin program falls back to the existing skinned
depth submit, not a static rig submit.

Global LOD substitution applies only after resolving a mesh. A camera, light
or unresolved entity must never acquire geometry from the global group. LOD
pick counters cover the substituted mesh population, not all visible records.

Scene light collection preserves explicit zero intensity for directional, point,
spot and area components. Nonpositive and nonfinite values contribute zero;
missing-field unit defaults belong to component deserialization, never rendering.
Both selected-light and Forward+ uploads consume the same resolved intensity.
Disabled lights do not occupy selected-light candidates, Forward+ entries or
area/directional upload slots. The primary directional resolver skips disabled
lights in both filtered and legacy scans; time-of-day remains its explicit driver.

Scene Forward+ storage stays absent on default-off, empty, camera-less or
light-environment-less color views. First enabled collection admits the module
and both parallel point/spot arrays transactionally; no collector can write
through an incompletely allocated shading array. OFF disables and retains a
complete owner across sequential views; renderer teardown releases it once.
Allocation failure preserves fixed-light fallback and retries on the next
eligible view. The private immutable provider seam exercises the actual owner
without changing the public Forward+ constructor or pretending to prove GPU
upload success, driver OOM, physical memory residency or rendered fidelity.

Forward+ admission follows actual fragment-family support. Tint, texture-array
and fade aliases remain fixed-light families: primitive and tinted-model batches
fall back to existing material-preserving PBR submission when clustered lighting
is active. Untinted instancing retains its real Forward+ variant. GPU foliage
LOD retains culling, bands and indirect draws but uses hard band transitions for
this unsupported fade/cluster combination. Pixel controls, not requested cvars
or post-render reset flags, establish lighting participation.

Draw-command content identity includes actual Forward+ frame participation,
not just the requested gate. Mode edges rebuild concrete cached PBR program
handles once; steady state retains warm hits. Public SDK bidirectional and
round-trip lighting captures must match independent steady references, with
cache-disabled controls and zero draw-command VERIFY mismatches.

The public debug-target inventory includes active static CSM cascade depth maps
independently of the dynamic atlas. Current shadow binding, valid CSM data and
per-cascade cached contents gate exposure; allocation or zero initialization
alone cannot publish stale depth. Legal texture handle zero remains valid.

Directional receiver depth bias is one texel-space policy in
engine/shaders/pbr/shadow_bias.sh, reused by mesh, terrain and water.
Keep normal-offset bias separate; surface type must not silently multiply
the shared depth offset. Validate short-caster contact and unoccluded
flat/sloped receivers together before declaring pixel acceptance.
Render-queue frame totals reset before shadow passes, then accumulate
shadow and main flushes; resetting at colour-pass entry loses actual work.

## GPU emitter debug targets

`r.particles.debug_stats` is a generic opt-in console setting, default false.
The scene particle driver enables the private allocator diagnostic before its
GPU update; normal operation does not allocate or dispatch diagnostic textures.
The existing public debug-target inspector exposes stable pool-slot names
`particles.gpu_alive_00` through `particles.gpu_alive_63` only for referenced
emitters updated in the current renderer frame. A 1x1 RGBA8 target stores the
alive count in little-endian RGB24 and alpha validity; it is numeric data, not
a colour preview or a CPU estimate. Allocation capacity does not mean alive
particles. Sentinel UINT16_MAX is absent, while texture index zero is valid.
The pure table is bounded by its existing 16-target budget plus the emitter pool
capacity; CPU controls cover frame validity, zero handles and truncated outputs.

Optional particle debug targets expose actual simulation populations, not
rendered counts. CPU totals retain exact zero, reject 24-bit overflow, and
require current renderer frame plus scene identity. Default-off observation
must not alter routing or impose resources on ordinary rendering; diagnostic
textures belong to renderer lifetime and stay absent on unavailable input.

CPU particle diagnostics distinguish simulation alive total from billboards
submitted by the most recent render view. Zero drawn is recorded before early
returns, then replaced only after actual submit; neither means visible pixels.
Both default-off textures share exact encoding, frame/scene validity and
renderer lifetime. Stress witnesses must reject simulation/draw disagreement
and label observer-on frame samples as correctness diagnostics.

CPU billboards consume the renderer-owned CPU instance upload service through
jce_sr_particle_batch.c. Model, queue and billboard uploads share qualified
32/64/80-byte lanes under one 128 MiB logical charge, including candidates and
retired handles. Complete-fit transient admission retains an explicit reserve
for later non-instance submissions; insufficient space uses lazy dynamic
buffers without compute flags. Each view consumes disjoint same-epoch slices.
The particle setting remains an upper-layer caller admission budget, clamped
to the shared ceiling, rather than a separate physical allocation. Failures
retain actual partial submitted counts and rate-limited diagnostics.

The shared provider-driven owner and its tests are graphics-free; scene particle
adapters own no second upload allocator or device buffers. CPU upload acceptance
and deferred destruction do not establish physical residency or GPU completion.

UIButton feedback is drawn at its own graphic hierarchy position, preserving
9-slice and clipping. The interaction pass updates cached state for the next
frame; never redraw buttons over all later siblings or modal panels.

UIButton gestures are Canvas-owned: a down edge captures only the top-most
eligible button, and held motion cannot transfer that press to another button.
Only release over the captured button clicks it. Pointer-invalid intervals,
missing/disabled/alpha-hidden owners, blocked interactivity, skipped canvases and
failed input storage cancel ownership; an unknown pointer interval requires a
valid up sample before another down edge. The existing hit traversal validates
the one owner, without scanning interaction capacity or adding a registry.
Per-widget hover/pressed feedback remains in the existing store. This does not
change Slider/Toggle/Dropdown gesture policy or Editor native-hover admission.
Render-owned click/value polling outputs clear immediately after the non-NULL
Canvas guard, before scene/extent/no-Canvas refusal. A rejected render attempt
must not replay the preceding frame's events. InputField text/submit channels
remain owned by their external input producer and existing clear-on-read API.

Canvas child traversal and equal-sort-order root canvases must use stable entity order. ECS component enable/disable changes migrate archetypes and must not change painter or hit-test ordering; keep the overlapping-button enable-toggle regression.

Cached NO_DRAW admission is presence-based and conservative: only entities
with Transform/EditorMeta/Tag/Layer legacy bits and no zero-flag registry
components may bypass the common draw dispatch. Unknown/custom and all other
components fail open. Structural membership invalidation uses existing scene
mutation helpers/world-cache epochs; enable toggles cannot create admission.
The shared entity list remains intact for light, particle and other passes.

Late private registry append is not assumed to bump scene epochs. Each renderer
snapshots registry count at creation; at render entry a count change first
invalidates existing structural-keyed metadata through the existing scene
world-cache invalidation seam, then latches count. Any mid-render count
mismatch still converts cached NO_DRAW to local OTHER, preserving fail-open
behavior without changing public ABI.

An explicit glTF model claims the render path while its resource is pending. Do not render the MeshRenderer primitive default as a temporary substitute for an authored model.

Explicit model color and shadow submissions request all authored texture maps together and defer both passes until maps are resident. Geometry-only imports must not flash an embedded/default material or an unrelated primitive while textures load.

Canvas image and widget quads whose final packed alpha byte is zero skip their
own texture resolution and draw submission. Do not prune the hierarchy: layout,
raycast and children that ignore parent CanvasGroups remain independently live.
Visible low alpha is retained; do not substitute an arbitrary opacity threshold.

Scene Transform serialization emits the actual quaternion with rotW, matching
the existing Pivot representation. Legacy Euler-only input remains accepted.
The private jce_scene_rotation_internal.h owns the shared normalization policy.
Already unit-length float32 quaternions retain their representation when loaded;
non-unit inputs are normalized. Repeated public scene save/clear/load must not
accumulate Euler conversion drift or alter position/scale. This is the same
serialization consumed by editor history, not a separate Play snapshot format.

The runtime resolved-texture cache owns its uploaded GPU textures. Scene
renderer teardown cancels and releases pending decode jobs, frees remaining
CPU results, destroys completed uploads, and clears cache indices. Textures
returned through external callbacks bypass this cache and remain borrowed.

## Canvas font and text ownership

`jce_ui_canvas_resources.c` owns the shared configuration generation and public
resource-invalidation notification. Each Canvas font cache binds that generation
and its actual VFS identity/policy; changing one viewport's VFS does not globally
clear another viewport's atlas every frame. Cache main and fallback-leaf objects
separately; leaves must not recursively acquire the global fallback chain.
`jce_ui_canvas_text.c` owns production text measurement/submission; rich run
advances and math baselines must use the same fitted scale and formula metrics.
CPU ports prove submission/ownership only; real glyph pixels require a font and
a real backend. Same-path content changes need an explicit owner notification.

## Current grass and shadow transaction

`jce_sr_grass_prepare.c` owns placement/resident geometry;
`jce_sr_grass_shadow_content.c` owns shared production keys and complete current
caster admission. Preparation precedes cache admission. Empty grass is valid;
nonempty failed preparation is not. Stream all authored entities per cascade,
not just the resident-cache slots, and invalidate depth caches after incomplete
submission. Bind the same main camera, time, wind and fade to colour/depth's
stock grass vertex stage. Depth program ownership is private to the scene
renderer, destroyed through the public shader owner. Mask byte reads reuse the
foliage asset-byte owner and obey isolated VFS; missing requested masks cannot
silently become full density. Same-path mask replacement requires cache reset.

Terrain chunk upload converts the public 32-byte JceTerrainVertex into a
48-byte JceMeshVertex through `jce_sr_terrain_vertices.c/.h`; pointer-casting these
layouts is an out-of-bounds upload. Zero initialized tangents delegate generation
to the existing mesh owner. Conversion storage is temporary and released once
on success and failure. Failed replacement retains the owned old mesh but does
not report it as current success. First reason diagnostics and per-failure trace
have different lifetimes; neither proves successful GPU pixels.

## Scene shadow resource ownership

Initialize every lazy-owned bgfx handle to `UINT16_MAX`, including
`cloud_shadow_tex`. A calloc value of zero is a valid backend handle and can
belong to another owner before any scene upload. Release each acquired uniform
reference, including `u_csm_dyn_params`, before the backend shuts down.
`test_jce_shadow_target_lifecycle` checks cold scene destruction against the
actual Noop backend texture/FBO baseline and observes fatal records through
shutdown. These controls establish resource ownership; they do not establish
shadow pixels, GPU completion or physical memory residency.

Lazy water, sky-grading and CPU particle sprite handles follow the same
sentinel and symmetric-release contract, including partial initialization.
Camera-prepass framebuffers borrow their attachments; the scene renderer owns
and releases each texture, including albedo. The private prepass owner retains
up to four complete MRT sets keyed by the exact scene view base. Scene View and
Game View share the Editor's renderer but use distinct view bases and extents.
Same-size interleaving reuses each set; resize/layout changes replace only that
view after complete staged creation. Partial failure retires the candidate once,
preserves the prior set and clears current aliases so no incompatible target is
submitted. At capacity, successful admission retires the least recently used
set; failed admission preserves every resident set. Reset/destroy retire all
sets once. Static prepass content keys include the monotonic target generation.
The prepass color outputs remain normal0/albedo1/velocity2 in both static and
skinned velocity shaders. The velocity-enabled clear explicitly routes three
palette entries to those same slots: normal, black opaque albedo, and exact
float (0.5,0.5,0,1) velocity. RGBA8 128/255 is not neutral after the shader's
`sample*2-1` decode into an RGBA16F motion target. An unused palette index must
never stand in for a live velocity attachment; D3D11 leaves that attachment
uncleared. The no-velocity single-color clear retains its existing behavior.

Velocity world history shares the existing `vel_frame_gen` host authority.
Its private owner keeps current transforms separate from the previous snapshot
latched once per entity/generation, so another viewport cannot replace that
snapshot. The first round of a new full-generation entity has no prior world
in any viewport. Pruning consumes the preceding active round's union before
the next actual velocity prepass and backshifts collided entries without
tombstones or another allocation. Unhosted single-view passes advance the same
existing generation; no additional clock or public API is introduced.
Scene identity changes and the existing entity-cache reset invalidate the table
while retaining reusable storage. Camera-cut colour-history policy remains a
separate owner. Static prepass-cache hits skip table maintenance. Velocity-cache
reuse requires two completed passes at the same scene transform counter, as well
as the existing camera/target/content keys, so stopping an object refreshes its
motion to zero once before continued static reuse. Publication captures the
prior valid scene/view/target/extent/list/VP/layout identity before invalidating
the bounded slot. An unrelated or invalid predecessor leaves previous-counter
eligibility unavailable; equal numeric counters from colliding views are not
stability evidence. This is CPU history/cache
eligibility, not proof of shader execution or complete temporal image quality.
Creation/release CPU spans and correlated owner/view/handle/generation counters
use the existing trace authority. Counted owner controls and real Noop scene
resource baselines do not prove shader execution, GPU completion or pixels.
Each explicit uniform creation
acquires a reference even when shader reflection shares that handle. Keep Web
prepared program identities until `jce_shader_program_destroy`, including the
crowd PBR program; raw bgfx destruction cannot retire prepared source bytes.
The Noop uniform baseline observes used-handle occupancy, not each reference
or successful water, sky, particle or prepass pixels.

Canvas image cache capacity: the existing 64-entry budget includes negative
entries. At capacity existing hits remain usable; new paths reject without IO
or upload. The owner logs path/count/limit/generation and flat-color fallback
once per cache generation, resetting when that generation clears.
tools/tests/test_canvas_texture_budget.py executes actual resource/draw functions
with counted ports and a compiled missing-diagnostic negative. These controls
do not establish project IO, GPU pixels or a larger capacity.

Grass colour admission: jce_sr_grass_submit_internal.h owns requested/accepted/
rejected receipts for visible ranges, separate from authored and culled instances.
`jce_sr_grass_colour.c/h` owns complete colour state and actual range emissions.
The environment owner keeps its visible cell runs, root order and field receipt.
Each resident or transient emission admits instance bytes and geometry before
publishing grass deformation/camera/colours, directional lighting, frame shadows,
cloud, output/fog and the final view mode exactly once, then submits with ALL
discard. Every split and later cell run carries that complete state. Rejection
before emission publishes no uniform updates. The ordinary default PBR material
is unused by this standalone shader and is not bound for grass. Geometry owns
the shared `u_cameraPos` handle with actual camera xyz and w=0; the private
directional light helper omits camera. The existing PBR view-mode entry supplies
the sole `u_normalScale` write, exactly {0,0,mode,0}. No handle-write cache or
discard-policy change masks duplicates. Shadow submissions retain the same
geometry binder and camera-facing deformation. The shared neutral output pack
in `jce_sr_colour_output.h` also initializes both ordinary PBR bind paths; their
IBL-only x/y overrides are unchanged. The existing fog and frame-shadow/cloud
owners remain shared. Capacity
or emission rejection emits its first reason per renderer lifetime with owner,
view, entity and quantities. Scoped per-field counters and FAILED outcomes remain
current after the first WARN. CPU command acceptance does not prove rasterization,
shader fidelity or performance; tests/middleware/scene/test_jce_grass_colour_submit.c
uses counted admission ports. `test_jce_grass_colour_binding.c` compiles the actual
colour/geometry, ordinary/directional lighting and PBR view-mode owners with
typed backend leaves. Named handles alias camera owners; texture binding counts
as a sampler uniform write. Split/resident/rejection and final-value controls,
including zero directional lights and masked shadow-light slots, use CPU state.
Ordinary duplicate-shadow, camera-alias and first-batch-only mutations must fail.
Neither these controls nor their shared scene binder leaves establish GPU pixels.

FoliageCluster colour keeps its whole-cluster transient policy: query the full
requested capacity before allocating, then validate both returned data and count
before copying. The private preparation helper shares grass's receipt shape;
its accepted count describes prepared CPU bytes only. The draw owner records
command acceptance after bgfx_submit, and reports rejected geometry with zero
acceptance. Capacity/null/short-allocation rejection emits one common WARN per
reason per renderer lifetime, with owner/view/entity/program and quantities;
no entity diagnostic registry or global warning flag is added. Scoped counters
and FAILED outcomes remain current after the first WARN. Counted exact-helper
controls cover original whole-cluster refusal, NULL/short allocation, normal
copying and invalid input. The old short-count guard mutation must compile and
fail ordinary assertions using physically safe fixture storage, never crash.
These controls do not certify backend allocation, pixels or GPU completion.
This receipt begins only after the existing visible/transform, program/quad,
cache and nonzero-count preconditions. Those earlier returns remain outside the
receipt and are not evidence of diagnosed shader, mesh or cache readiness.
`LOG_WARN` in the ordinary DIST branch still delegates to common `jce_log_write`
at WARN level; runtime filtering remains authoritative. The separate explicit
host-tool no-logging policy (`JCE_LOG_NONE` in the logger header) may suppress
macros and must not be confused with the shipping DIST policy.

Canvas default font selection consumes shared installed Print, then system Sans,
then the licensed engine/ui Aileron Latin fallback. Only that ordinary default
gains the flat CJK then Symbols leaf chain; explicit component/default paths and
configured fallback lists retain their own priority. Each leaf uses the same
Canvas path/size/SDF cache and resource epoch, while the renderer uses actual
face coverage to choose the shaped glyph's font/atlas. Primary coverage always
wins, and adding a fallback invalidates shaped runs through the existing owner.
Missing CJK/Symbols candidates are diagnosed once per generation and leave the
other fonts usable. No per-frame OS probe or system-font packaging is permitted.
The resource fixture uses counted coverage/leaf ports; real face metadata and
shaping controls are separate from a real GPU font capture.

Shadow resolution/cascade selection owns policy only. `sr_ensure_shadow_map_size`
does not create depth surfaces; a real directional pass must first resolve a
shadow-casting primary light, then admit its complete CSM set. Dynamic atlas
admission occurs before the dual-ready branch and requires dual mode,
per-cascade tracking and nonzero dynamic casters. Failed admission retains the
ordinary static-cascade mover fallback. Same-size existing targets remain cached
across inactive views/frames; a resolution change and normal teardown release
them through the existing depth-pair owner. Inactive local samplers use the
existing white texture when no depth target exists. No resolution, cascade,
shader or budget reduction is implied. Counted CPU and Noop controls prove
admission/ownership; physical memory or pixel improvement needs a real capture.

Static-instance velocity owns its lazy program, tried flag and render queue in
each `JceSceneRenderer`, never in a process-global cache. Initialize the program
to `UINT16_MAX` before any constructor failure can reach destruction. Publish
program and queue together only after both acquisitions succeed; queue failure
retires the new program through `jce_shader_program_destroy`. Release each queue
and program once before backend shutdown, clear their fields, and reset the tried
flag. Scene/Game views and recreated sessions have independent owners. Prepare
and release use common lifecycle scopes; a failed shader/queue attempt is latched
for that owner and keeps the existing per-entity fallback without a per-frame
warning. `test_renderer_owned_cleanup.py` executes the actual prepare/release
bodies with counted ports and a compiled missing-release negative; the existing
`test_jce_shadow_target_lifecycle` observes real Noop cold sentinels, queue release,
scene recreation, resource baselines and fatal-zero shutdown. These controls do
not establish velocity pixels or complete physical GPU allocation attribution;
a real complete allocator trace must independently close every owned identity.

Depth-prepass batching retains its lazy 1024-entry CPU render queue in each
`JceSceneRenderer`. Prepare resets the binder and queued commands through the
existing queue owner; same-owner reuse does not create a lifecycle event. Normal
destruction releases and clears the slot once before backend shutdown. Failed
queue admission preserves the existing per-entity draw fallback and may retry;
other Scene/Game owners remain independent. The constructor's initial cull
buffers all unwind on any allocation failure, including a successful 1024-byte
light buffer followed by failed fast-buffer allocation.
`test_renderer_owned_cleanup.py` executes the actual queue prepare/release, draw
admission, destructor call and constructor CPU prefix against counted ports;
compiled missing-wire and missing-light-release mutants must fail at runtime.
`test_jce_shadow_target_lifecycle` additionally uses the real queue allocator,
Noop renderer destruction and recreation. These controls do not prove prepass
pixels, physical GPU completion or process RSS reclamation.

TAA projection admission synchronizes the applied pipeline through
`jce_scene_renderer_taa_requested` before Scene/standalone begin-frame or the
Game View's independent temporal driver. The private TAA policy-seen state is
separate from shadow/MSAA change latches; unchanged pipeline values retain
explicit console overrides. `jce_postfx_taa_supported` queries the current
PostFX format authority before jitter. Rejected requests leave the clean
projection and do not advance Halton state. These are CPU request/format
observations, not shader/input/allocation or GPU completion acceptance.

Model admission diagnostics live in jce_sr_model_diagnostics.c/.h and walk the
current capacity of the grow-on-demand stable-entry pointer table only for the
first WARN or first captured detailed witness for a reason/identity. NULL buckets
are unoccupied; loaded, pending, failed and otherwise unavailable admitted entries
consume capacity. Detail schema 2 reports current limit and separate max_limit;
summary schema 1 is unchanged. The public resident count retains loaded semantics.
Saturation preserves hits and rejects new paths before IO. The actual lookup
and async completion pass their existing path hash to the private hashed entry;
no repeated authored-path hashing, IO or per-path registry is added there.

The existing renderer owns eight fixed reason counters. Captured repeats only
update checked uint64 counts and first/last entity-index/path-hash witnesses.
WARN remains once per reason/cache generation, independently of whether tracing
was enabled then. The first captured FAILED rejection keeps the occupancy
snapshot. Repeats use schema-1 reject_summary packets: exact pending/total counts,
reason, owner, cache/capture generation, scene/context, backend CPU round and
flush trigger. Wide counts and scene/context identities use low/high fields.
A saturated count is an explicit lower bound, count_complete=0, never wrapped.

The real admitted resource-prepare owner binds before polling completed models.
A changed backend CPU round flushes pending counts once; equality handles frame
zero and wrap without an ordering guess. Scene/context/cache/capture changes
close the old batch and rearm its detailed witness. Cache reset and renderer
destroy flush the final pending batch. Recent bindings and unpublished batches
have separate fixed keys, so disabled tracing cannot relabel an old batch.
After capture restart, a carried summary retains its old capture generation,
publishes current_capture=0 and never joins the new capture's request total.
A suppressed/disabled writer retains pending counts; owner destruction explicitly
WARNs when those observations remain unpublished. No counter is manufactured
for requests made while tracing is unavailable.

per_request_details=0 and detail_budget=1 declare first-witness coverage, not a
complete path roster or per-API-call history. Summaries do not replace actual
resource/job creation, release or application lifecycle events. These records
describe CPU rejection admission; they neither acknowledge GPU completion nor
fix an exhausted cache, missing models, rendered pixels or whole-frame speed.

Grass shader failure is latched until the existing explicit entity-cache reset.
That reset clears its reason diagnostics and admits one new colour/depth attempt
in the new generation. Valid prepared programs and uniforms remain resident;
invalid or failed deferred programs release through the shared shader owner
before their tried flag is cleared. Missing assets cannot trigger shader IO or
WARN every frame. Actual program admission retains owner, submitted entity,
colour/depth program, generation and requested shadow participation; a FAILED
record does not certify rasterization or shader fidelity. Same-path shader/mask
replacement still requires an explicit reset; no VFS polling is added.

The JCE_GRASS_MASK_BATCH=1 opt-in reuses a successful mask grid
only within one synchronous opaque colour batch. The existing bounded resolver
still runs for each request. Actual encoded bytes, path, active FS and policy,
and the threshold must match. Failed replacements keep the previous entry
without serving it for the failed request. Local and borrowed grids have
separate release ownership; the batch retires its single entry before return.
Absent or unrecognized selectors are off. CPU decode/grid scopes and their
outcomes do not acknowledge GPU completion or pixel equivalence.

Early colour grass gate evidence uses the renderer-owned eight-view
`SrGrassGateTrace`, separate from lifetime `grass_gate_reported_reasons` logs.
The unchanged GrassField request body lives in `jce_sr_grass_gate.c`; the oversized
draw TU retains its original opaque/has-component branch and delegates only an
actual request. Pass-local summaries count requests reaching that branch,
actual admission, overlapping kill/component/caps/tier/project reasons and
otherwise unobserved refusal. Earlier layer/kind/entity filtering is outside
coverage; first entity values are witnesses, never a complete entity list.
Snapshots compare every scalar and array field without hash-based suppression.
The current trace generation and existing grass resource generation rebaseline
the eight view slots; unchanged passes emit nothing. Disabled/suppressed tracing
or a refused scope does not consume pending evidence. A ninth distinct view
publishes one FAILED overflow with coverage_truncated=1 and covered_views=8,
then remains outside coverage until the next epoch; slots are not rotated to
create per-frame flooding. Empty-list and normal loop tails share the actual
publication helper. This adds no loading, allocation, admission rule or log;
trace-off performs no snapshot initialization, collection or cache scan. Records
describe CPU branch admission, not GPU completion, pixels or all-view coverage.

Scene video phases: `jce_scene_video_advance` opens and advances the decoder
without texture creation, pixel upload or draw submission. The old combined
`jce_scene_video_update` delegates to that owner and private render preparation.
`jce_scene_renderer_render` prepares only after valid renderer/scene admission;
the published decoder counter makes subsequent views idempotent. A stopped
one-shot's final published frame remains eligible. Failed create/replacement
keeps the old output and its counter retryable. Existing path-change and ECS
move/copy/dtor ownership still retires resources once; this release command is
not a GPU fence. Scoped advance/prepare/retire records retain decoder, component,
texture and published-counter identities without a new registry or per-frame INFO.
`test_jce_scene_video_phases` compiles the full production video owner, real public
scene/ECS operations and counted decoder/texture/byte ports. Its strict renderer
admission-prefix extract proves this wire only; no later render pass is certified.
Same-owner old CPU-upload, playing-gate, failed-counter and missing-render-wire
controls must fail ordinary assertions. Native background behavior, decoder
quality, rendered video pixels and GPU completion need separate acceptance.

## Scene resource preparation boundary

`jce_scene_renderer_prepare_resources` reuses the actual scene-video upload and
async texture/model poll owners on the render-owner thread. Its private key is
scene, device context generation, backend CPU frame index and model-cache
generation; reset invalidates it. New terminal decode jobs are observed once
per job so arrivals within that round invalidate reuse without repeatedly
polling budget-blocked models. The existing one-model-per-backend-frame budget
remains; context changes rearm its context-local stamp. Video uses the same
pending counter predicate as its upload owner, including stopped final frames
and retryable upload failures. No worker touches host observation flags.

Preparation permits resource create/update/retirement and emits the existing
scoped `scene.resources.prepare` phase with backend/context/reuse facts. It
submits no scene colour, shadow or compute pass, advances no dt/simulation,
and neither proves GPU completion nor rebuilds resources after context loss.
Legacy render calls it; model reference clock and eviction remain per complete
render. Skinned/sprite animation, bones, water/fade and other pass maintenance
remain render-owned: a host must continue complete rendering until these have
an independent shared advance owner. Do not advertise 5 Hz live preview yet.

## Skeletal multi-viewport membership

`sr_update_skinned_anims` retains the existing velocity-generation guard and
SimLOD camera policy, but all six pose passes use a short scene-wide animator
snapshot from the existing component enumeration. A first viewport's focus
cull cannot define which actors exist for later viewports. The snapshot uses
authored scene order, so event dispatch remains serial and stable after pose
sampling; event-added components join the next snapshot. Existing has/get
checks still handle event removals before subsequent pose passes.

Requests and full-generation entity IDs share the existing per-renderer
`anim_requests` allocation, with checked combined capacity and disjoint ranges.
Growth finishes before sampling. Allocation/snapshot failure does not stamp the
velocity generation, so a later viewport can retry without duplicated time.
Scene transitions invalidate the old accepted generation before prune publishes
new ownership; a same-round OOM cannot borrow the previous scene's acceptance.
The former scene-wide hash/collect-index memo is unnecessary;
`JCE_ANIM_ASEL_VERIFY` validates the actual snapshot instead. Legacy callers
without velocity-frame driving retain per-render timing. This fixes membership,
not the simulation-camera policy or all CPU/GPU lifecycle separation; morph,
palettes and other render-owned maintenance stay in their existing phases.

The complete pure view-order owner reserves the existing camera-depth base+1
slot before color base+0 even with every optional band disabled: a pipeline-only
depth_prepass has no separate argument in this helper. Reserving the slot does
not submit/touch an inactive pass. Geometry-only depth/normal/velocity shaders
read geometry, material constants and current/prior matrices, not current lit
color. Existing visibility-compute/shadow dependencies and post-color SSAO,
SSR/SSGI, fog, underwater, camera overlays and reader bands retain their order.
Native remap IDs and windows must remain within 0..255, independently of the
128-entry array capacity; reject overflowing contiguous and sparse bands.
CPU permutation tests and complete-owner old_order/old_boundary controls prove
those scheduling contracts, not physical GPU completion or a frame-time gain.

Grass and foliage lazy program owners use the renderer's private startup-root
loader for their existing colour/depth pairs, including grass/grass_shadow.
Their current renderer supplies only the explicit copied development root;
there is no environment read, implicit active-VFS shader lookup or ShaderManager
lease in Scene. Missing stages retain existing per-stage PAK/embedded fallback;
present invalid stages fail closed. Lazy tried flags, deferred resolution and
creator reference destruction retain their existing owner. Existing valid
programs are not invalidated by this startup-only wire; a new renderer/process
is required to consume a changed root. Other independent shader families and
full hot reload remain outside this change.

Camera prepass storage follows explicit per-call color demand. The private
request's appended prefer_depth_only bit defaults false, preserving legacy
zero-initialized callers. Only a view requesting neither normals (SSR, SSGI or
planar composite) nor velocity prefers one depth attachment. Velocity keeps the
existing normal0/albedo1/velocity2 layout and precision. Shader selection,
geometry, alpha/deformation, camera matrices and draw order do not change.
The backend adapter validates only the new one-attachment layout before create;
legacy3/4 attachment forwarding is unchanged. A provider refusal can also mean
allocation failure, not merely unsupported hardware. The unpublished staged
depth remains separately owned while trying the legacy MRT fallback. A total
failure retires all staged resources and leaves the prior target owned but
unpublished. Successful fallback keeps the preferred request key, avoiding
repeat creation on each frame. Actual allocated handles select color clearing.
Mode and format join the existing per-view reuse key; replacement generation
invalidates depth/velocity history through existing owners. Reset, capacity
retirement and teardown preserve their current ownership order. Existing target
trace packets expose actual plane handles and generation; nominal storage/count
controls are not backend memory, GPU execution, pixel or whole-frame evidence.

The sky admission gate belongs to `sr_draw_sky_pass` in the existing environment
owner. It preserves the former caller's sky_drawn result even when program or
transient admission fails; diagnostics do not change rendering policy. The
existing native device resource-DIAG latch plus Core trace enable a bounded
`sr.sky.admission` service for each attempted sky pass, with balanced policy
skips, resource refusals and CPU submit-return outcomes. Context generation is
split into exact 32-bit counters; capture generation, backend CPU frame, view
and viewport qualify the observation without pointer identities. Raw program
index admission tests only the invalid sentinel, never backend liveness.
Uniform counters retain float bits from the actual uploaded sky mode/exposure
and gradient colors. The base caller-enabled bit does not pretend to be the
overlay-only camera_clear_mode field. No resource resolve, new environment
selector, INFO log, cache reset or GPU-completion assertion is introduced.
Diagnostic trace traffic is observer work, not an ordinary-performance baseline.

Desktop GL 2.1 stock sky keeps gradient, HDR and authored stylized modes.
Physical atmosphere uses the analytic sky at the existing physical exposure
scale. `jce_sr_cloud_atlas.c` reuses the async density producer and uploads
RGBA8 on this profile. The reduced sky uses two bounded slab samples and
skips physical atmosphere LUTs. Grass and foliage retain existing ground
cloud shadows.

On desktop GL 2.1 without instancing, `jce_sr_grass_expanded.c` owns bounded
static grass geometry and per-cell ranges. Foliage uses cached expanded cards
and CPU particles use bounded transient quads. Water keeps the authored
stylized data mask and Gerstner waves. Legacy FFT draws sample the existing
scene water field within a per-view vertex allowance. The physical material
uses analytic reflection tint and sunlight. A refused CPU mesh uses analytic
water, and the draw omits depth, planar reflection and vertex texture reads.
Terrain retains its heightfield, chunk LOD and base PBR layer whenever the
layered terrain program is unavailable.

Colour-pass preparation updates time-of-day/camera CPU inputs without recording
light or GI uniforms ahead of admitted draws. Existing material/model callbacks
own those inputs per submission. This does not change the separate legacy
centroid GI uploads or certify their single-write ownership.

jce_sr_fcluster_colour.c/h owns whole-cluster instance/geometry admission and
complete foliage-colour emission. Refusal precedes uniform/texture publication.
The actual caller camera reaches the private producer; camera, fog, view mode,
cloud and canonical CSM splits are published for each accepted submit. The
environment owner retains resource/cache lookup and scoped receipt publication.
The foliage shader's existing output formula is unchanged. Instance admission
and CPU submit-return are not GPU completion or pixel acceptance.

jce_sr_shadow_frame.c/h owns the existing frame-active/r.shadows/CSM selection
and CSM split packing. The full shadow-state binder and split-only foliage
consumer reuse this owner; split-only binding does not bind shadow textures or
PCF constants. Returned CSM pointers are per-call borrows with existing lifetime.
The GPU foliage model-LOD adapter leaves per-band material and frame binding
to the actual model owner and armed callback, avoiding a default prebind.

Water colour retains its initial default-PBR/global material and frame binder.
The later shadow/view-mode rebinds are redundant and removed; the initial
normal-scale tuple already has the same four lanes and current view-mode z.
Geometry handle refusal precedes that first draw-state publication. Existing
water simulation, resource preparation, cloud/depth/planar bindings and output
shader remain unchanged. This is binding ownership, not an algorithm speed claim.

## XP Flecs OS capability bridge

`jce_scene_flecs_os.c/.h` supplies Core-backed OS callbacks only when
`FLECS_NO_OS_API_IMPL` excludes Flecs' default provider. The ordinary modern
path returns true without table access. Preparation precedes the first Flecs
OS use: an allocation-free registration word claims installation, official
defaults/get/set preserve logging, abort, module and lifetime callbacks, and
readback verifies the replacement before release publication. In this opt-out
path, contention, foreign callbacks, refused installation and
`FLECS_TRACK_OS_ALLOC` fail closed. The installed table persists across world
lifetimes; no global heap or mutex owner is added, and concurrent world
creation is not made thread-safe.

Heap callbacks retain Flecs attempt counters and reject nonpositive sizes.
Thread tickets preserve the callback result, roll back failed creation and
free only after Core join has released its thread. Mutex/condition operations,
string duplication, nanosecond time and bounded rounded-up sleeps reuse Core.
The official time getter is preserved through an installer-local pointer.
`tests/compatibility/xp_flecs/bridge_control.c` consumes the complete bridge and
real headers through typed Flecs/Core ports and ordinary failure controls.
Those CPU controls do not qualify a real Flecs world, SDL thread or XP Editor.

Authored decal pool admission belongs to `sr_drive_decals`: zero authored
components never create a pool; existing authored pools clear stale stamps.
The per-SR `decals_authored_tried` latch records one actual public create
attempt, including failure, without blocking runtime-stamped updates/rendering.
`jce_scene_renderer_reset_entity_caches` clears this latch at the existing
explicit scene/rebuild boundary; a successful pool is retained, while a failed
owner may try once on subsequent authored demand. There is no per-frame retry
or automatic recovery claim. Scene code consumes public create's NULL outcome
and does not duplicate the renderer's private compatibility profile policy.

Built-in pick program acquisition uses the public compatibility policy helper.
Compatibility GL/ES uses the static ID program on CPU-skinned geometry. Colour,
shadow, wireframe and picking use the same per-entity buffers prepared in
`jce_sr_morph.c` after IK and the final ragdoll pose. Morph deltas are applied
before skinning. The renderer checks scene, entity and model identity before
borrowing a pose, and the ID pass rebinds its ID on each primitive. It omits the
rest-pose frustum test for active deformation. GPU ID picking still requires
the existing blit/readback capabilities.

The no-readback click query borrows the same scene model and matching
animation instance. It tests retained CPU triangles against a finite world
ray, and an exact miss cannot be replaced by a transform box. The model owns
its pick data. The ray query reapplies the current morph and skin state on
demand, without a second persistent posed vertex buffer per entity.

Normal G-buffer acquisition belongs to its complete local helper in
`jce_sr_cull.c`: only first actual normal demand attempts acquisition. A
compatibility refusal retains INVALID and the existing depth-only fallback;
its existing attempted flag prevents repeated profile queries or loads.

WaterSpectrum is an optional versioned component in the existing ECS registry,
not an expansion of the unversioned Water ABI. Its append-only registry row
preserves old dense IDs. Water retains wind/seed/fetch/swell ownership; PM/TMA
and directionality are configured before FFT evolution. Shared scene acquisition
uses the extension only for enabled FFT bodies; absent, disabled and non-FFT
bodies keep legacy selection. WaterField rebuilds commit only complete resources
and preserve borrowed field addresses and the single scene clock. CPU regression
coverage is not proof of current GPU pixels or low-end visual acceptance.

Water gradient mips are optional WaterSpectrum shading state. jce_water_gradient_gpu.c owns only RGBA16F textures and CPU packing scratch, keyed by the shared field identity/revision; invalid or incomplete uploads cannot enable filtered shading. Slot eviction/reset/destruction releases that owner through sr_water_slot_free. sr_water_destroy owns the moved complete water teardown. Sampler 13 is ripple (distinct from depth 2); 14 is the filtered spectral gradients. Geometry/physics still consume the full-resolution field. The pure packing and typed bgfx-port controls do not establish native shader/pixel acceptance.

WaterFoam is an optional versioned component; its history lives in the existing
WaterFieldSet slots alongside, never inside, absolute-time ambient WaterField.
Only the admitted set driver advances its bounded birth/decay state. Lattice
changes and rewind clear history; same-lattice spectrum edits preserve it.
Generation identity prevents stale renderer reuse after recreation. Removal and
slot sweep release history. jce_water_foam_gpu.c owns only upload scratch and
RGBA16F mips, with density in RG and stochastic detail in B. The FFT foam path
shares stage4 exclusively with stylized distance data, preserves Jacobian alpha,
and falls back to instantaneous foam on refusal/GL2.1. Private tests do not
certify native pixels, temporal convergence or shoreline-flow simulation.

Water component defaults and shared field/foam authoring resolution live in
jce_scene_water.c. Scene ECS storage/accessors remain in jce_scene.c; this
split preserves the existing file-size budget without changing ownership.

WaterCrest uses the existing scene registry for authoring. The water renderer binds one two-vector constant per modern water draw and reuses the still-water plane, directional light and CSM/cloud shadow. Missing or invalid authoring binds zero strength.

The RGB caustic atlas owns its padded mesh, RGBA16F target and uniforms per viewport. It submits one signed-offset draw per channel into a bounded tile and reserves base+69 before scene colour. Consumers sample opaque receiver world XZ before water absorption.

### Terrain-driven nearshore

WaterNearshore is a versioned optional link from an FFT Water entity to a
terrain entity. The scene resolver samples an exact terrain-cache revision and
world transform into a complete bed relative to the water still plane. Explicit
regions wider than 32 metres use 64x64 cells; smaller regions retain 32x32,
including the existing 16-metre default. No public component layout changes.
The shared terrain projection in jce_terrain_surface.c retains the legacy
foliage/grass queries. Nearshore admits resident monolithic heightfields and
rejects missing coverage, sampled cut cells, singular transforms and bed
heights outside its bounded domain. The solver retains no terrain borrow.
The WaterFieldSet admits at most four flow/foam states within a total 4096-cell
budget. One 64x64 region consumes the same budget as four legacy 32x32 regions;
refusal does not silently lower the requested resolution. It advances them
on its existing FFT clock. A failed step does not publish partially updated
flow or foam. Missing, invalid and retired beds select the existing water path.
The scene surface query and renderer share the depth-based coverage rule.
`jce_water_shore_atlas.h` owns the pure stage-13 packer and relative
render-height extension shared by the atlas and CPU surface query. Dry bed
height is not a free surface: the one-cell dry border extends adjacent wet
height before filtering, while depth, foam, momentum and the solver bed remain
unchanged. The existing still-plane transform supplies world height for water
VS/PS and rain-surface consumers; no zero world elevation is assumed.
Actual packer regressions cover dry/wet bilinear filtering and padded flow.

WaterMotion resolves through the same scene field acquisition as WaterSpectrum.
The field owns the phase anchor independently of its real scene time. A live
rate edit preserves the current phase, while foam and shallow-water state
step on the real clock. Horizontal scaling and the radial cap apply to the
packed displacement before the wrapped Jacobian. Two bands split a positive
cap and use one inverse of their summed world mapping for scene samples.

The water/cloud handoff rejects unpublished, stale-frame, different-scene,
different-camera and changed clean-projection inputs. Prepare and bind invalidate
the viewport publication before work; only a successful current submit publishes.
Modern water may borrow that texture when its camera is above the cloud base,
opaque depth is available and the sky grade has no bands or varying palette.
The shader proves the entire cloud-shell interval precedes its actual surface;
a representative cloud distance is not sufficient. Planar reflections and
surfaces crossing the cloud retain their original water path. Full resolution
uses the same producer at the viewport size. Its opaque composite is ordered
after the water optics trace and before transparency, so reflection/refraction
reads the original opaque radiance. Only a submitted current-view composite
marks opaque backgrounds as already containing clouds; a missing pass retains
the depth-specific water alpha fallback. No additional render target is used.

Modern opted-in, unscaled FFT oceans use `jce_sr_water_mesh.h` for a 96-ring,
192-segment surface (18433 vertices, 36672 triangles). The shared ring factory
admits at most 128 rings, 512 segments and 65535 total vertices, preserving
16-bit addressability and all old 64-ring configurations. The regression calls
that actual factory with the renderer constants, not just its radius helper.
Linear near spacing extends
to 48 metres, followed by an exponential interval with the same initial radial
derivative. The former 32-to-43-metre ring jump is absent. FFT resolution,
physics fields, transformed/bounded water and compatibility meshes are unchanged.
The stage-13 atlas carries a padded same-revision velocity tile below the shore
height/depth/foam tile; finite shallow-water momenta divided by wet depth supply
metres-per-second flow. Dry cells supply zero, speed is capped at 8 m/s, and
legacy programs ignore the added tile. Density and velocity share one upload.


Modern physical cumulus on high/ultra uses a split density representation.
The asynchronous base producer stores shape and precipitation optical gain in
128x32x128 RG16F (2 MiB), while a fixed 64x64x64 R32F erosion field (1 MiB)
resolves a 512-metre periodic volume independently of the layer height. The
first compatible job bakes detail once; coverage, curl, erosion and layer
changes reuse its resident texture. Seed, detail frequency and octave policy
are currently renderer constants; exposing them requires a detail cache key.
A low-tier renderer starts with the complete-density 256-KiB atlas. A renderer
that has already used the split path retains its 1-MiB detail cache until
destruction so a later tier increase does not rebake it.

Generation and representation guards reject stale asynchronous publications.
Unsupported formats, failed allocation/bake/upload or incomplete uniforms
select the complete-density fallback. Modern camera and light rays consume
that complete density directly, without a second erosion or daylight gain.
All modern paths and CPU ground shadows share the same local-height metric,
coverage carve, core-preserving erosion, optical gain and extinction default.
Authored zero extinction resolves to 1/km without rewriting authoring data.

Cloud detail binds stage 4. Temporal previous color binds stage 7 for its draw;
resolved cloud replaces stage 7 only after that producer submit for sky/opaque
consumers. Stage 5 remains reserved. The opaque pass does not acquire a new
texture. Detail bake-complete and resident-handle logs describe distinct CPU
producer/upload events and never claim GPU completion. Diagnostic native
fixtures must pin the exact shaders and verify current parameters, clean
camera projection and atlas generation before using an image as evidence.

WaterNormalDetail reuses the versioned water ECS accessors and append-only dense
component registry (legacy flag zero). jce_water_normal_detail_config.h owns the
finite range/version admission. jce_scene_components_water_normal_detail.c owns
defaults and the frequencyLow/frequencyHigh/strength JSON adapters; absent fields
retain defaults, invalid supplied values reject the complete component write.
Frequencies are independent, not sorted. No additional simulation owner or clock
is introduced by authoring. Renderer phase/resource integration belongs to the
existing water draw and field owners; scene serialization is not GPU evidence.

Camera JSON parse_camera/ser_camera/serw_camera moved intact to the focused
component camera TU. Only parse_camera and serw_camera gain private external
linkage for the existing central registry; helpers and wire values are unchanged.
This keeps the registry audit visible without growing the oversized central TU.

`jce_water_field_phase` in the private field header exposes the already evaluated
spectral phase, including phase-preserving WaterMotion rate changes and rate-zero
freeze. It never accumulates a renderer clock and does not replace scene time
used by rain, history or Gerstner waves. `jce_sr_environment.c` transports optional
WaterNormalDetail through one vec4 uniform (two frequencies, slope strength,
field phase), resets it for every modern water draw, and admits only a valid,
enabled component on an actual FFT field. Missing/disabled/non-FFT draws publish
zero. Initial invalidation, lazy acquisition and destruction remain in the
renderer and water-resource lifecycle owners; no texture sampler is added.
Water optical trace and resolved colour use the same renderer named-program
loader, so supported shader asset resolution applies to both programs. Native
shader identity, inputs, positive trace hits and fallback require separate GPU
captures; successful compilation alone does not certify a replacement was used.

`jce_water_ripple_internal.h` owns only continuous body-to-water velocity drive.
Wet cells relax toward an upward m/s target with an exact exponential weight
from executed physics seconds and local wave-crossing time, using existing
depth, collider radius and cell spacing. The shared footprint traversal retains
public `jce_water_ripple_impulse` as a one-shot downward velocity increment;
zero target is meaningful only for the continuous driver. Invalid dt is a no-op.
This bounded local coupling does not certify arbitrary forcing or all contact
scenes; runtime and native integration evidence remain separate.

Water optics visible-sky admission stays private to `jce_sr_water_optics.c`.
Reset sky readiness in every viewport prepare, publish only actual successful
sky submission from `sr_draw_sky_pass`, and upload readiness plus the active
target inverse dimensions on every trace draw. Configuration or a requested sky
is not successful submission; a later viewport must not inherit earlier success.
The vec4 follows the existing uniform invalidation/acquisition/destruction owner
and reuses opaque colour/depth, with no new sampler, target or public field.
The shader accepts only a four-neighbour exact-clear screen-sky footprint and
keeps finite-march/offscreen limitations; runtime GPU evidence is separate from
CPU reset/lifecycle controls.
