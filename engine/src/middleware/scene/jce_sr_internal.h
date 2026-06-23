/* jce_sr_internal.h  Internal shared state for the scene-renderer modules.
 *
 * Holds the JceSceneRenderer struct + the Sr* helper types and the engine
 * includes they need, so the (formerly monolithic) renderer can be split
 * across jce_sr_*.c translation units that all share ONE definition of the
 * renderer state.  Internal to the renderer implementation — NOT a public
 * header and never installed.  (Extracted from jce_scene_renderer.c.)
 */
#ifndef JCE_SR_INTERNAL_H
#define JCE_SR_INTERNAL_H


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
#include <jce/middleware/physics/jce_cloth.h>
#include <jce/middleware/scene/jce_space_partition.h>
#include <jce/middleware/scene/jce_terrain.h>
#include <jce/middleware/scene/jce_foliage.h>
#include <jce/middleware/scene/jce_water_fft.h>
#include <jce/middleware/scene/jce_component_registry.h>
#include <jce/middleware/scene/jce_tilemap.h>
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_frustum.h>
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_csm.h>
#include <jce/renderer/jce_debug_draw.h>
#include <jce/renderer/jce_ibl.h>
#include <jce/os/core/jce_thread.h>   /* async IBL bake worker */
#include <jce/os/core/jce_jobs.h>     /* data-parallel frustum cull */
#include <jce/os/core/jce_console.h>  /* r.forwardplus cvar toggle */
#include <jce/renderer/jce_particles.h>
#include <jce/renderer/jce_gpu_particles.h>
#include <jce/renderer/jce_lighting.h>
#include <jce/renderer/jce_lighting_system.h>
#include <jce/renderer/jce_forwardplus.h>
#include <jce/renderer/jce_material.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_skinned_mesh.h>
#include <jce/renderer/jce_local_shadow.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_gpu_scene.h> /* GPU-driven rendering (roadmap #18) */
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_ssao.h>      /* screen-space ambient occlusion */
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

/* Animation retargeting (internal src-side header): play a clip authored for a
 * DIFFERENT (source) skeleton on the entity's own (dst) skeleton.  engine/src
 * is on every engine layer's private include path. */
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
#define SR_ANIM_INSTANCE_MAX   64   /* per-entity skinned-anim playback state */
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
#define SR_TEX_CACHE_MAX       512
#define SR_TEX_MAX_INFLIGHT    6    /* concurrent async texture decodes */
/* Render-list cap = max entities considered per frame.  Sized to cover a
 * full-load (non-streamed) big world so pulling the camera back surveys the
 * WHOLE world instead of an arbitrary first-N subset.  The list lives in BSS
 * (static in jce_scene_renderer_render), not on the stack, so this size is
 * free of stack-overflow risk; the per-frame cost is the O(n) collect + cull,
 * which the instanced passes keep cheap. */
#define SR_MAX_ENTITIES        32768
#define SR_MAT_CACHE_MAX       512
#define SR_MAT_PROG_CACHE_MAX  64   /* per-path Shader Graph custom programs */
/* Distinct viewport identities that drive this shared renderer in one frame
 * (editor Game + Scene viewports today).  Each keeps its own previous-frame
 * view*proj for correct, order-independent per-view TAA motion vectors. */
#define JCE_SR_VIEWPORT_SLOTS  4
#define SHADOW_ORTHO_SIZE      50.0f
#define CSM_DIST_SCALE         512.0f
#define CSM_DIST_MAX           1200.0f
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
#define SR_WATER_GRID_RES       64                 /* quads per side        */
#define SR_WATER_SLOT_MAX       16                 /* == water_cache[] size */

/* Local (spot/point) shadow atlas — P1. A square atlas packs up to
 * JCE_MAX_LOCAL_SHADOWS perspective depth tiles in a NxN grid; each
 * shadow-casting local light renders into one tile via its own bgfx view
 * (view_id_base + 4 + slot). v1 wires SPOT lights; point lights TODO. */
#define JCE_MAX_LOCAL_SHADOWS  4
#define JCE_LOCAL_SHADOW_TILES 2   /* 2x2 grid -> 4 tiles */
#define JCE_VIEW_LOCAL_SHADOW_OFFSET 4 /* base+4..base+8, free for base 0/3/80 */
/* GPU particle COMPUTE view (simulate + emit dispatches; no draws).  base+9
 * is the last free slot below the shadow band; the view-order builder pushes
 * it ahead of base+0 so dispatches execute before the draw that consumes the
 * pool. One JceGpuParticleSystem per GPU-flagged emitter (size/color lerp
 * comes from global uniforms per update call, so a pool cannot be shared). */
#define JCE_VIEW_GPU_PARTICLE_OFFSET 9
#define SR_GPU_PARTICLE_MAX          64
/* Point lights are omnidirectional; v1 approximates with a single wide-FOV
 * perspective frustum aimed straight down (good for elevated point lights,
 * weaker for ground-level ones). ~126deg. dual-paraboloid/cube is a future upgrade. */
#define JCE_POINT_SHADOW_FOV   2.2f

/* ── Internal struct ──────────────────────────────────────────────── */

/* Async IBL bake job (own: jce_sr_environment.c).  Defined here — not privately
 * in the environment module — because jce_scene_renderer_destroy (core) tears
 * down an in-flight bake by dereferencing ->result, so both TUs need the full
 * type.  The worker writes `result` + flips `done`; the main thread joins,
 * uploads + swaps (sr_ibl_poll). */
struct SrIblJob {
    float         *pixels;  /* owned copy of equirect; freed by the worker */
    uint32_t       w, h, irr, pf;
    JceIblCpuData *result;  /* set by the worker before `done` is raised */
    volatile int   done;    /* 0 = running, 1 = finished (publish barrier=join) */
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
    JceThread      *thr;      /* decode worker */
    struct SrModelJob *job;   /* worker job (owns done flag + cpu result) */
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
    float             morph_last_weights[JCE_MORPH_MAX_WEIGHTS];
    int               morph_last_count;   /* -1 = never uploaded (force first) */

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
} SrAnimInstance;

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
} SrInstEntry;

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
    /* #8 — composed world matrix cached ONCE per frame (ecull build loop) so the
     * shadow cascades / depth prepass / color submit paths read it instead of
     * re-walking the parent chain (~5× jce_scene_get_world_matrix per entity per
     * frame).  world_valid mirrors "entity has a transform" (false => callers
     * fall back to jce_scene_get_world_matrix; e.g. transform-less specials). */
    jce_mat4 world;
    bool     world_valid;
} SrEntityCull;

struct JceSceneRenderer {
    JceRenderer            *renderer;
    const JcePakArchive    *pak;
    JceSceneRendererCallbacks cbs;
    bool                    has_cbs;
    bool                    homogeneous_depth;
    /* When true (Play), a bound animation state machine drives active_clip.
     * When false (editor preview), the SM is left idle so manual clip selection
     * in the Inspector / Animation Editor previews normally. */
    bool                    anim_sm_active;

    /* Sky shader + uniforms. */
    bgfx_program_handle_t   prog_sky;
    bgfx_vertex_layout_t    sky_layout;
    bgfx_uniform_handle_t   u_sky_colors;
    bgfx_uniform_handle_t   u_sky_params;
    bgfx_uniform_handle_t   u_sky_equirect;
    /* Preetham analytic sky (fs_sky.sc mode 2). */
    bgfx_uniform_handle_t   u_sky_perez;     /* vec4[4]: Y/x/y A..D + E pack */
    bgfx_uniform_handle_t   u_sky_zenith;    /* vec4: Yz,xz,yz,normalize     */
    bgfx_uniform_handle_t   u_sky_sun_dir;   /* vec4: sun dir (toward sun)   */

    /* Procedural meshes. */
    JceMesh                *cube_mesh;
    JceMesh                *plane_mesh;
    JceMesh                *sphere_mesh;
    JceMesh                *capsule_mesh;
    JceMesh                *cylinder_mesh;

    /* Fallback textures. */
    bgfx_texture_handle_t   white_tex;
    bgfx_texture_handle_t   checker_tex;     /* magenta/yellow "missing" pattern */

    /* Legacy lighting uniforms. */
    bgfx_uniform_handle_t   u_light_dir;
    bgfx_uniform_handle_t   u_light_color;

    /* Optional time-of-day override (driven by jce_time_of_day_evaluate). */
    bool                    tod_active;
    JceTimeOfDayState       tod_state;

    /* Preetham analytic sky frame state (captured from the scene's
     * rendering settings each frame, consumed by sr_draw_sky_gradient). */
    int                     sky_mode;        /* JceSkyMode (0=gradient)      */
    float                   sky_turbidity;   /* Preetham haze, [1,10]        */

    /* Optional editor-supplied ambient override. When active and ToD is
     * inactive, replaces the renderer's hardcoded ambient before lights
     * are gathered each frame. */
    bool                    ambient_override_active;
    jce_vec3                ambient_override_color;
    float                   ambient_override_intensity;

    /* Shadow map resources. */
    bgfx_texture_handle_t      shadow_tex;
    bgfx_frame_buffer_handle_t shadow_fbo;
    bgfx_uniform_handle_t      u_shadowMap;
    bgfx_uniform_handle_t      u_shadowVP;
    bool                       shadow_valid;
    bool                       shadow_use_csm;

    /* SSAO (screen-space AO).  Camera depth pre-pass RT + the SSAO subsystem.
     * Lazily created when a scene enables SSAO; off => all UINT16_MAX/NULL and
     * the frame path is byte-identical.  The SSAO result is bound into the AO
     * sampler stage of the PBR pass (u_ssaoParams drives screen-UV sampling). */
    bgfx_texture_handle_t      ssao_depth_tex;
    bgfx_frame_buffer_handle_t ssao_depth_fbo;
    uint16_t                   ssao_w;
    uint16_t                   ssao_h;
    bool                       ssao_valid;       /* depth RT allocated */
    JceSsao                   *ssao;             /* SSAO sampling+blur pipeline */
    bool                       ssao_active_frame;/* true => bind SSAO into AO stage this frame */
    uint16_t                   ssao_ao_idx;      /* SSAO result texture handle idx */
    bgfx_uniform_handle_t      u_ssao_params;    /* vec4: x=enabled, yz=1/w,1/h */

    /* SSR normal G-buffer: the depth pre-pass also writes world-normal+roughness
     * (color attachment) for primitive/static meshes so SSR uses real normals +
     * per-material roughness instead of depth-reconstructed normals. */
    bgfx_texture_handle_t      ssao_normal_tex;  /* rgb=normal*.5+.5, a=roughness */
    bgfx_program_handle_t      prog_gbuffer;     /* vs_gbuffer + fs_gbuffer */
    bool                       gbuffer_prog_tried;
    bgfx_uniform_handle_t      u_gbuffer_mat;    /* x = roughness */

    /* TAA per-object MOTION VECTOR G-buffer: a THIRD color attachment on the
     * depth pre-pass FBO (RGBA16F).  When TAA is on, the prepass writes real
     * per-object (and per-bone, for skinned) screen motion into RG using the
     * SAME encoding as fs_motion_vec.sc ((curNDC-prevNDC)*0.5+0.5), so the TAA
     * resolve can reproject moving/animated geometry without ghosting.  Only
     * allocated when the velocity attachment is requested (taa_want_velocity);
     * UINT16_MAX / unused when TAA is off => byte-identical legacy path. */
    bgfx_texture_handle_t      ssao_velocity_tex; /* rg=(curNDC-prevNDC)*.5+.5 */
    bool                       ssao_has_velocity; /* fbo created WITH velocity RT */
    bool                       taa_want_velocity; /* TAA on this frame => write velocity */
    bool                       velocity_valid_frame; /* velocity written this frame */
    bgfx_program_handle_t      prog_gbuffer_vel;     /* vs_gbuffer_vel + fs_gbuffer_vel (static) */
    bgfx_program_handle_t      prog_gbuffer_vel_skinned; /* skinned velocity+normal */
    bool                       gbuffer_vel_prog_tried;
    bgfx_uniform_handle_t      u_prevModel;          /* mat4: prev-frame world (static) */
    bgfx_uniform_handle_t      u_prevViewProj;       /* mat4: prev-frame un-jittered view*proj */
    bgfx_uniform_handle_t      u_curViewProj;        /* mat4: this-frame un-jittered view*proj */
    bgfx_uniform_handle_t      u_prevBones;          /* mat4[JCE_MAX_BONES]: prev world palette */
    float                      cur_view_proj[16];    /* this render's clean view*proj */
    float                      frame_prev_vp[16];    /* prev VP for THIS render's velocity ctx */
    /* PER-VIEWPORT previous-frame clean view*proj.  The editor renders the Scene
     * + Game viewports (different cameras) through this one shared renderer each
     * frame; a single shared prev would be clobbered by the other viewport's
     * camera, giving a wrong TAA reprojection (translucent trail).  Indexed by
     * cfg->viewport_id so each viewport reprojects against ITS OWN last frame —
     * order-independent, the correct per-view motion vectors for TAA. */
    float                      prev_view_proj_cache[JCE_SR_VIEWPORT_SLOTS][16];
    bool                       prev_view_proj_valid[JCE_SR_VIEWPORT_SLOTS];
    /* Per-FRAME generation counter (bumped once per frame by the editor via
     * jce_scene_renderer_begin_velocity_frame).  The editor renders BOTH the
     * Scene and Game viewports through this single shared renderer every frame;
     * skin_anim_gen gates the skinned-animation sample + WORLD-SPACE prev-palette
     * snapshot to once per frame (first viewport) so the per-bone motion vectors
     * aren't double-advanced / zeroed.  (The prev view*proj is per-viewport above,
     * not gen-gated.) */
    uint32_t                   vel_frame_gen;
    uint32_t                   skin_anim_gen;
    bool                       velocity_frame_driven; /* editor calls begin_velocity_frame: gate anim once/frame */
    /* Camera frustum planes for the depth/velocity pre-pass.  That pass is
     * screen-space (SSAO/SSR/TAA-velocity), so off-screen entities contribute
     * nothing — culling them keeps the per-frame uniform writes (and bgfx's
     * Vulkan uniform scratch buffer) bounded on large scenes (else the scratch
     * buffer overflows -> crash).  Set once per pre-pass in sr_draw_depth_prepass. */
    jce_vec4                   prepass_cull_planes[6];
    SrPrevXform                prev_xform[SR_PREV_XFORM_MAX]; /* per-entity prev world */

    /* SSR (screen-space reflections).  Shares the SSAO depth pre-pass; reads
     * the lit color (cfg->ssr_color_tex_handle) + reconstructs normals from
     * depth; the result is composited by the editor via
     * jce_scene_renderer_composite_ssr(). */
    JceSsr                    *ssr;
    bool                       ssr_active_frame; /* result valid this frame */
    uint16_t                   ssr_result_idx;   /* SSR reflection RT handle idx */
    uint16_t                   shadow_map_size;
    bgfx_texture_format_t      shadow_depth_fmt;
    bool                       shadow_far_valid;
    float                      shadow_far_cached;

    /* CSM resources. */
    uint32_t                   csm_cascade_count;
    bgfx_texture_handle_t      csm_tex[JCE_CSM_MAX_CASCADES];
    bgfx_frame_buffer_handle_t csm_fbo[JCE_CSM_MAX_CASCADES];
    bgfx_uniform_handle_t      u_csm_samplers[JCE_CSM_MAX_CASCADES];
    bgfx_uniform_handle_t      u_csm_vp;
    bgfx_uniform_handle_t      u_csm_splits;
    bgfx_uniform_handle_t      u_csm_params;
    bgfx_uniform_handle_t      u_csm_bias_scales;
    /* Shadow FILTER tier (x lane; see JceRenderPipelineDesc
       .shadow_filter_quality). Frame-constant uniform branch in
       fs_pbr/fs_terrain selecting 1-tap / 3x3 / full PCF — coherent for
       every fragment of a draw, so the untaken side's texture fetches are
       genuinely skipped on SM3+ hardware. */
    bgfx_uniform_handle_t      u_shadow_quality;
    float                      shadow_filter_tier; /* cached per frame */
    bool                       csm_valid;
    float                      csm_blend_ratio;
    float                      csm_normal_bias;
    float                      csm_filter_radius;
    JceCsmData                 last_csm;
    bool                       last_csm_valid;
    /* Cached inputs to jce_csm_compute(); when unchanged frame-to-frame the
       recompute is skipped and last_csm is reused (static camera + light). */
    struct {
        bool      valid;
        jce_mat4  view;
        jce_vec3  light_dir;
        float     znear, zfar, fov, aspect, lambda;
        uint32_t  cascades, map_size;
        bool      homog;
        bool      caster_valid;
        jce_vec3  caster_min, caster_max;
    }                          csm_key;

    /* Local (spot/point) shadow atlas — P1. */
    bgfx_texture_handle_t      local_atlas_tex;
    bgfx_frame_buffer_handle_t local_atlas_fbo;
    bgfx_uniform_handle_t      u_local_shadow_map;    /* sampler stage 15 */
    bgfx_uniform_handle_t      u_local_shadow_vp;     /* MAT4[JCE_MAX_LOCAL_SHADOWS] */
    bgfx_uniform_handle_t      u_local_shadow_params; /* x=tiles/side y=1/atlas z=bias w=texel */
    bgfx_uniform_handle_t      u_local_shadow_bias;   /* VEC4: per-slot depth bias (lane=slot) */
    bgfx_uniform_handle_t      u_spot_shadow_slot;    /* VEC4: lane i = slot for spot i (-1=none) */
    bgfx_uniform_handle_t      u_point_shadow_slot;   /* VEC4[2]: 8 point lanes (-1=none) */
    bool                       local_atlas_valid;
    /* Per-frame local-shadow state, filled by sr_draw_local_shadow_pass.
       Spot + point lights share ONE atlas slot pool (max JCE_MAX_LOCAL_SHADOWS). */
    jce_mat4                   frame_local_vp[JCE_MAX_LOCAL_SHADOWS];
    float                      frame_local_bias_slot[JCE_MAX_LOCAL_SHADOWS]; /* per-slot depth bias */
    float                      frame_spot_slot[JCE_MAX_SPOT_LIGHTS];   /* spot i -> slot or -1 */
    float                      frame_point_slot[JCE_MAX_POINT_LIGHTS]; /* point j -> slot or -1 */
    uint32_t                   frame_local_count;
    float                      frame_local_bias;
    bool                       frame_local_active;

    /* Distance/importance light selection (Unity-style): the most important
       point/spot lights for the current view. Both the light gather AND the
       local-shadow producer skip lights not in these sets, so when a scene has
       more lights than the cap the NEAREST/brightest are kept (not an arbitrary
       ECS-order suffix), and the two passes stay index-aligned because they
       apply the SAME membership filter while iterating `list` in order. */
    JceEntity                  frame_sel_point[JCE_MAX_POINT_LIGHTS];
    uint32_t                   frame_sel_point_n;
    JceEntity                  frame_sel_spot[JCE_MAX_SPOT_LIGHTS];
    uint32_t                   frame_sel_spot_n;

    /* Per-frame culling stats (updated each render). */
    uint32_t                   stat_total_entities;
    uint32_t                   stat_visible_entities;
    uint32_t                   stat_culled_entities;
    bool                       stat_culling_enabled;

    /* World-space bounds of all drawn entities, captured during the cull pass
     * (sr_compute_visible).  Fed to jce_csm_compute so the directional-shadow
     * cascades extend their near plane to enclose tall casters (fixes shadow
     * truncation).  One frame stale (cull runs after the shadow pass) — fine
     * since scene bounds barely change frame-to-frame. */
    jce_vec3                   scene_world_min;
    jce_vec3                   scene_world_max;
    bool                       scene_world_valid;

    /* Persistent cull-space (uniform grid). Reused every frame; reset
     * + re-insert each draw to amortise allocation overhead. */
    JceSpaceIndex             *cull_space;
    JceAABB                   *cull_aabbs;
    struct SrCullPrep         *cull_prep;   /* Pass-A scratch for parallel cull */
    uint32_t                   cull_aabb_cap;

    /* #6 — shadow-caster spatial grid.  Built ONCE per frame (from the ecull
     * cache AABBs) before the CSM cascade loops so each cascade gathers only the
     * casters overlapping its (conservative) world bounds instead of scanning
     * the full entity list per cascade.  Separate from cull_space (which the
     * color pass rebuilds with camera planes later in the same frame) so neither
     * disturbs the other.  shadow_noaabb_* lists casters with no resolvable AABB:
     * those are visited unconditionally by every cascade (never broadphased). */
    JceSpaceIndex             *shadow_space;
    bool                       shadow_space_valid;
    uint32_t                  *shadow_noaabb;        /* ecull indices, count below */
    uint32_t                   shadow_noaabb_count;
    uint32_t                   shadow_noaabb_cap;
    uint32_t                  *shadow_query;         /* per-cascade query results */
    uint32_t                   shadow_query_cap;

    /* Optional global LOD group. When non-NULL every entity with a
     * resolved mesh has its mesh replaced by the pick at draw time.
     * MVP — per-entity LOD attachment lands when the ECS schema gains
     * a LodGroup component. */
    const JceLodGroup         *global_lod;
    /* Hash-bucketed previous-level memory for hysteresis. Aliases on
     * collision (harmless: at worst one frame of slightly-wrong level). */
    int8_t                     lod_prev[1024];
    uint32_t                   stat_lod_picks[JCE_LOD_MAX_LEVELS];
    uint32_t                   stat_lod_culled;
    bool                       stat_lod_enabled;

    /* Terrain per-chunk draw stats (P1-terrain-lod), reset each scene render. */
    uint32_t                   stat_terrain_chunks_total;
    uint32_t                   stat_terrain_chunks_drawn;
    uint32_t                   stat_terrain_chunks_culled;

    /* Render-queue stats — accumulated across all flushes this frame
     * (shadow + main mesh pass).  Reset at start of each scene render. */
    JceRenderQueueStats        stat_rq;
    bool                       stat_rq_active;

    /* Occlusion culling stats (from the most recent render). */
    JceOcclusionStats          stat_occlusion;
    bool                       stat_occlusion_enabled;

    /* Lighting environment. */
    JceLightEnv              *light_env;

    /* Forward+ clustered lighting (ROUND A — built + uploaded each frame but
     * DORMANT: no shader samples the cluster textures yet, so it has zero
     * effect on the rendered image.  Disabled by default; toggle with
     * jce_forwardplus_set_enabled.  Parallel scratch arrays describe the same
     * point+spot lights gathered for light_env, 1:1 with fp_proxies. */
    JceForwardPlus           *forwardplus;
    JceLightProxy            *fp_proxies;   /* geometry per cluster light */
    JceForwardPlusLightParam *fp_params;    /* shading per cluster light */
    uint32_t                  fp_max_lights;/* capacity of the two arrays */
    /* r.forwardplus console cvar (bool, default false).  Registered once at
     * create; read each frame -> jce_forwardplus_set_enabled +
     * jce_renderer_set_forwardplus_program_active so the console toggles the
     * clustered path live.  NULL if registration failed. */
    JceCvar                  *cv_forwardplus;
    /* True for the current frame when the clustered path is BOTH enabled AND
     * the fs_pbr_fwdplus variant program loaded — gates the per-submit
     * jce_forwardplus_bind in the material bind callbacks. */
    bool                      fp_active_frame;

    /* ── Temporal Anti-Aliasing (r.taa) ──────────────────────────────────
     * Persistent jitter + previous-camera state.  Zero-init valid.  Driven
     * by the editor/runtime caller via jce_scene_renderer_taa_begin_frame /
     * _end_frame around the main colour-pass matrix setup + postfx apply.
     * cv_taa is the opt-in console toggle (default OFF → no jitter, no TAA
     * passes → byte-identical to the legacy FXAA path).  NULL if registration
     * failed. */
    JceTaaState               taa_state;
    JceCvar                  *cv_taa;

    /* Skybox / IBL state. */
    JceSkybox               *skybox;
    JceIblData              *ibl_data;
    bgfx_texture_handle_t    brdf_lut;
    bgfx_uniform_handle_t    u_ibl_irradiance;
    bgfx_uniform_handle_t    u_ibl_prefilter;
    bgfx_uniform_handle_t    u_ibl_brdf_lut;
    bgfx_uniform_handle_t    u_ibl_params;
    char                     skybox_hdr_path[256];
    bool                     skybox_active;
    float                    skybox_exposure;
    float                    skybox_rotation;
    bool                     postfx_tonemap_active;
    /* ── Async IBL bake ───────────────────────────────────────────────
     * The irradiance+prefilter convolution runs on a worker thread so a
     * cache-miss does not freeze the main thread for several seconds. The
     * worker produces a CPU buffer; the main thread uploads it to GPU and
     * swaps it in when ready (the scene renders with fallback ambient until
     * then). See sr_scan_skybox. */
    struct SrIblJob         *ibl_job;        /* in-flight job, NULL = none */
    JceThread               *ibl_thread;     /* worker for ibl_job */
    char                     ibl_job_hdr[256]; /* HDR the in-flight bake is for */

    /* ── Baked GI consumption (P1-baked-gi-consume) ───────────────────
     * Reflection probe: a baked .ktx cubemap (+ .irr.ktx sidecar) loaded
     * lazily and bound into IBL sampler stages 6/7 to override the sky
     * prefilter for objects inside the dominant probe.
     * Light probe SH9: 9 RGB ambient coeffs from the nearest baked
     * LightProbeGroup, uploaded as u_sh9 and evaluated per-fragment. */
    struct {
        char                  path[256];     /* baked specular .ktx path */
        bgfx_texture_handle_t spec;          /* specular cubemap */
        bgfx_texture_handle_t irr;           /* irradiance sidecar cube */
        uint16_t              spec_mips;
        bool                  used;
        bool                  failed;        /* load attempted + failed */
    } rprobe_cache[8];
    int rprobe_cache_count;

    /* Per-frame dominant reflection probe (nearest to camera, baked). */
    bgfx_texture_handle_t    gi_probe_spec;
    bgfx_texture_handle_t    gi_probe_irr;
    float                    gi_probe_intensity;
    uint16_t                 gi_probe_spec_mips;
    bool                     gi_probe_active;

    /* Per-frame dominant light-probe SH9 (nearest baked group). */
    float                    gi_sh9[9][3];
    bool                     gi_sh9_active;

    bgfx_uniform_handle_t    u_sh9;       /* 9 vec4: SH9 RGB coeffs */
    bgfx_uniform_handle_t    u_gi_params; /* x=sh9 on, y=probe intensity */

    /* Sprite batch for 2D sprite entities. */
    JceSpriteBatch          *sprite_batch;

    /* Model / animation cache. */
    SrModelCache             model_cache[SR_MODEL_CACHE_MAX];
    int                      model_inflight;   /* concurrent async model decodes */
    SrAnimInstance           anim_inst[SR_ANIM_INSTANCE_MAX];

    /* Per-entity 2D sprite-animator playback state (sheet + player). Created
     * lazily when a SpriteAnimator entity is first ticked; rebuilt when its
     * sheet/atlas/grid authoring changes. */
    SrSpriteAnim             sprite_anim[SR_SPRITE_ANIM_MAX];

    /* Resolved-texture cache (path → JceTexture). Prevents per-frame
     * bgfx texture leaks. Cleared at destroy.
     *
     * Runtime mode loads asynchronously: a cache miss kicks a worker that
     * decodes the texture to CPU (PAK decompress + image decode), and the
     * render thread uploads it (sr_tex_poll) once ready.  While pending,
     * the entry's tex stays invalid so the draw falls back to white_tex —
     * the texture pops in within a few frames instead of stalling the
     * render thread on first sight. */
    struct {
        char            path[256];
        JceTexture      tex;
        bool            used;
        bool            failed;  /* tried, but loader returned invalid */
        bool            pending; /* async decode in flight */
        JceThread      *thr;     /* decode worker */
        struct SrTexJob *job;    /* worker job (owns done flag + result) */
    } tex_cache[SR_TEX_CACHE_MAX];
    int tex_cache_count;
    int tex_inflight;            /* concurrent async decodes */

    /* Shader Graph custom-program lookup cache (material_path → program).
     * A .mat.json may declare a graph-generated shader (customProgramVs /
     * customProgramFs); jce_pbr_material_load_json links it into
     * JcePbrMaterial.custom_program.  We cache the resolved handle here keyed
     * by material path so the per-frame draw avoids re-reading the .mat.json.
     * The PROGRAMS themselves are owned by the process-wide cache in
     * jce_pbr_material.c — this is references only, never destroyed here. */
    struct {
        char            path[256];
        JceShaderHandle program;   /* UINT16_MAX = no custom shader */
        bool            used;
        bool            resolved;  /* attempted load (cache the negative too) */
    } prog_cache[SR_MAT_PROG_CACHE_MAX];
    int prog_cache_count;

    /* PostFX pipeline (owned). */
    JcePostFXPipeline       *postfx_pipeline;

    /* Terrain mesh cache (path -> JceTerrain* + per-chunk JceMesh*).
     * Loaded on first use; terrain entities are drawn chunk-by-chunk
     * (sr_draw_terrain_chunks) with per-chunk frustum culling + distance LOD
     * so a large heightmap is no longer merged into one giant draw.  Each
     * chunk mesh is cached at the LOD it was first built for and rebuilt only
     * when its required LOD changes or the terrain is edited. */
    struct {
        char                  path[256];
        JceTerrain           *terrain;
        bgfx_texture_handle_t splat_tex;
        bool                  used;
        bool                  failed;
        bool                  splat_uploaded;

        /* Per-chunk LOD mesh cache. chunk_meshes[i] is the cached GPU mesh
         * for chunk index i (cz*chunk_count_x + cx); chunk_lod[i] records the
         * LOD it was built at (-1 = not built yet). chunk_min/max hold the
         * chunk's terrain-LOCAL AABB (pre-transform) for frustum culling. */
        int                   chunk_count;   /* ncx * ncz, 0 until loaded   */
        int                   chunk_nx;      /* chunk_count_x               */
        int                   chunk_nz;      /* chunk_count_z               */
        JceMesh             **chunk_meshes;  /* [chunk_count]               */
        int8_t               *chunk_lod;     /* [chunk_count], -1 = unbuilt */
        jce_vec3             *chunk_min;      /* [chunk_count] local AABB min*/
        jce_vec3             *chunk_max;      /* [chunk_count] local AABB max*/
    } terrain_cache[16];

    /* Vegetation scatter cache (P0 foliage): per-entity deterministic scatter
     * (jce_foliage_scatter) of an instanced mesh over the terrain.  Rebuilt only
     * when the component params change (param_hash) so per-frame cost is just N
     * model draws (GPU instancing is a perf follow-up).  Freed on destroy. */
    struct {
        bool                used;
        JceEntity           entity;
        char                mesh_path[256];
        uint32_t            param_hash;     /* seed/density/area/slope/scale */
        JceFoliageInstance *insts;
        uint32_t            inst_count;
        uint32_t            inst_cap;
    } foliage_cache[16];

    /* Terrain shader uniforms (created lazily on first terrain submit). */
    bgfx_uniform_handle_t u_terrain_params;
    bgfx_uniform_handle_t s_terrain_splat;
    bgfx_uniform_handle_t s_terrain_layer0;
    bgfx_uniform_handle_t s_terrain_layer1;
    bgfx_uniform_handle_t s_terrain_layer2;
    bgfx_uniform_handle_t s_terrain_layer3;

    /* ── Water (Gerstner surface, roadmap 2.3) ────────────────────────
     * One animated translucent grid per JceWaterComponent.  The grid mesh
     * (flat XZ plane, SR_WATER_GRID_RES²) is cached per entity and rebuilt
     * only when size_x/size_z change; the wave displacement + analytic normal
     * are evaluated on the GPU (vs_water.sc, the twin of jce_water.c).  The
     * water program + its uniforms are created lazily on first water submit.
     * water_time is a per-renderer phase clock advanced each render. */
    struct {
        bool      used;
        JceEntity entity;
        float     size_x;       /* size the cached grid was built for */
        float     size_z;
        JceMesh  *mesh;         /* flat grid; displaced in vs_water    */

        /* ── FFT ocean (Tessendorf) per-entity state (water_mode==FFT) ──
         * Created lazily on first FFT submit and rebuilt only when the FFT
         * params change.  fft_tex is a MUTABLE displacement texture (created
         * once, refreshed each frame with bgfx_update_texture_2d — mirrors the
         * Forward+ dynamic-texture lifecycle).  Both are torn down on eviction
         * and renderer destroy. */
        JceWaterFft           *fft;
        bgfx_texture_handle_t  fft_tex;
        int                    fft_res;        /* resolution fft_tex was sized for */
        float                  fft_patch_size;  /* params the fft was built for      */
        float                  fft_wind_speed;
        float                  fft_wind_dir_x;
        float                  fft_wind_dir_z;
        float                  fft_amplitude;
    } water_cache[16];
    bgfx_program_handle_t prog_water;
    bool                  water_prog_tried;   /* load attempted (success or not) */
    float                 water_time;         /* accumulated phase seconds */
    bgfx_uniform_handle_t u_water_wave_a;     /* vec4[4] amp,wavelen,speed,steep */
    bgfx_uniform_handle_t u_water_wave_b;     /* vec4[4] dir_x,dir_z,_,_          */
    bgfx_uniform_handle_t u_water_params;     /* x=count y=base_height            */
    bgfx_uniform_handle_t u_water_time;       /* x=time                           */
    bgfx_uniform_handle_t u_water_color_shallow;
    bgfx_uniform_handle_t u_water_color_deep;
    bgfx_uniform_handle_t u_water_shading;    /* x=transparency y=sun_specular    */
    bgfx_uniform_handle_t u_water_mode;       /* x=0 Gerstner / 1 FFT, y=patch_size */
    bgfx_uniform_handle_t s_water_disp;       /* FFT displacement texture (VS fetch) */

    /* Tilemap cache (path -> JceTilemapAsset + tileset + chunked static VBs).
     * Mirrors terrain_cache: loaded lazily on first draw, PAK-first, failed
     * latch, per-chunk frustum culling.  Cells are baked into 32x32-cell
     * chunks of textured quads in ENTITY-LOCAL space (1 cell = 1 unit, rows
     * grow down: cell (c,r) spans [c,c+1] x [-(r+1),-r]); the component tint
     * is baked into the vertex color (chunks rebuild when it changes).  Every
     * chunk indexes ONE shared static IB (1024 quads). */
    struct {
        char             path[256];          /* .tilemap.json cache key      */
        char             tileset_path[256];  /* resolved tileset key         */
        JceTilemapAsset *map;
        JceTilesetAsset *tileset;
        bool             used;
        bool             failed;
        uint32_t         baked_abgr;         /* tint baked into the VBs      */
        int              chunk_nx;           /* ceil(w/32)                   */
        int              chunk_ny;           /* ceil(h/32)                   */
        int              chunk_count;        /* chunk_nx * chunk_ny          */
        bgfx_vertex_buffer_handle_t *chunk_vb;    /* [count]; invalid=empty  */
        uint16_t        *chunk_quads;        /* [count] quads in each VB     */
        jce_vec3        *chunk_min;          /* [count] local AABB min       */
        jce_vec3        *chunk_max;          /* [count] local AABB max       */
        bool             chunks_built;
        bool             warned_bad_id;      /* id > rect_count (warn once)  */
        bool             warned_iso;         /* orientation != 0 (warn once) */
        bool             warned_src;         /* sourceW/H <= 0   (warn once) */
    } tilemap_cache[SR_TILEMAP_SLOT_MAX];
    bgfx_index_buffer_handle_t  tilemap_shared_ib;  /* 6144 u16 = 1024 quads */
    bgfx_uniform_handle_t       tilemap_s_tex;      /* "s_texColor" sampler  */
    bgfx_vertex_layout_t        tilemap_layout;     /* pos3f|color4u8|uv2f   */
    bool                        tilemap_layout_ready;

    /* ── Render-queue integration (Phase 2 stub) ─────────────────────
     * material registry is per-frame; reset at each sr_render begin.
     * frame_view_id / frame_shadow_vp let the binder rebuild bindings
     * without re-walking the scene. binder/queue wired in Phase 3. */
    JceRenderQueue   *render_queue;
    /* Separate back-to-front queue for alpha-blended (transparent)
     * materials.  Flushed after the opaque queue so transparency
     * composites over the solid scene in correct depth order. */
    JceRenderQueue   *transparent_queue;

    /* Per-frame per-entity cull cache (see SrEntityCull).  Heap-allocated once
     * (SR_MAX_ENTITIES entries) in create; rebuilt every frame (the scene is
     * editable so entities can move) in one O(n) pass that replaces ~6× redundant
     * per-entity world-AABB + model recomputes across the shadow/prepass cull sites. */
    SrEntityCull     *ecull;

    /* Color-pass GPU-instancing batch (see SrInstEntry).  Grown on demand;
     * reset at the start of each color entity walk, flushed at its end. */
    SrInstEntry      *inst_batch;
    uint32_t          inst_batch_count;
    uint32_t          inst_batch_cap;
    jce_mat4         *inst_gather;       /* contiguous per-model matrices for the instanced submit */
    uint32_t          inst_gather_cap;

    /* ── GPU-driven rendering (roadmap #18, Phase 0+1) ─────────────────
     * gpu_scene owns the persistent GPUScene buffer + compute cull program.
     * cv_gpu_driven is the r.gpu_driven console toggle (default off).
     * gpu_driven_frame is latched at the top of each render() from the config
     * field OR the cvar, AND gpu_scene support — it gates the divergence in
     * sr_inst_flush and the view-order builder.  gpu_rec is the per-flush
     * scratch array of GPUScene records (grown on demand). */
    JceGpuScene      *gpu_scene;
    JceCvar          *cv_gpu_driven;
    bool              gpu_driven_frame;
    JceGpuSceneRecord *gpu_rec;
    uint32_t          gpu_rec_cap;
    uint16_t          gpu_cull_view;     /* compute view for the cull dispatch */
    jce_vec4          gpu_frame_planes[6]; /* color-pass frustum planes (this frame) */
    bool              gpu_frame_planes_valid;
    uint32_t          gpu_scene_frame;       /* bgfx frame idx that claimed the GPU path */
    bool              gpu_scene_frame_valid;
    /* Per-flush GPU-driven run draw list: each eligible model-run's model, the
     * GPU visible-buffer partition base, the inst_batch source index (for the
     * CPU fallback if the dispatch fails), and the instance count.  Filled in
     * pass 1 (record build + cull batch), drawn in pass 2 after one dispatch. */
    struct { JceModel *model; uint32_t base; uint32_t src; uint32_t count; } *gpu_draw;
    uint32_t          gpu_draw_count;
    uint32_t          gpu_draw_cap;
    /* Shadow-pass GPU-instancing batch (separate from the color batch: the
     * shadow pass runs per cascade/light, so this auto-flushes whenever the
     * target shadow view changes, plus a final flush before the color pass).
     * Reuses inst_gather as scratch (flushes are synchronous, no overlap). */
    SrInstEntry      *sh_batch;
    uint32_t          sh_batch_count;
    uint32_t          sh_batch_cap;
    uint16_t          sh_batch_view;     /* shadow view the pending batch targets */
    SrMaterialEntry   mat_cache[SR_MAT_CACHE_MAX];
    uint32_t          mat_count;
    /* 1-entry memo for sr_bind_material_cb's linear mat_cache search. The
     * binder now runs once per SUBMIT (bind-per-submit, see jce_rq_flush),
     * and consecutive submits overwhelmingly share a material after the
     * queue's material-aware sort — the memo turns the common case into a
     * single compare. Self-validating (the indexed entry's key is checked),
     * so per-frame cache resets need no explicit invalidation. */
    uint32_t          bind_memo_key;
    uint32_t          bind_memo_idx;
    uint16_t          frame_view_id;
    JceScene         *frame_scene;
    float             frame_shadow_vp[16];
    bool              frame_shadow_vp_valid;
    bool              frame_shadow_active;

    /* Volumetric fog (lazily created when first enabled). */
    JceVolumetricFog *vfog;
    int               vfog_w;
    int               vfog_h;
    bool              vfog_last_rendered;

    /* ── Weather / decals / time-of-day (P2-weather-decals-tod) ───────
     * All three subsystems are created lazily on first use so a scene
     * that authors none of them allocates nothing.  weather + decals own
     * GPU resources (need the pak for their shaders); the ToD clock is a
     * pure-CPU driver feeding jce_scene_renderer_set_time_of_day. */
    JceWeatherSystem *weather;
    JceDecalPool     *decals;          /* runtime-stamped (API) decals    */
    JceDecalPool     *decals_authored; /* rebuilt each frame from JceDecalComponent */

    /* ── GPU particles (compute-driven; P3-E wiring) ──────────────────
     * One pool per GPU-flagged emitter.  gpu_particle_frame guards the
     * simulate/emit dispatch against the editor's multi-viewport double
     * render: only the FIRST scene render of a bgfx frame dispatches; later
     * renders in the same frame only re-submit the draw. */
    SrGpuParticleRec  gpu_particles[SR_GPU_PARTICLE_MAX];
    uint32_t          gpu_particle_frame;
    bool              gpu_particle_frame_valid;
    /* Internal advancing hour-of-day; seeded from the scene's authored
     * tod_hour the first frame ToD is enabled, then advanced by tod_speed.
     * tod_clock_valid gates the seed so editing tod_hour while frozen still
     * takes effect (we re-seed when the clock is inactive). */
    float             tod_clock_hour;
    float             tod_authored_hour;
    bool              tod_clock_valid;
    bool              tod_driven;     /* ToD currently driven from settings */

    /* Animation frame-event sink (P1 anim-events → gameplay). Default NULL =
       log-only (sr_anim_event_dispatch keeps its LOG_DEBUG). When set (the
       runtime points it at its entity→script dispatch), each fired event is
       forwarded to anim_event_fn(entity, ev, anim_event_user). Set via
       jce_scene_renderer_set_anim_event_fn. */
    JceSceneRendererAnimEventFn anim_event_fn;
    void                       *anim_event_user;

    /* Animation state-change sink (state-enter/exit → gameplay). Default NULL =
       the SM drives the pose with NO state events (and the per-instance state is
       not even polled). When set (the runtime points it at its entity→script
       dispatch), each SM active-state change is forwarded to
       anim_state_fn(entity, from_name, to_name, anim_state_user). Set via
       jce_scene_renderer_set_anim_state_fn. */
    JceSceneRendererAnimStateFn anim_state_fn;
    void                       *anim_state_user;

    /* Ground-query hook (Foot IK ground adaptation).  Default NULL =
       sr_apply_foot_ik is a NO-OP (no ground info -> pose byte-identical).
       The runtime points it at a physics raycast.  Set via
       jce_scene_renderer_set_ground_query_fn. */
    JceSceneRendererGroundQueryFn ground_query_fn;
    void                         *ground_query_user;
};

/* The per-frame entity list collected from the scene.  Defined here (instead of
 * privately in jce_scene_renderer.c) so the extracted jce_sr_*.c modules can
 * receive it across the module boundary with one shared definition. */
typedef struct {
    JceEntity entities[SR_MAX_ENTITIES];
    int       count;
} EntityList;

/* ── Shared internal renderer helpers (cross-module) ──────────────────
 * Helpers that were file-static in the monolithic jce_scene_renderer.c but are
 * now called across the split jce_sr_*.c translation units.  These keep their
 * original definitions (with `static` removed) in whichever module owns them;
 * the prototypes here give the other modules visibility.  Pure move + linkage:
 * no behaviour change. */

/* Frustum / AABB helpers (own: core). */
void  sr_extract_frustum_planes(const jce_mat4 *m, jce_vec4 planes[6]);
bool  sr_aabb_in_frustum(const jce_vec4 planes[6], jce_vec3 mn, jce_vec3 mx);
void  sr_transform_aabb(const jce_mat4 *m, jce_vec3 lmn, jce_vec3 lmx,
                        jce_vec3 *out_mn, jce_vec3 *out_mx);

/* Material bind helpers (own: core). */
void  sr_inline_bind_pbr_global(JceSceneRenderer *sr, const JcePbrMaterial *pbr,
                                uint16_t view_id, JceScene *scene, EntityList *list);

/* Terrain (own: jce_sr_terrain.c). */
int       sr_terrain_find_or_load_slot(JceSceneRenderer *sr, const char *path);
void      sr_terrain_free_chunks(JceSceneRenderer *sr, int slot);
JceMesh  *sr_terrain_chunk_mesh(JceSceneRenderer *sr, int slot,
                                int cx, int cz, int lod);
void      sr_draw_terrain_chunks(JceSceneRenderer *sr, JceScene *scene,
                                 EntityList *list,
                                 const JceCamera *camera, uint16_t view_id,
                                 int slot, JceTerrainComponent *tc,
                                 const jce_mat4 *model,
                                 const JcePbrMaterial *pbr,
                                 const bgfx_texture_handle_t layer_tex[4]);
bool      sr_try_submit_terrain_shadow(JceSceneRenderer *sr, JceScene *scene,
                                       JceEntity e, uint16_t view_id);

/* Particles (own: jce_sr_particles.c). */
void      sr_draw_particles(JceSceneRenderer *sr, JceScene *scene,
                            uint16_t view_id);
void      sr_drive_gpu_particles(JceSceneRenderer *sr, JceScene *scene,
                                 uint16_t view_id_base, float dt_sec);

/* Core caches / lookups (own: core jce_scene_renderer.c) used by every module. */
bool          entity_enabled(JceScene *scene, JceEntity e);
JceTexture    sr_resolve_texture(JceSceneRenderer *sr, const char *path);
JceTexture    sr_resolve_texture2(JceSceneRenderer *sr,
                                  const char *material_path,
                                  const char *mesh_path);
SrModelCache *sr_get_model(JceSceneRenderer *sr, const char *path,
                           uint32_t entity_id);

/* Environment (own: jce_sr_environment.c): vegetation, water, tilemap, sky,
 * cloth, async IBL bake + skybox scan, time-of-day / weather / decals. */
void  sr_draw_foliage(JceSceneRenderer *sr, JceScene *scene,
                      EntityList *list, JceEntity e, uint16_t view_id);
void  sr_draw_water(JceSceneRenderer *sr, JceScene *scene,
                    EntityList *list, JceEntity e, uint16_t view_id);
void  sr_water_slot_free(JceSceneRenderer *sr, int slot);
int   sr_tilemap_find_or_load_slot(JceSceneRenderer *sr,
                                   const JceTilemapComponent *tmc);
void  sr_tilemap_free_slot(JceSceneRenderer *sr, int i);
uint32_t sr_tilemap_color_abgr(const float c[4]);
void  sr_tilemap_build_chunks(JceSceneRenderer *sr, int slot, uint32_t abgr);
void  sr_draw_tilemap_chunks(JceSceneRenderer *sr, int slot,
                             const JceCamera *camera, uint16_t view_id,
                             const jce_mat4 *model);
void  sr_draw_sky_gradient(JceSceneRenderer *sr, uint16_t view_id);
void  sr_draw_cloth(JceSceneRenderer *sr, JceScene *scene, uint16_t view_id);
void  sr_scan_skybox(JceSceneRenderer *sr, JceScene *scene, EntityList *list);
void  sr_drive_time_of_day(JceSceneRenderer *sr,
                           const JceSceneRenderingSettings *rs, float dt_sec);
void  sr_drive_weather(JceSceneRenderer *sr,
                       const JceSceneRenderingSettings *rs,
                       uint16_t view_id, float dt_sec);
void  sr_drive_decals(JceSceneRenderer *sr, JceScene *scene,
                      uint16_t view_id, float dt_sec);

/* Animation (own: jce_sr_anim.c): morph VBs, sprite animator, avatar mask,
 * frame events, IK (two-bone / foot / full-body), ragdoll override,
 * retargeting + the per-frame skinned-animation evaluation pass. */
SrAnimInstance *sr_find_anim_instance(JceSceneRenderer *sr, uint32_t entity);
int       sr_find_sprite_anim(JceSceneRenderer *sr, uint32_t entity);
void      sr_free_morph_vbs(SrAnimInstance *a);
uint16_t  sr_morph_vb_cb(void *user, uint32_t node, uint32_t prim);
void      sr_update_sprite_anims(JceSceneRenderer *sr, JceScene *scene,
                                 EntityList *list, float dt_sec);
void      sr_update_skinned_anims(JceSceneRenderer *sr, JceScene *scene,
                                  EntityList *list, float dt_sec);

/* Cross-module helpers shared with the shadow / draw / cull modules
 * (own: core jce_scene_renderer.c, except where noted). */
void  sr_rq_flush_and_collect(JceSceneRenderer *sr);
const char *sr_mesh_renderer_model_path(JceScene *scene, JceEntity e);
bool  sr_build_entity_model(JceSceneRenderer *sr, JceScene *scene,
                            JceEntity e, int cull_idx, jce_mat4 *out_model,
                            JceMesh **out_mesh);
jce_vec3 sr_light_world_shine_direction(const jce_vec3 *comp_dir,
                                        const JceTransform *xf);
bool  sr_resolve_primary_dir_light(JceSceneRenderer *sr, JceScene *scene,
                                   EntityList *list, bool shadow_only,
                                   jce_vec3 *out_to_light, jce_vec3 *out_color,
                                   float *out_intensity);
/* Shadow-pass GPU-instancing batch (own: core/draw instancing). */
bool  sr_sh_batch_add(JceSceneRenderer *sr, JceModel *model,
                      const jce_mat4 *world);
void  sr_sh_flush(JceSceneRenderer *sr, uint16_t view_id);
/* Shadow-submit helpers (own: core; route skinned / model casters). */
bool  sr_try_submit_skinned_shadow(JceSceneRenderer *sr, JceScene *scene,
                                   JceEntity e, int cull_idx, uint16_t view_id);
bool  sr_try_submit_mesh_renderer_model_shadow(JceSceneRenderer *sr,
                                               JceScene *scene, JceEntity e,
                                               int cull_idx, uint16_t view_id);
/* Light selection membership (own: cull jce_sr_cull.c, promoted early). */
bool  sr_light_selected(const JceEntity *arr, uint32_t n, JceEntity e);

/* Shadow (own: jce_sr_shadow.c): caster cull, shadow VP/CSM helpers, shadow
 * target lifecycle, the directional + local shadow passes, the per-frame
 * shadow-caster spatial grid. */
bool  sr_shadow_caster_aabb(JceSceneRenderer *sr, JceScene *scene,
                            JceEntity e, const jce_mat4 *world,
                            jce_vec3 *out_mn, jce_vec3 *out_mx);
bool  sr_entity_casts_shadow(JceScene *scene, JceEntity e);
void  sr_bind_frame_shadow_state(JceSceneRenderer *sr);
void  sr_apply_view_order(uint16_t view_id_base,
                          const JceSceneRenderConfig *cfg,
                          uint32_t csm_cascade_count,
                          bool gpu_cull_view);
void  sr_destroy_shadow_targets(JceSceneRenderer *sr);
void  sr_create_shadow_targets(JceSceneRenderer *sr);
void  sr_ensure_shadow_map_size(JceSceneRenderer *sr, uint16_t size);
void  sr_draw_shadow_pass(JceSceneRenderer *sr, JceScene *scene,
                          const JceCamera *camera, EntityList *list,
                          uint16_t view_id_base, uint32_t vp_w,
                          uint32_t vp_h, float shadow_distance,
                          float split_lambda);
void  sr_draw_local_shadow_pass(JceSceneRenderer *sr, JceScene *scene,
                                EntityList *list, uint16_t view_id_base);

/* Mesh resolution (own: core jce_scene_renderer.c). */
JceMesh *sr_resolve_mesh(JceSceneRenderer *sr, const JceMeshRenderer *mr);

/* Cull / TAA-velocity / baked-GI (own: jce_sr_cull.c): the per-object TAA
 * previous-world-matrix table, the SSAO/velocity depth pre-pass, the
 * distance/importance light selection, the uniform-grid frustum cull, and the
 * baked-GI (reflection probe + SH9) consumption. */
void  sr_ensure_ssao_target(JceSceneRenderer *sr, uint16_t w, uint16_t h,
                            bool want_velocity);
void  sr_draw_depth_prepass(JceSceneRenderer *sr, JceScene *scene,
                            const JceCamera *camera, EntityList *list,
                            uint16_t view_id_base, uint32_t vp_w, uint32_t vp_h);
void  sr_select_lights(JceSceneRenderer *sr, JceScene *scene,
                       EntityList *list, const JceCamera *camera);
uint32_t sr_compute_visible(JceSceneRenderer *sr, JceScene *scene,
                            const EntityList *list, const jce_vec4 planes[6],
                            bool *visible);
void  sr_gather_baked_gi(JceSceneRenderer *sr, JceScene *scene,
                         const JceCamera *camera, EntityList *list);
void  sr_bind_baked_gi(JceSceneRenderer *sr, float ibl_params[4]);

/* Core helpers consumed by the draw module (own: core jce_scene_renderer.c). */
bool  sr_is_gltf_model_path(const char *path);
JceShaderHandle sr_resolve_custom_program(JceSceneRenderer *sr,
                                          const char *material_path);
void  sr_rq_flush_queue_and_collect(JceSceneRenderer *sr, JceRenderQueue *q);
/* Per-frame material registry (own: core; consumed by sr_draw_entities). */
void     sr_reset_material_cache(JceSceneRenderer *sr);
uint32_t sr_compute_material_key(const JcePbrMaterial *pbr, bool is_terrain,
                                 int terrain_slot,
                                 const bgfx_texture_handle_t *terrain_layer_tex);
uint32_t sr_register_material(JceSceneRenderer *sr, uint32_t key,
                              const JcePbrMaterial *pbr, bool is_terrain,
                              int terrain_slot, float terrain_tile_scale,
                              bool terrain_splat_enabled,
                              const bgfx_texture_handle_t *terrain_layer_tex);
void     sr_bind_material_cb(uint32_t material_key, void *user);

/* Draw (own: jce_sr_draw.c): the color-pass + shadow-pass GPU-instancing
 * batches, GPU-driven instanced draw, the per-entity model/material draw
 * helpers, and the main entity-rendering pass. */
void  sr_draw_entities(JceSceneRenderer *sr, JceScene *scene,
                       const JceCamera *camera, EntityList *list,
                       uint16_t view_id, float dt_sec,
                       const JceSceneRenderConfig *cfg);

#endif /* JCE_SR_INTERNAL_H */