/* jce_sr_types.h  The scene renderer's cache-record vocabulary.
 *
 * The Sr* helper types -- one per thing the renderer caches per entity or per
 * asset (models, materials, textures, animation instances, cull results, LOD
 * and fade state) -- together with the engine headers and the capacity
 * constants they are sized by.
 *
 * WHY IT IS ITS OWN FILE.  jce_sr_internal.h described itself as holding "the
 * JceSceneRenderer struct + the Sr* helper types and the engine includes they
 * need".  Those are two different things: this half is the vocabulary the
 * split jce_sr_*.c modules speak to each other in, the other half is one
 * struct.  Kept together they made a 3161-line header that the file-size
 * ratchet had frozen -- no module could add a field to its own cache record
 * without the gate refusing the commit, which is not a size problem the
 * ratchet was meant to create.
 *
 * Internal to the renderer implementation -- NOT a public header and never
 * installed.  Included by jce_sr_internal.h, which is what the modules
 * include; there is no reason to include this one directly.
 */
#ifndef JCE_SR_TYPES_H
#define JCE_SR_TYPES_H

#include <jce/middleware/animation/jce_animation.h>
#include <jce/middleware/animation/jce_anim_sm_binding.h>
#include <jce/middleware/animation/jce_anim_blend_tree.h>
#include <jce/middleware/animation/jce_anim_ik.h>
#include <jce/middleware/animation/jce_anim_foot_ik.h>
#include <jce/middleware/animation/jce_anim_fbbik.h>
#include <jce/middleware/animation/jce_avatar_mask.h>
#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/middleware/scene/jce_lod.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_fullscreen_effect.h>
#include <jce/middleware/physics/jce_cloth.h>
#include <jce/middleware/scene/jce_space_partition.h>
#include <jce/middleware/scene/jce_terrain.h>
#include <jce/middleware/scene/jce_foliage.h>
#include <jce/middleware/scene/jce_water_fft.h>
#include <jce/middleware/scene/jce_component_registry.h>
#include <jce/middleware/scene/jce_tilemap.h>
#include "jce_sr_light_probe.h"
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_frustum.h>
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/resource/jce_static_batch.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_csm.h>
#include <jce/renderer/jce_debug_draw.h>
#include <jce/renderer/jce_ibl.h>
#include <jce/os/core/jce_thread.h>   /* async IBL bake worker + data-parallel cull */
#include <jce/os/core/jce_console.h>  /* r.forwardplus cvar toggle */
#include <jce/renderer/jce_particles.h>
#include <jce/renderer/jce_gpu_particles.h>
#include <jce/renderer/jce_lighting.h>
#include <jce/renderer/jce_lighting_system.h>
#include <jce/renderer/jce_forwardplus.h>
#include <jce/renderer/jce_material.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_impostor.h>   /* octahedral impostor terminal LOD */
#include <jce/renderer/jce_skinned_mesh.h>
#include <jce/renderer/jce_local_shadow.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_gpu_scene.h> /* GPU-driven rendering (roadmap #18) */
#include <jce/renderer/jce_gi_probes.h> /* GI L1 dynamic irradiance probes */
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_ssao.h>      /* screen-space ambient occlusion */
#include <jce/renderer/jce_planar_reflection.h> /* second, mirrored render */
#include <jce/renderer/jce_ssgi.h>      /* screen-space global illumination */
#include <jce/renderer/jce_ssr.h>       /* screen-space reflections */
#include <jce/renderer/jce_render_queue.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_occlusion_culler.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_skybox.h>
#include <jce/renderer/jce_sprite.h>
#include <jce/renderer/jce_taa.h>     /* r.taa temporal anti-aliasing driver */
#include <jce/renderer/jce_sprite_batch.h>
#include <jce/renderer/jce_texture.h>
#include <jce/renderer/jce_views.h>
#include <jce/renderer/jce_volume_profile.h>
#include <jce/renderer/jce_decals.h>
#include <jce/middleware/world/jce_weather.h>
#include <jce/middleware/world/jce_time_of_day.h>
#include <jce/middleware/world/jce_sky.h>

#include "jce_scene_renderer_view_order.h"
#include "jce_scene_internal.h"   /* particle system accessor (private) */
#include "jce_sr_batch_tracker.h"
#include "jce_sr_gpu_policy.h"

/* Animation retargeting (internal src-side header): play a clip authored for a
 * DIFFERENT (source) skeleton on the entity's own (dst) skeleton.  engine/src
 * is on every engine layer's private include path. */
#include <jce/middleware/animation/jce_humanoid.h>   /* role-based retarget map */
#include "middleware/animation/jce_anim_override_controller.h"
#include "middleware/animation/jce_anim_retarget.h"

/* Local prototype for the internal clip sampler.  It lives in the SRC-side
 * jce_animation.h (engine/src/middleware/animation/jce_animation.h), which we
 * cannot include here because its forward-declared typedefs collide in C99 with
 * the PUBLIC <jce/middleware/animation/jce_animation.h> already included above.
 * The signature mirrors that header exactly. */
void jce_anim_clip_sample(const JceAnimClip *clip, float time,
                          jce_mat4 *out_locals, uint32_t num_joints,
                          const jce_vec3 *rest_t,
                          const jce_quat *rest_r,
                          const jce_vec3 *rest_s);

/* Renderer-internal C↔C++ bridge: load baked reflection-probe cubemaps
 * (.ktx) for consumption in the IBL sampler slots. engine/src is on the
 * private include path for every engine layer (see engine/CMakeLists.txt). */
#include "renderer/jce_ktx2_writer.h"

#include <bgfx/c99/bgfx.h>
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jce/os/core/jce_str.h>

#define LOG_TAG "scene_renderer"

/* C99-compatible compile-time assertion. */
#define JCE_SASSERT_CAT_(a, b)  a##b
#define JCE_SASSERT_CAT(a, b)   JCE_SASSERT_CAT_(a, b)
#define JCE_SASSERT(cond)       typedef char JCE_SASSERT_CAT(jce_ct_, __LINE__)[(cond) ? 1 : -1]

/* Public scene_renderer.h declares JceSceneLodStats with picks[JCE_SCENE_LOD_MAX_LEVELS]
   so it does not have to include middleware/scene/jce_lod.h. Keep the
   constants in lock-step. */
JCE_SASSERT(JCE_SCENE_LOD_MAX_LEVELS == JCE_LOD_MAX_LEVELS);

/* Per-path model cache slots.  Must exceed the number of DISTINCT model files a
 * scene references (a full-load city uses ~60: commercial + skyscraper + house
 * variants + cars + trees + props).  Was 32 — which silently OVERFLOWED past 32
 * unique models (33rd+ resolved to NULL -> never rendered).  Looked up O(1) via
 * an open-addressing path hash (sr_get_model). */
#define SR_MODEL_CACHE_MAX     256
#define SR_MODEL_MAX_INFLIGHT  3    /* concurrent async model decodes */
#define SR_MODEL_UPLOADS_PER_FRAME 1 /* GPU uploads; CPU decode stays parallel */
/* 千万 S4: max distance-LOD bands for the model scatter (LOD0..LODn + impostor
 * terminal share these contiguous bands). */
#define JCE_FOLIAGE_LOD_BANDS  6
/* VRAM ceiling: a model must go UNreferenced (not resolved via sr_get_model)
 * for this many full frames before it is eligible for eviction.  A generous
 * window so a model that briefly leaves the view (or a one-frame cull gap) is
 * never freed then immediately reloaded; only a chunk that has genuinely
 * unloaded (its entities destroyed) stays untouched this long.  Compared as
 * (model_frame - last_used_frame) >= grace, where model_frame was bumped at
 * the start of THIS render — so a model used LAST frame has delta 1. */
#define SR_MODEL_EVICT_GRACE   30u
/* Streaming LOD cross-fade (Direction B): seconds a freshly streamed detail
 * entity dithers in (screen-door) while the resident HLOD proxy stays visible.
 * Matches the streamer's proxy-hide delay (JCE_WS_PROXY_HIDE_DELAY) so the
 * proxy is hidden exactly as the detail finishes fading in. */
#define JCE_SR_FADE_DURATION   0.4f
/* Per-entity skinned-anim playback slots.  256 (was 64): with crowd instancing
 * default-on, every animating character still collapses into the instanced
 * draws, so a larger cap means MORE characters truly animate in a big crowd
 * instead of freezing at bind pose past the cap.  Cost: SrAnimInstance is
 * ~17 KB (two 128-bone palettes), so the renderer's anim_inst array grows
 * ~1.1 MB -> ~4.4 MB — acceptable against the 512 MB charter budget; the
 * per-frame pose-eval cost scales with the ANIMATING count and is parallel
 * (sr_anim_sample_range workers). */
#define SR_ANIM_INSTANCE_MAX   256  /* per-entity skinned-anim playback state */
#define SR_SPRITE_ANIM_MAX     64   /* per-entity 2D sprite-animator playback state */
/* Seed sentinel for SrAnimInstance.sm_prev_state: a value that can never be a
 * valid SM state index (or the -1 "no active state").  jce_anim_sm_poll_state_
 * change reports a change from this into the initial state on the first poll, so
 * the initial state's on_state_enter fires once per (re)bind. */
#define SR_SM_STATE_SEED       (-2147483647 - 1)   /* INT_MIN without <limits.h> */
/* FEATURE 3.1 GPU morph deform: max morph-bearing primitives per instance that
 * can hold a live per-instance dynamic vertex buffer.  Bounds the per-instance
 * handle footprint so SR_ANIM_INSTANCE_MAX * SR_MORPH_PRIM_MAX dynamic VBs stay
 * well within the bgfx dynamic-VB handle pool budget.  A face rig is typically
 * 1-2 morph prims; 8 is generous headroom. */
#define SR_MORPH_PRIM_MAX      8
/* Clips resolved from a FILE rather than from the model.  Eight is the
   SkeletalAnimator's own clip_names[8]: an entity cannot reference more
   clips than it can name. */
#define SR_FILE_CLIP_MAX       8
#define SR_TEX_CACHE_MAX       512
#define SR_TEX_HASH_SIZE       2048 /* pow2, >=4x SR_TEX_CACHE_MAX (load <0.25) */
#define SR_TEX_MAX_INFLIGHT    6    /* concurrent async texture decodes */
/* INITIAL per-frame render-list / cull-cache capacity (large-world capacity,
 * Direction C).  This is NO LONGER a hard cap: the per-frame entity list and the
 * per-entity cull cache GROW on demand to the scene's actual entity count, so a
 * world of any size renders fully (no silent first-N drop).  Sized to cover a
 * typical full-load big world so the common case allocates once and never
 * reallocs (a scene with <= this many entities behaves exactly as the old fixed
 * array did).  Buffers are heap, reused across frames; the per-frame cost is the
 * O(n) collect + cull, which the instanced passes keep cheap. */
#define SR_MAX_ENTITIES        32768
#define SR_MAT_CACHE_MAX       512
#define SR_MAT_HASH_SIZE       2048 /* pow2, >=4x SR_MAT_CACHE_MAX (load <0.25) */
#define SR_MAT_PROG_CACHE_MAX  64   /* per-path Shader Graph custom programs */
/* Distinct viewport identities that drive this shared renderer in one frame
 * (editor Game + Scene viewports today).  Each keeps its own previous-frame
 * view*proj for correct, order-independent per-view TAA motion vectors. */
#define JCE_SR_VIEWPORT_SLOTS  7
/* Who owns which slot, because "pass a different id" is not a rule anyone can
 * check: 0 the editor Scene View and the shipped runtime, 1 the editor Game
 * View, 2 free, 3 the planar reflection, 4..6 the camera-stack overlays. */
#define JCE_SR_VIEWPORT_SLOT_CAMERA_OVERLAY 4
#define JCE_SR_FULLSCREEN_EFFECT_CAP JCE_SCENE_FULLSCREEN_EFFECT_MAX_PASSES
#define SHADOW_ORTHO_SIZE      50.0f
#define CSM_DIST_SCALE         512.0f
#define CSM_DIST_MAX           300.0f  /* was 1200: quartered the near-cascade
                                          texel density for every scene without
                                          an authored distance; the aerial
                                          view_reach growth in
                                          sr_draw_shadow_pass covers far vistas
                                          on demand */
#define CSM_DIST_MIN           50.0f
/* Stable, camera-independent CSM cascade near plane (see shadow_near below). */
#define JCE_CSM_SHADOW_NEAR    0.1f
#define CSM_FAR_HYST_REL       0.03f
#define CSM_FAR_HYST_ABS       8.0f

/* Terrain per-chunk LOD + culling (P1-terrain-lod). Each terrain entity is
 * drawn as N independent chunk draws instead of one merged ~16M-vert mesh:
 * per-chunk frustum culling skips off-screen chunks, and the chunk's LOD is
 * picked from camera distance so far chunks decimate to a fraction of the
 * verts.  Chunk meshes are cached (built once at the LOD first requested, and
 * rebuilt only when that chunk needs a different LOD or the terrain is edited).
 *
 * SR_TERRAIN_MAX_CHUNKS caps the per-terrain chunk array; a 4097^2 heightmap
 * at the default chunk_size (64) tiles to 64x64 = 4096 chunks, so 4096 covers
 * the documented worst case.  LOD thresholds are in *world units* measured from
 * the camera to the chunk centre; LOD i applies beyond SR_TERRAIN_LOD_DIST*i.
 * SR_TERRAIN_MAX_LOD clamps decimation so a chunk never collapses below a 2x2
 * quad grid regardless of distance. */
#define SR_TERRAIN_MAX_CHUNKS   4096
#define SR_TERRAIN_MAX_LOD      4
#define SR_TERRAIN_LOD_DIST     80.0f   /* world units per LOD band   */
/* Shadow casters use a coarse fixed LOD: the depth pass doesn't need the full
 * silhouette and this caps the per-chunk shadow cost.  A chunk already cached
 * by the colour pass is reused as-is (its exact LOD barely matters for depth);
 * only un-cached chunks are built specifically at this LOD. */
#define SR_TERRAIN_SHADOW_LOD   2
/* Skirt depth as a fraction of the terrain's max world height.  A small
 * vertical apron is dropped around each chunk's outer ring so that T-junction
 * cracks between neighbouring chunks at different LODs are hidden behind solid
 * (textured) geometry rather than showing the background through the seam. */
#define SR_TERRAIN_SKIRT_FRAC   0.04f

/* Tilemap chunked draw (P5-tilemap).  Mirrors the terrain pattern: the map is
 * baked into 32x32-cell chunks of textured quads (entity-local space, one
 * static VB per non-empty chunk, ONE shared static IB for all of them) and
 * drawn with per-chunk frustum culling.  No LOD — tile quads are already the
 * cheapest representation. */
#define SR_TILEMAP_SLOT_MAX     8
#define SR_TILEMAP_CHUNK_DIM    32                 /* cells per chunk side  */
#define SR_TILEMAP_CHUNK_QUADS  (SR_TILEMAP_CHUNK_DIM * SR_TILEMAP_CHUNK_DIM)

/* Water surface tessellation: the wave displacement happens per-VERTEX in
 * vs_water.sc, so the grid must be dense enough to resolve the shortest
 * authored wavelength.  64x64 quads (65x65 verts) over the plane is a
 * reasonable default (matches the roadmap's "e.g. 64x64"). */

/* Shadow-cache caster key: pure arithmetic, so it lives in its own header
 * and can be tested without a bgfx context. */
#include "jce_shadow_key.h"
#include "jce_shadow_sun.h"
#include "jce_shadow_lod.h"
#include "jce_shadow_bucket.h"
#include "renderer/jce_sky_stylise.h"
#include "jce_shadow_blend.h"
#include "jce_contact_shadow.h"
#include "jce_water_optics.h"

#include <jce/middleware/world/jce_environment.h>
#include "renderer/jce_underwater.h"
#include "renderer/jce_cloud_shadow.h"

#define SR_WATER_GRID_RES       64                 /* quads per side        */
#define SR_WATER_SLOT_MAX       16                 /* == water_cache[] size */

/* Local (spot/point) shadow atlas — P1. A square atlas packs up to
 * JCE_MAX_LOCAL_SHADOWS perspective depth tiles in a NxN grid; each
 * shadow-casting local light renders into one tile via its own bgfx view
 * (view_id_base + 4 + slot). v1 wires SPOT lights; point lights TODO. */
#define JCE_MAX_LOCAL_SHADOWS  4
#define JCE_LOCAL_SHADOW_TILES 2   /* 2x2 grid -> 4 tiles */
#define JCE_VIEW_LOCAL_SHADOW_OFFSET 4 /* base+4..base+8, free for base 0/3/80 */
/* Omnidirectional point shadows (parity #7) — OPT-IN via the point_cube_shadows
 * gate; default OFF keeps the 2x2/4-tile layout byte-identical. When on, the same
 * single 2D atlas (stage 15) subdivides 6x6 (36 tiles): up to JCE_POINT_SHADOW_MAX
 * point lights get 6 contiguous cube-face tiles each (FOV=PI/2), the first 4 slots
 * still serve spots/legacy. The 6 per-face tile views relocate to a FREE view band
 * base+100.. (base+50/60/70 are EDITOR_OVERLAY/PREVIEW/PICK; 72..231 is free). */
#define JCE_POINT_CUBE_FACES   6
#define JCE_POINT_SHADOW_MAX    2   /* nearest shadow-casting point lights with cube */
#define JCE_LOCAL_SHADOW_TILES_CUBE 6   /* 6x6 = 36 tiles when cube shadows on */
#define JCE_VIEW_LOCAL_SHADOW_CUBE_OFFSET 100 /* base+100..+115 (free band 72..231) */
/* The dual shadow-map dynamic-atlas view band lives in
 * jce_scene_renderer_view_order.h (JCE_VIEW_DYN_CSM_OFFSET), next to the
 * order builder that reserves it. */

#define JCE_POINT_CUBE_FOV     1.5708f  /* PI/2 per cube face */
/* GPU particle COMPUTE view (simulate + emit dispatches; no draws).  base+9
 * is the last free slot below the shadow band; the view-order builder pushes
 * it ahead of base+0 so dispatches execute before the draw that consumes the
 * pool. One JceGpuParticleSystem per GPU-flagged emitter (size/color lerp
 * comes from global uniforms per update call, so a pool cannot be shared). */
#define JCE_VIEW_GPU_PARTICLE_OFFSET 9
/* GPU-driven cull counter-RESET compute view (roadmap #18 Direction C).
 *
 * It said "the free pre-color slot base+3 (base+1=velocity, base+2=SSAO,
 * base+3 free)".  base+3 is NOT free: jce_ssao_render() takes one view id and
 * uses that one AND the next (sample, blur), so SSAO owns base+2 and base+3.
 * With SSAO and r.gpu_driven both on, the order builder pushed base+3 to sort
 * FIRST -- ahead of everything -- so SSAO's blur ran before SSAO's own sample.
 *
 * The slot is now declared in the PUBLIC reservation table next to the rest of
 * the band, because a reservation that lives only in one subsystem's private
 * comment is exactly what let this happen.  Ordering requirement is unchanged:
 * before the cull/compact view (base+9) so bgfx inserts a cross-view compute
 * barrier between the counter reset and the compact atomics (D3D12 gets no
 * same-view barrier because both keep the counter in UAV state). */
#define JCE_VIEW_GPU_CULL_RESET_OFFSET JCE_VIEW_SR_GPU_CULL_RESET_OFFSET
#define SR_GPU_PARTICLE_MAX          64
/* Grass spatial-cull grid: up to 32x32 = 1024 cells per field.  The blade
 * field is binned into this grid at scatter time so the color pass can
 * frustum + fade-distance cull whole cells (submitting only on-screen blades)
 * instead of the entire — post-080e7b15, 2x denser — field every frame. */
#define JCE_GRASS_CELL_DIM           32u
#define JCE_GRASS_CELL_CAP           (JCE_GRASS_CELL_DIM * JCE_GRASS_CELL_DIM)
/* Point lights are omnidirectional; v1 approximates with a single wide-FOV
 * perspective frustum aimed straight down (good for elevated point lights,
 * weaker for ground-level ones). ~126deg. dual-paraboloid/cube is a future upgrade. */
#define JCE_POINT_SHADOW_FOV   2.2f

/* ── Internal struct ──────────────────────────────────────────────── */

/* Async IBL bake job (own: jce_sr_environment.c). Defined here because the
 * renderer core tears it down. CPU output is uploaded by the render thread. */
struct SrIblJob {
    float         *pixels;  /* owned copy of equirect; freed by the worker */
    uint32_t       w, h, irr, pf;
    JceIblCpuData *result;  /* worker → render thread */
    float           bake_ms;   /* worker-side convolution time */
};

/* Per-emitter GPU particle record (P3-E gpu-particles wiring).  One compute
 * pool per GPU-flagged JceParticleEmitterComponent; created lazily when the
 * routing predicate (jce_scene_particle_emitter_uses_gpu) first passes and
 * reaped by mark-and-sweep when the entity vanishes / toggles back to CPU.
 * The authored desc is cached so the .particles.json is not re-read per
 * frame; `epoch` detects authoring edits (rebuild). */
typedef struct {
    JceEntity              entity;
    JceGpuParticleSystem  *sys;
    JceParticleEmitterDesc desc;        /* cached authored description */
    char                   tex_path[256]; /* authored billboard texture ("" = none) */
    float                  emit_accum;  /* fractional emit carry */
    bool                   burst_done;  /* one-shot emit_burst fired */
    uint64_t               epoch;       /* authoring-change marker */
    bool                   referenced;  /* mark-and-sweep flag */
    bool                   used;
} SrGpuParticleRec;

/* Per-frame material registry entry. Snapshot of everything the binder
 * needs to re-bind textures + uniforms when render-queue auto-batching
 * starts a new material run. Built lazily as scene_renderer iterates
 * entities; flushed at frame end. */
typedef struct {
    uint32_t        key;
    JcePbrMaterial  pbr;
    bool            is_terrain;
    int             terrain_slot;   /* index into sr->terrain_cache, -1 if none */
    /* Terrain runtime params (copied so binder doesn't need TerrainComponent) */
    float           terrain_tile_scale;
    bool            terrain_splat_enabled;
    bgfx_texture_handle_t terrain_layer_tex[4]; /* layer0..3 albedo handles (white fallback) */
    /* The rendering layer of this run's objects (0..31).  Part of the KEY, so
     * every draw in the run shares it. */
    uint32_t        receiver_layer;
} SrMaterialEntry;

/* Shared, per-PATH model (mesh/skeleton/materials/textures) — heavy, loaded
   once and shared by every entity that references the same file.
   Runtime (no editor callback) loads asynchronously: a cache miss kicks a
   worker that decodes the glTF to a CPU intermediate (parse + vertex
   extraction + image decode), and the render thread uploads it
   (sr_model_poll). While pending, model stays NULL so callers skip the
   entity until it's ready. */
typedef struct {
    char            path[256];
    uint32_t        path_hash; /* FNV-1a of path — open-addressing key (sr_get_model) */
    JceModel       *model;
    bool            used;
    bool            failed;   /* load was attempted and failed; do not retry */
    bool            pending;  /* async decode in flight */
    JceAsyncTask   *task;     /* structured decode task */
    struct SrModelJob *job;   /* worker job (owns CPU result) */

    /* VRAM ceiling (large-world-opt): refcount-by-frame-reachability eviction.
     * `last_used_frame` is stamped with sr->model_frame every time any draw /
     * cull / shadow / anim path resolves this path via sr_get_model — i.e. a
     * per-frame reference count.  When a streamed chunk unloads, its entities
     * are destroyed and stop resolving their model, so the slot goes untouched;
     * after a grace window of frames with NO touch it is provably unreferenced
     * by any LIVE entity and can be freed (jce_scene_renderer_evict_models).
     * vram_bytes caches jce_model_gpu_bytes(model) at upload so the budget +
     * LRU victim selection read real GPU bytes, not a flat estimate.  Both are
     * meaningful ONLY in runtime mode (SR owns the model); in editor mode the
     * asset-cache callback owns models and SR never frees them. */
    uint64_t        last_used_frame;
    uint64_t        vram_bytes;

    /* STATIC-BATCH MEMBERS: the index sub-range and world box of each mesh
     * that was merged into this model, from the "<stem>.batch.json" the bake
     * wrote beside it.
     *
     * A merged group is ONE draw -- the point of merging -- and therefore one
     * cullable object: a row of forty fence posts folded into one mesh draws
     * all forty whenever any one of them is on screen.  With this table the
     * colour pass submits only the runs it can see, and a group that is
     * wholly visible still coalesces to exactly ONE submit.
     *
     * `batch_probed` is the has-been-asked flag, separate from a zero count:
     * almost every model in a project has no sidecar, and re-asking the pak
     * once per frame per entity for a file that is not there is the shape of
     * a cache that is not one. */
    JceStaticBatchMember *batch_members;   /* owned; NULL = none         */
    uint32_t              batch_member_count;
    bool                  batch_probed;
    /* Said ONCE, the first frame this group actually culls something.
     * Per frame it would be 60 lines a second; never, and the only
     * evidence that the member table reached the submit would be that the
     * picture did not change -- which is also what a table that never ran
     * looks like. */
    bool                  batch_cull_logged;
} SrModelCache;

/* Per-ENTITY skeletal-animation instance. The model above is shared by path;
   each entity gets its OWN player, playback state, and skin palette so multiple
   instances of the same model animate independently. (Previously the palette
   lived on the per-path cache, so N copies of one .glb shared a single pose and
   overwrote each other every frame — last writer won.) The skeleton is
   read-only during evaluation, so many players may safely share one skeleton.

   skin_palette is evaluated ONCE per frame in sr_update_skinned_anims (before
   the shadow pass) and consumed by both the shadow and color passes so the cast
   shadow deforms in lock-step with the lit mesh. count == 0 => draw bind pose. */
typedef struct {
    uint32_t        entity;
    JceModel       *model;        /* borrowed; owned by SrModelCache */
    JceAnimPlayer  *player;
    int             active_clip;
    bool            loop;
    float           speed;
    bool            paused;
    bool            used;
    jce_mat4        skin_palette[JCE_MAX_BONES];
    uint32_t        skin_palette_count;
    /* GPU crowd instancing (JCE_CROWD_INSTANCE): this frame's base offset (in
       BONES) of this character's palette inside the shared per-frame bone
       texture, and the frame it was packed.  The color/prepass/shadow batchers
       read crowd_palette_base into i_data0.x so one instanced draw renders the
       whole same-mesh crowd, each instance sampling its own pose region.  Valid
       only when crowd_palette_frame == sr->bone_tex_frame (else fall back to the
       per-character skinned draw). */
    uint32_t        crowd_palette_base;
    uint32_t        crowd_palette_frame;
    /* TAA per-bone motion: the PREVIOUS frame's resolved skin palette + count,
       used to compute prev-frame clip positions in the velocity pass so even
       limb animation produces correct motion vectors (no ghosting).  Snapshotted
       at the START of sr_update_skinned_anims (before this frame overwrites
       skin_palette), so during the prepass prev_skin_palette == last frame's.
       prev_skin_valid gates the first frame (no prev => zero motion). */
    jce_mat4        prev_skin_palette[JCE_MAX_BONES];
    uint32_t        prev_skin_palette_count;
    bool            prev_skin_valid;
    JceAnimSmBinding *sm_binding;  /* lazily created from the component's sm_path */
    char             sm_path[256]; /* path the binding was built from (detect change) */
    JceAnimBlendTree *blend_tree;  /* cached 1D/2D tree; rebuilt when entries change */
    int               bt_count;    /* clip count the tree was built for */
    float             bt_thresh[8];/* thresholds (1D) / X positions (2D) built for */
    float             bt_pos_y[8]; /* Y positions the 2D tree was built for */
    int               bt_mode;     /* mode the tree was built for (0=1D,1/2=2D) */
    float             bt_time;     /* shared phase time advanced each frame */

    /* Generic motion-driven SM/blend params: planar speed from frame-to-frame
       Transform delta, fed into the SM as a "Speed" float (and blend_param)
       so any animated entity auto-switches state by how fast it moves. */
    jce_vec3          sm_prev_pos;
    bool              sm_have_prev;
    float             sm_speed;     /* smoothed (EMA) so the SM doesn't flicker */

    /* SM transition crossfade: elapsed time inside the active transition
       (drives the to-clip's local time) + the transition index it belongs
       to, so a new transition restarts the clock.  sm_seed_time hands the
       blended to-clip time to the single-clip player when the transition
       completes (-1 = nothing to seed). */
    float             sm_trans_time;
    int               sm_trans_idx;  /* -1 = not transitioning last frame */
    float             sm_seed_time;

    /* SM state-change dispatch (state-enter/exit → gameplay). Tracks the SM's
       active state index across frames so jce_anim_sm_poll_state_change can
       detect transitions into a new state and fire on_state_exit/on_state_enter
       via the renderer's anim_state_fn hook. Seeded to SR_SM_STATE_SEED (a
       sentinel < -1) on create/rebind so the FIRST poll reports a change into
       the initial state (on_state_enter for the start state). */
    int               sm_prev_state;

    /* Frame-event dispatch (P1 #16). Events are loaded once from a
       <skeleton_path>.anim.json sidecar (per-clip arrays) and fired by
       jce_anim_events_advance() over the (prev,cur] clip-time interval each
       frame. ev_loaded gates the one-shot lazy load; ev_prev_time/ev_clip
       track the playhead so wraps and clip changes reset cleanly. */
    JceAnimEvent     *ev_pool;       /* owned; all events of all clips, contiguous */
    int               ev_pool_count;
    JceAnimEventTrack ev_tracks[16]; /* per-clip view into ev_pool (index == clip) */
    int               ev_track_count;
    bool              ev_loaded;     /* sidecar load attempted (success or absent) */
    int               ev_clip;       /* clip the prev-time belongs to (-1 = none) */
    float             ev_prev_time;  /* clip time at the previous frame */

    /* IK constraints: warn about unresolved bone names only once per
       instance instead of spamming the log every frame. */
    bool              ik_warned;

    /* Avatar bone mask (FEATURE 3.3): lazily loaded from the entity's
       JceAvatarComponent.mask_path and resolved against the model skeleton.
       Cached so the .mask asset is parsed once; reloaded when the path
       changes. NULL when the avatar authors no mask (the common case → the
       layered-blend path is never engaged and playback is byte-identical). */
    JceAvatarMask    *avatar_mask;
    char              avatar_mask_path[128];

    /* Avatar additive/override layer stack (FEATURE 3.3 authoring). Up to
       SR_AVATAR_MAX_LAYERS layers, each with its own cached .mask (resolved
       once per distinct path). Composed over the base pose via
       jce_anim_player_blend_layers when the avatar authors layer_count > 0. */
    JceAvatarMask    *layer_mask[4];
    char              layer_mask_path[4][128];

    /* Morph targets / blendshapes (FEATURE 3.1, last-mile). Final per-target
       weights resolved once per frame in sr_update_skinned_anims =
       jce_morph_resolve_weights(track ⊕ authored static JceMorphWeights). Only
       populated when the model has morph data AND the entity authors a
       JceMorphWeights component OR the model carries a morph-weight track;
       morph_count == 0 means "no morph this frame" (byte-identical to legacy).
       jce_morph_apply against the model's per-prim deltas runs on these. */
    float             morph_weights[JCE_MORPH_MAX_WEIGHTS];
    uint32_t          morph_count;

    /* FEATURE 3.1 GPU vertex-deform: per-instance dynamic vertex buffers
       holding the CPU-morphed (jce_morph_apply) verts for each morph-bearing
       primitive.  Lazily created on the first morphing frame and reused; the
       UNCHANGED skinned program reads these in place of the shared static VB
       (the bone palette / u_model[] upload is untouched).  morph_vb_node/_prim
       record which (node, prim) each slot maps to so the draw callback can match
       primitives; morph_vb_count is how many slots are live.  morph_last_weights
       /_count are the dirty token — the deform re-uploads only when the resolved
       weight vector changes, so static weights cost nothing per frame.  All
       handles start BGFX_INVALID_HANDLE (memset path); destroyed at every
       instance-lifecycle site before the shared model (handle-leak guard). */
    bgfx_dynamic_vertex_buffer_handle_t morph_vb[SR_MORPH_PRIM_MAX];
    uint32_t          morph_vb_node[SR_MORPH_PRIM_MAX];
    uint32_t          morph_vb_prim[SR_MORPH_PRIM_MAX];
    uint16_t          morph_vb_count;
    /* Which of those slots hold a COMPUTE_WRITE buffer (bit s = slot s).
     *
     * PER SLOT, not per instance, because the decision is per PRIMITIVE: the
     * GPU cache can be full, or one primitive's layout may not be 4-byte
     * addressable, while its neighbour is fine.  And it has to be REMEMBERED
     * rather than re-derived: a COMPUTE_WRITE buffer is a device-local
     * resource with no CPU access, so a frame that fell back to
     * jce_morph_apply on one would upload into nothing and show the LAST
     * deform forever -- a stuck face, not an error. */
    uint32_t          morph_vb_gpu_mask;
    float             morph_last_weights[JCE_MORPH_MAX_WEIGHTS];
    int               morph_last_count;   /* -1 = never uploaded (force first) */
    /* Warn once per instance, not once per frame: a clip that animates
       blendshapes on more than one node cannot be honoured by a per-INSTANCE
       weight vector, and 60 identical lines a second is how a real warning
       becomes something people filter out. */
    bool              morph_multi_node_warned;
    /* One line per instance, not per frame: the muscle clamp firing is
     * worth saying once and worth saying 60 times a second never. */
    bool              humanoid_clamp_logged;

    /* Animation retargeting (optional). When the entity authors a
       retarget_source_skeleton, the active clip is sampled against the SOURCE
       rig and transferred onto this (dst) skeleton via this cached map. The map
       borrows the src/dst skeletons (owned by SrModelCache), so it is rebuilt
       whenever either skeleton pointer changes (model swap / cache reclaim) and
       freed at every instance-lifecycle site. retarget_src is the path the map
       was built for (detect a changed source field). NULL map ⇒ no retargeting
       this frame ⇒ byte-identical legacy playback. */
    JceAnimRetargetMap *retarget_map;
    const JceSkeleton  *retarget_src_skel;  /* src skeleton the map was built on */
    const JceSkeleton  *retarget_dst_skel;  /* dst skeleton the map was built on */
    char                retarget_src[256];  /* source skeleton path of the map   */
    /* ROLE-based maps, built beside the name map and used INSTEAD of it when
       they cover more joints -- which is the case the name map cannot serve at
       all: two humanoid rigs from different tools share no bone names, so a
       name map covers zero and the clip visibly does nothing.
       By value: JceHumanoidMap is a POD of two fixed arrays, so there is
       nothing to free and no lifetime to get wrong at the instance-teardown
       sites the name map has to be released at. */
    JceHumanoidMap      humanoid_src;
    JceHumanoidMap      humanoid_dst;
    bool                humanoid_valid;   /* both maps built for these skeletons */
    bool                humanoid_wins;    /* ...and covers more than the names */
    /* The role map of the entity's OWN rig, for humanoid-limb IK goals
       (JceIkConstraint kind 7).  Separate from humanoid_dst above even though
       the two hold the same thing when a retarget is running: humanoid_dst is
       only built ON the retarget path, so a rig playing its own clips would
       find it empty, and an IK goal that silently does nothing on the rigs
       that need no retargeting is the worse half of a half-wired feature. */
    JceHumanoidMap      humanoid_self;
    bool                humanoid_self_built;
    /* Animator override controller: ONE graph, a different set of clips.
       Cached here the same way the retarget map above is and for the same
       reasons -- per-entity, resolved from a path that can change, and the
       alternative is reading a file every frame.  `aoc_path` is what it was
       loaded for, so a changed field reloads and an unchanged one does not. */
    JceAnimOverrideController *aoc;
    char                       aoc_path[128];

    /* Clips that are NOT inside the model, loaded from
       `<skeleton dir>/<name>.animclip.json` when the model has no clip of
       that name.  Cached per INSTANCE because a file clip has no other
       owner -- the model owns its own -- and released at the same
       instance-lifecycle sites as retarget_map and aoc above, which is the
       reason it does not add a third lifetime to get wrong.

       A MISS IS CACHED AS A MISS (clip == NULL): a name in neither place
       would otherwise stat the filesystem every frame for every entity, and
       a negative entry makes the second frame a strcmp.  Same shape as the
       runtime's authored-curve cache. */
    struct {
        char         name[64];
        JceAnimClip *clip;        /* NULL = looked for and not found */
    } file_clips[SR_FILE_CLIP_MAX];
    int  file_clip_count;
    /* The skeleton path the file clips resolve relative to, stashed
       where it is in scope (the frame loop's `sa->skeleton_path`) so
       the resolvers below need only the instance. */
    char skel_path[256];
} SrAnimInstance;

/* GPU crowd instancing: per-model accumulator of the color pass's eligible
 * skinned characters, keyed by resolved JceModel*.  Each group flushes to one
 * instanced draw per skinned primitive (bases -> i_data0.x).  bases grows on
 * demand and is kept across frames (only count resets). */
#define SR_CROWD_MAX_GROUPS 32
typedef struct {
    void     *model;    /* JceModel* key (opaque here) */
    jce_mat4 *worlds;   /* per-instance world matrix (i_data0..3) */
    uint32_t *bases;    /* per-instance palette base   (i_data4.x) */
    uint32_t  count;
    uint32_t  cap;
} SrCrowdGroup;

#define SR_AVATAR_MAX_LAYERS 4

/* Per-entity 2D sprite-animator playback state (P1 #16). */
typedef struct {
    uint32_t         entity;
    char             sheet_path[256];
    char             atlas_path[256];
    int              frame_w, frame_h;
    char             cur_anim[64];
    JceSpriteSheet  *sheet;   /* owned */
    JceSpritePlayer *player;  /* owned */
    bool             used;
} SrSpriteAnim;

/* TAA per-object motion: previous-frame world matrix per static/model entity.
   Looked up before drawing into the velocity G-buffer, written back AFTER the
   prepass.  A small open-addressed table keyed by entity id; LRU-ish: dead
   entries are pruned each frame (touched flag).  Only maintained while TAA is
   on (taa_want_velocity), so it costs nothing when TAA is off. */
#define SR_PREV_XFORM_MAX 512
typedef struct {
    uint32_t entity;
    jce_mat4 prev_world;
    bool     used;
    bool     touched;   /* set when looked-up/updated this frame; prune untouched */
} SrPrevXform;

/* Per-frame color-pass GPU-instancing accumulator entry: a static, non-skinned,
 * no-material-override glTF model instance.  Entries are grouped by model
 * pointer at flush and drawn with one jce_model_draw_instanced per model. */
typedef struct {
    JceModel *model;
    jce_mat4  world;
    /* In-asset auto-LOD level for this instance (large-world-opt P1 #6).  0 =
     * base (LOD0).  The batch is sorted+run-split by (model, lod) so every copy
     * in a run shares one index set and STILL collapses into a single instanced
     * submit — distant batched meshes drop triangles without exploding the draw
     * count.  Almost always 0 (the common case) => one run per model, identical
     * to the pre-LOD batching. */
    uint16_t  lod;
    /* Per-instance baseColor tint (large-world-opt P1 #7).  RGBA, linear.  A
     * MeshRenderer that authored only a non-white baseColor (no divergent
     * texture/material) STAYS in the instanced batch carrying its colour here
     * instead of being kicked to a solo draw — the tint is fed to i_data4 of
     * vs_pbr_inst_tint and modulates albedo exactly like u_baseColorFactor on a
     * solo draw.  [1,1,1,1] = no tint (the common case → the run uses the plain
     * stride-64 vs_pbr_inst program, byte-identical to before). */
    float     tint[4];
} SrInstEntry;

/* Factor-only primitive tint-instancing batch entry (color-pass, opt-in
 * JCE_PRIM_INSTANCE).  One per eligible shape primitive (ec->parallel_eligible:
 * shared built-in mesh, pure factors, opaque, SHADED).  Grouped by (mesh,
 * mat_key) — where mat_key is computed with base_color forced WHITE — so
 * base_color-only variants collapse into a single instanced submit that carries
 * each copy's real colour per-instance in i_data4 (vs_pbr_inst_tint), instead of
 * one solo draw apiece.  Closes the primitive analogue of the (model,lod)+tint
 * batch that already exists for glTF models. */
typedef struct {
    JceMesh  *mesh;      /* shared built-in shape mesh (batch key part 1) */
    uint32_t  mat_key;   /* registered material key, base_color WHITE (part 2) */
    uint64_t  state;     /* shared bgfx render state (blend/cull/depth) */
    jce_mat4  world;     /* per-instance world matrix */
    jce_vec4  tint;      /* per-instance real base_color → i_data4 */
    /* Squared view distance, used ONLY as the last sort key inside a batch.
     * Batching collapses thousands of copies into one instanced draw, which
     * is a large CPU win but destroys the front-to-back submission order that
     * early-Z depends on: the GPU rasterises instances in buffer order, so an
     * arbitrarily-ordered batch shades every layer of overdraw.  Measured with
     * RenderDoc on a 150k-cube stress scene: ONE 21221-instance draw cost
     * 24.8 ms of a 25.9 ms frame at ~250k triangles -- 31M indices/s, far too
     * slow to be geometry-bound, i.e. pure fill.  Sorting within the batch
     * restores the ordering without splitting the batch, because the grouping
     * keys still compare first. */
    float     view_dist2;
    /* Shared bgfx stencil word (0 = none), like `state`; AFTER view_dist2
     * because the gathered upload asserts world and tint stay adjacent. */
    uint32_t  stencil;
} SrPrimInstEntry;

/* Texture-diverse instancing (slice-A, opt-in JCE_TEX_INSTANCE) batch entry.
 * One per eligible entity that shares a mesh + all non-albedo material factors
 * with others but has its own albedo TEXTURE.  Grouped by (mesh, mat_key), the
 * distinct albedos of a group are packed into a 2D-array (one layer each) and
 * the group draws as ONE instanced submit carrying each copy's layer in
 * i_data4.x (fs_pbr_inst_tex_array samples texture2DArray at that layer). */
typedef struct {
    JceMesh  *mesh;      /* shared mesh (batch key part 1) */
    uint32_t  mat_key;   /* material key with albedo forced WHITE (batch key part 2) */
    uint64_t  state;     /* shared bgfx render state */
    jce_mat4  world;     /* per-instance world matrix */
    uint16_t  albedo;    /* this entity's albedo bgfx tex idx → array layer */
    uint16_t  w, h;      /* albedo dimensions (must match within a group) */
    uint8_t   mips;      /* source mip count (array is built with a matching chain) */
    /* Front-to-back key, last in the sort so batching is unaffected --
     * see SrPrimInstEntry for why an unordered instance batch costs the
     * whole frame in overdraw. */
    float     view_dist2;
    uint32_t  stencil;   /* shared bgfx stencil word (0 = none) */
} SrTexInstEntry;

/* Built albedo 2D-array cache entry: one per distinct (sorted albedo-handle set,
 * w, h). Keeps the source handle list so an entity's albedo maps to its layer,
 * and is rebuilt (re-blitted) only when its source set changes. */
#define SR_TEXARR_MAX_LAYERS 64
/* Cap on the built-array cache: past this many entries in one session the cache
 * is reset (arrays destroyed, rebuilt on demand) to bound growth and clear
 * entries whose source bgfx handle may have been recycled. */
#define SR_TEXARR_CACHE_MAX  256
typedef struct {
    uint32_t              set_hash;     /* FNV of the sorted src[] + w/h + mips */
    uint16_t              w, h, layers;
    uint8_t               mips;         /* mip levels blitted into the array */
    uint16_t              src[SR_TEXARR_MAX_LAYERS]; /* source albedo idx per layer */
    bgfx_texture_handle_t array_tex;    /* the built 2D-array */
    bool                  used;
} SrTexArrayEntry;

/* ── Octahedral impostor terminal LOD (roadmap P2 #10) ──────────────────
 * Per-PATH atlas cache: a baked octahedral atlas (+ metadata) loaded once and
 * shared by every entity whose LODGroup references the same .impostor.json.
 * Keyed by the meta path; the texture is resolved via sr_resolve_texture so it
 * rides the normal (editor callback / runtime PAK) texture cache. */
#define SR_IMPOSTOR_CACHE_MAX 64
typedef struct {
    char             meta_path[256];
    JceImpostorAtlas atlas;     /* atlas texture + metadata          */
    bool             used;
    bool             failed;    /* load attempted + failed; don't retry per frame */
} SrImpostorCache;

/* Per-frame per-atlas instance accumulator: far-cards sharing one atlas are
 * collected during the entity walk and submitted as ONE instanced draw at
 * flush (sr_impostor_flush), so a forest = a handful of draws. */
typedef struct {
    int                   cache_slot;  /* index into sr->impostor_cache         */
    JceImpostorInstance  *insts;       /* grown on demand                        */
    uint32_t              count;
    uint32_t              cap;
} SrImpostorBatch;

/* Per-frame per-entity cull cache.  Built ONCE per frame (after the entity
 * collect, before the shadow pass) and reused by every cull site that would
 * otherwise recompute the SAME world AABB + model resolve ~6×/frame (collect,
 * each CSM cascade, the color/prepass cull).  Indexed by the SAME index as
 * EntityList.entities[i].  has_aabb mirrors sr_shadow_caster_aabb's return:
 * false => no resolvable model => DON'T cull (let the entity through, matching
 * the pre-cache behaviour where the && short-circuited to not-culled). */
typedef struct {
    jce_vec3 wmin, wmax;
    bool     has_aabb;
    bool     casts_shadow;
    /* world_valid mirrors "entity has a transform" (false => callers fall
     * back to jce_scene_get_world_matrix; e.g. transform-less specials).  The
     * matrix itself lives in the index-parallel sr->ecull_world array (SoA
     * split): the hot cull loops stream this 40B entry, not the 64B matrix
     * only the submit paths read. */
    bool     world_valid;
    /* In-asset auto-LOD level for THIS entity this frame (large-world-opt P1 #6).
     * 0 = base (LOD0); >=1 selects the (lod_level-1)'th reduced index set on the
     * entity's own model.  Computed ONCE in the ecull build loop (camera-
     * dependent, so recomputed every frame even on a wcache hit) so the color
     * pass and the shadow caster loop read the SAME level — the cast silhouette
     * always matches the rendered LOD.  lod_culled => the LODGroup's far cull
     * fired (skip the entity entirely in both passes). */
    uint8_t  lod_level;
    bool     lod_culled;
    /* LOD cross-fade (large-world #6): fraction [0,1] of the OUTGOING level
     * (lod_level) to keep while transitioning to lod_level+1; 1.0 = steady (no
     * transition, byte-identical). When <1 the draw dithers lod_level out and
     * fills the holes with lod_level+1 drawn full underneath — a pop-free fade. */
    float    lod_fade;
    /* Cross-pass render-kind classification (Fix #1): computed ONCE per frame in
     * the ecull classify loop so the color / shadow / depth-velocity passes can
     * skip re-running the per-entity has_* dispatch ladder — the dominant cross-
     * pass CPU redundancy (the same classification was re-derived 5-6×/frame,
     * each derivation running ~16 has_* component probes).
     *
     * NOT because of "flecs slow-path lookups above FLECS_HI_COMPONENT_ID", as
     * this comment used to claim: ECS_COMPONENT_DEFINE sets use_low_id, so every
     * RENDER-path component lands below 256 (JceMeshRenderer is registration #5,
     * JceEditorMeta #28) and takes the inline table->component_map[id] path.
     * The cost was the NUMBER of probes, not the cost of each.  (Components
     * built via ecs_entity_init with a zeroed desc -- the net-replication ones
     * in jce_network_variable.c and jce_gas_replication.c -- do land above 256
     * and do take the hashmap, but sr_loop never touches them.)
     * kc_enabled mirrors entity_enabled(); render_kind is one of SR_RK_*.  Only
     * trusted when sr->kindcache_on (JCE_DISABLE_KINDCACHE=1 turns it off, so the
     * passes fall back to their own probes — an exact A/B bisect). */
    bool     kc_enabled;
    uint8_t  render_kind;   /* SR_RK_* */
    bool     kc_has_light;  /* has dir/point/spot light → light gather+select skip
                             * the O(N) sparse scan over non-light entities */
    /* Parallel-gather eligibility (concurrent-ECS opt-in): true when this entity
     * is a kc_fast PRIM_MESH whose color-pass material build is a PURE function
     * of read-only state (factor-only material — no textures, no custom shader
     * program, no video) so it can be built on a worker thread (ecs_get_id reads
     * under flecs readonly mode + disjoint cmd write). Set in sr_classify_entity
     * where mr_comp is already resolved. Everything not eligible stays serial. */
    bool     parallel_eligible;
    /* Per-cascade CSM gates: this entity is a shadow caster that mutates in
     * place (rigidbody/character/skeletal/... — jce_scene_entity_is_dynamic).  Set on
     * the ecull miss path; listed in sr->shadow_dyn so the shadow pass can
     * test WHICH cascades a mover overlaps instead of voiding all of them. */
    bool     is_dyn_caster;
    /* JceLayerComponent.layer (0..31), READ FRESH EVERY FRAME on both the hit
     * and the miss path -- unlike render_kind beside it.  A layer edit changes
     * a component VALUE and bumps no epoch, so a cached one would go stale. */
    uint8_t  layer;
} SrEntityCull;

/* SrEntityCull.render_kind values.  PRIM_MESH = a plain mesh-renderer with a
 * non-glTF (primitive shape or shared .mesh) mesh AND none of the diverting
 * components (terrain/tilemap/vegetation/line/trail/water/grass/skinned/sprite/
 * billboard/video/lod_group) — every pass's type-dispatch + skinned/terrain
 * probes are provably false for it, so they jump straight to the mesh submit. */
enum { SR_RK_OTHER = 0, SR_RK_PRIM_MESH = 1, SR_RK_DISABLED = 2, SR_RK_MODEL = 3 };

/* Parallel-gather worker output: one pre-built draw cmd + its material per
 * eligible entity, written to a DISJOINT slot by a worker thread (no shared
 * mutable state), then registered + pushed to the render queue serially. */
typedef struct {
    JceDrawCmd     cmd;
    JcePbrMaterial pbr;
    JceMesh       *mesh;
    bool           ok;
    /* The receiver layer this cmd's material_key was built with: the draw-cmd
     * cache keeps this struct, and nothing else would tell a hit that the
     * entity changed layer. */
    uint8_t        layer;
} SrPgCmd;
/* PRIM_MESH and MODEL are both "fast" kinds: a plain non-skinned, non-LOD mesh
 * entity with none of the diverting components, so every pass skips the type-
 * dispatch + skinned/terrain probes and lets the existing mesh/model submit (the
 * code AFTER the ladder) run unchanged.  The ONLY per-pass difference is the
 * depth-velocity pre-pass, which still routes MODEL through the multi-primitive
 * model-velocity helper (so kc_prim, not kc_fast, gates that one). */
#define SR_RK_IS_FAST(rk) ((rk) == SR_RK_PRIM_MESH || (rk) == SR_RK_MODEL)

/* Cross-frame persistent world-matrix + AABB cache for STATIC entities
 * (large-world-opt M1 #2).  Keyed by entity id (open-addressed linear probe);
 * an entry survives across frames so a static building/road/tree/lamp keeps its
 * composed world matrix + world AABB instead of recomposing the parent chain +
 * re-transforming 8 corners every frame.  Validity is gated on the scene's
 * structural epoch (bumped on any set_transform / reparent / component edit) AND
 * a per-entity "static" predicate (no rigidbody / character / skeletal / vehicle
 * / wheel / softbody in the entity or its ancestor chain — those are mutated
 * in-place by the runtime physics sync without an epoch bump, so they are never
 * cached here and always recomputed).  Entries untouched for a frame are pruned
 * so the table can't grow without bound in a streaming world. */
typedef struct {
    uint32_t entity;          /* 0 = empty slot */
    uint64_t structural_epoch;/* scene structural epoch this entry was valid for */
    uint64_t xform_gen;       /* per-entity transform gen at cache time (dynamic-opt:
                               * a hit requires BOTH epoch AND this to match, so a
                               * moving entity invalidates only itself, not the scene) */
    jce_mat4 world;
    jce_vec3 wmin, wmax;
    bool     world_valid;
    bool     has_aabb;
    bool     casts_shadow;
    /* Fix#1 (rank-1): cached render-kind classification so a static hit skips the
     * ~16 ECS has_* probes in sr_classify_entity.  render_kind/kc_has_light derive
     * from component PRESENCE (epoch-gated → safe to cache); kc_enabled is an
     * EditorMeta VALUE that can toggle without an epoch bump, so the hit path
     * re-validates it per-frame and reclassifies only on a flip. */
    uint8_t  render_kind;     /* SR_RK_* */
    bool     kc_enabled;
    bool     kc_has_light;
    bool     parallel_eligible; /* PRIM_MESH factor-only → worker-buildable cmd */
    bool     used;
    bool     touched;         /* set when hit/stored this frame; prune untouched */
    /* Lever ③ persistent draw-cmd cache (opt-in JCE_DRAWCMD_CACHE, default OFF).
     * For a static opaque PRIM_MESH parallel-eligible non-dynamic entity the
     * built SrPgCmd (mesh + material + FNV key) is bit-stable frame to frame — a
     * hit skips sr_pg_build_one (mesh+texture resolve + pbr assembly + FNV).
     * Validity: dc_valid AND this entry's {structural_epoch,xform_gen} (already
     * gated by sr_wcache_find) AND dc_material_gen (MeshRenderer set / async
     * pop-in, per-entity) AND dc_content_gen (frame-global SSAO/view-mode state).
     * On a hit only dc_cmd.cmd.transform is refreshed from the current world.
     * Zero behavioural cost when the lever is off (fields never touched). */
    bool     dc_valid;
    uint64_t dc_material_gen;
    uint64_t dc_content_gen;
    SrPgCmd  dc_cmd;
} SrWorldCacheEntry;

/* Per-entity LOD hysteresis state (large-world-opt P1 #6).  Open-addressed
 * table entry: `value` is (selected_level + 1), 0 == empty/no record.  See
 * JceSceneRenderer::lod_state for the rationale (replaces a colliding fixed
 * 1024-mask). */
typedef struct SrLodStateEntry {
    uint32_t entity;   /* 0 = empty slot */
    uint8_t  value;    /* (level + 1); 0 = no record */
    bool     touched;  /* referenced this frame; untouched entries are pruned */
} SrLodStateEntry;

/* Per-entity streaming-fade state (Direction B; dithered detail fade-in).
 * Mirrors SrLodStateEntry's open-addressed-table pattern: records the renderer
 * phase-clock time (sr->fade_time) at which the entity was FIRST seen in the
 * color pass, so the dithered cross-fade can ramp fade = (now-first_seen)/dur.
 * Pruned to the live touched set each render so it tracks the streamed set and
 * a despawned-then-respawned cell re-fades.  Untouched until a draw records a
 * first-seen time => zero overhead when nothing is streaming in. */
typedef struct SrFadeStateEntry {
    uint32_t entity;     /* 0 = empty slot */
    float    first_seen; /* sr->fade_time when first drawn (seconds) */
    bool     touched;    /* referenced this frame; untouched entries are pruned */
    /* Fade finished: the entry's answer is permanently 1.0 and only its
     * existence still matters — it is what stops a re-appearing entity from
     * fading in a second time.  Finished entries are therefore immune to the
     * prune, which is what lets the per-frame "touch every resident entity"
     * walk go away.  See sr_fade_state_prune. */
    bool     done;
} SrFadeStateEntry;

typedef struct SrFullscreenEffectSlot {
    JceEntity entity;
    JceFullscreenEffectPipeline *pipeline;
} SrFullscreenEffectSlot;

#endif /* JCE_SR_TYPES_H */
