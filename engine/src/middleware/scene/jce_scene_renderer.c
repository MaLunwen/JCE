/*
 * jce_scene_renderer.c  Engine scene renderer implementation.
 *
 * Adapted from editor's scene render code, factored to render a JceScene
 * directly with no editor-state coupling.
 *
 * ARCHITECTURE NOTE (Wave 8c Phase 3):
 * This implementation file includes middleware headers (scene, animation, LOD)
 * to access component data and iterate the ECS. However, the PUBLIC API in
 * jce_scene_renderer.h uses only opaque types (JceScene*, JceEntity) and
 * does NOT expose flecs or ECS implementation details. This satisfies
 * The-Forge layering: the renderer's public interface depends only on
 * renderer + resource layers; middleware dependency is an implementation detail.
 */

#include <jce/middleware/animation/jce_animation.h>
#include <jce/middleware/animation/jce_anim_sm_binding.h>
#include <jce/middleware/animation/jce_anim_blend_tree.h>
#include <jce/middleware/animation/jce_anim_ik.h>
#include <jce/middleware/scene/jce_lod.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/physics/jce_cloth.h>
#include <jce/middleware/scene/jce_space_partition.h>
#include <jce/middleware/scene/jce_terrain.h>
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_csm.h>
#include <jce/renderer/jce_debug_draw.h>
#include <jce/renderer/jce_ibl.h>
#include <jce/renderer/jce_particles.h>
#include <jce/renderer/jce_lighting.h>
#include <jce/renderer/jce_lighting_system.h>
#include <jce/renderer/jce_material.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_skinned_mesh.h>
#include <jce/renderer/jce_local_shadow.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_render_queue.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_occlusion_culler.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_skybox.h>
#include <jce/renderer/jce_sprite.h>
#include <jce/renderer/jce_sprite_batch.h>
#include <jce/renderer/jce_texture.h>
#include <jce/renderer/jce_views.h>
#include <jce/renderer/jce_volume_profile.h>
#include <jce/renderer/jce_decals.h>
#include <jce/middleware/world/jce_weather.h>
#include <jce/middleware/world/jce_time_of_day.h>

#include "jce_scene_renderer_view_order.h"
#include "jce_scene_internal.h"   /* particle system accessor (private) */

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

#define SR_MODEL_CACHE_MAX     32
#define SR_ANIM_INSTANCE_MAX   64   /* per-entity skinned-anim playback state */
#define SR_SPRITE_ANIM_MAX     64   /* per-entity 2D sprite-animator playback state */
#define SR_TEX_CACHE_MAX       512
#define SR_MAX_ENTITIES        4096
#define SR_MAT_CACHE_MAX       512
#define SR_MAT_PROG_CACHE_MAX  64   /* per-path Shader Graph custom programs */
#define SHADOW_ORTHO_SIZE      50.0f
#define CSM_DIST_SCALE         512.0f
#define CSM_DIST_MAX           1200.0f
#define CSM_DIST_MIN           50.0f
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

/* Local (spot/point) shadow atlas — P1. A square atlas packs up to
 * JCE_MAX_LOCAL_SHADOWS perspective depth tiles in a NxN grid; each
 * shadow-casting local light renders into one tile via its own bgfx view
 * (view_id_base + 4 + slot). v1 wires SPOT lights; point lights TODO. */
#define JCE_MAX_LOCAL_SHADOWS  4
#define JCE_LOCAL_SHADOW_TILES 2   /* 2x2 grid -> 4 tiles */
#define JCE_VIEW_LOCAL_SHADOW_OFFSET 4 /* base+4..base+8, free for base 0/3/80 */
/* Point lights are omnidirectional; v1 approximates with a single wide-FOV
 * perspective frustum aimed straight down (good for elevated point lights,
 * weaker for ground-level ones). ~126deg. dual-paraboloid/cube is a future upgrade. */
#define JCE_POINT_SHADOW_FOV   2.2f

/* ── Internal struct ──────────────────────────────────────────────── */

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
   once and shared by every entity that references the same file. */
typedef struct {
    char            path[256];
    JceModel       *model;
    bool            used;
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
    JceAnimSmBinding *sm_binding;  /* lazily created from the component's sm_path */
    char             sm_path[256]; /* path the binding was built from (detect change) */
    JceAnimBlendTree *blend_tree;  /* cached 1D tree; rebuilt when entries change */
    int               bt_count;    /* clip count the tree was built for */
    float             bt_thresh[8];/* thresholds the tree was built for */
    float             bt_time;     /* shared phase time advanced each frame */

    /* Generic motion-driven SM/blend params: planar speed from frame-to-frame
       Transform delta, fed into the SM as a "Speed" float (and blend_param)
       so any animated entity auto-switches state by how fast it moves. */
    jce_vec3          sm_prev_pos;
    bool              sm_have_prev;
    float             sm_speed;     /* smoothed (EMA) so the SM doesn't flicker */

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
} SrAnimInstance;

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
    uint16_t                   shadow_map_size;
    bgfx_texture_format_t      shadow_depth_fmt;
    bool                       shadow_near_valid;
    float                      shadow_near_cached;
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
    }                          csm_key;

    /* Local (spot/point) shadow atlas — P1. */
    bgfx_texture_handle_t      local_atlas_tex;
    bgfx_frame_buffer_handle_t local_atlas_fbo;
    bgfx_uniform_handle_t      u_local_shadow_map;    /* sampler stage 15 */
    bgfx_uniform_handle_t      u_local_shadow_vp;     /* MAT4[JCE_MAX_LOCAL_SHADOWS] */
    bgfx_uniform_handle_t      u_local_shadow_params; /* x=tiles/side y=1/atlas z=bias w=texel */
    bgfx_uniform_handle_t      u_spot_shadow_slot;    /* VEC4: lane i = slot for spot i (-1=none) */
    bgfx_uniform_handle_t      u_point_shadow_slot;   /* VEC4[2]: 8 point lanes (-1=none) */
    bool                       local_atlas_valid;
    /* Per-frame local-shadow state, filled by sr_draw_local_shadow_pass.
       Spot + point lights share ONE atlas slot pool (max JCE_MAX_LOCAL_SHADOWS). */
    jce_mat4                   frame_local_vp[JCE_MAX_LOCAL_SHADOWS];
    float                      frame_spot_slot[JCE_MAX_SPOT_LIGHTS];   /* spot i -> slot or -1 */
    float                      frame_point_slot[JCE_MAX_POINT_LIGHTS]; /* point j -> slot or -1 */
    uint32_t                   frame_local_count;
    float                      frame_local_bias;
    bool                       frame_local_active;

    /* Per-frame culling stats (updated each render). */
    uint32_t                   stat_total_entities;
    uint32_t                   stat_visible_entities;
    uint32_t                   stat_culled_entities;
    bool                       stat_culling_enabled;

    /* Persistent cull-space (uniform grid). Reused every frame; reset
     * + re-insert each draw to amortise allocation overhead. */
    JceSpaceIndex             *cull_space;
    JceAABB                   *cull_aabbs;
    uint32_t                   cull_aabb_cap;

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
    SrAnimInstance           anim_inst[SR_ANIM_INSTANCE_MAX];

    /* Per-entity 2D sprite-animator playback state (sheet + player). Created
     * lazily when a SpriteAnimator entity is first ticked; rebuilt when its
     * sheet/atlas/grid authoring changes. */
    SrSpriteAnim             sprite_anim[SR_SPRITE_ANIM_MAX];

    /* Resolved-texture cache (path → JceTexture). Prevents per-frame
     * bgfx texture leaks. Cleared at destroy. */
    struct {
        char       path[256];
        JceTexture tex;
        bool       used;
        bool       failed; /* tried, but loader returned invalid */
    } tex_cache[SR_TEX_CACHE_MAX];
    int tex_cache_count;

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

    /* Terrain shader uniforms (created lazily on first terrain submit). */
    bgfx_uniform_handle_t u_terrain_params;
    bgfx_uniform_handle_t s_terrain_splat;
    bgfx_uniform_handle_t s_terrain_layer0;
    bgfx_uniform_handle_t s_terrain_layer1;
    bgfx_uniform_handle_t s_terrain_layer2;
    bgfx_uniform_handle_t s_terrain_layer3;

    /* ── Render-queue integration (Phase 2 stub) ─────────────────────
     * material registry is per-frame; reset at each sr_render begin.
     * frame_view_id / frame_shadow_vp let the binder rebuild bindings
     * without re-walking the scene. binder/queue wired in Phase 3. */
    JceRenderQueue   *render_queue;
    /* Separate back-to-front queue for alpha-blended (transparent)
     * materials.  Flushed after the opaque queue so transparency
     * composites over the solid scene in correct depth order. */
    JceRenderQueue   *transparent_queue;
    SrMaterialEntry   mat_cache[SR_MAT_CACHE_MAX];
    uint32_t          mat_count;
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
    /* Internal advancing hour-of-day; seeded from the scene's authored
     * tod_hour the first frame ToD is enabled, then advanced by tod_speed.
     * tod_clock_valid gates the seed so editing tod_hour while frozen still
     * takes effect (we re-seed when the clock is inactive). */
    float             tod_clock_hour;
    bool              tod_clock_valid;
    bool              tod_driven;     /* ToD currently driven from settings */
};

/* ── Entity collection ────────────────────────────────────────────── */

typedef struct {
    JceEntity entities[SR_MAX_ENTITIES];
    int       count;
} EntityList;

static void collect_entity_cb(JceScene *s, JceEntity e, void *ud)
{
    (void)s;
    EntityList *list = (EntityList *)ud;
    if (list->count < SR_MAX_ENTITIES)
        list->entities[list->count++] = e;
}

/* ── Forward declarations for Phase 3 helpers ─────────────────────── */
static void     sr_reset_material_cache(JceSceneRenderer *sr);
static uint32_t sr_compute_material_key(const JcePbrMaterial *pbr,
                                        bool is_terrain,
                                        int terrain_slot,
                                        const bgfx_texture_handle_t *terrain_layer_tex);
static uint32_t sr_register_material(JceSceneRenderer *sr,
                                     uint32_t key,
                                     const JcePbrMaterial *pbr,
                                     bool is_terrain,
                                     int terrain_slot,
                                     float terrain_tile_scale,
                                     bool terrain_splat_enabled,
                                     const bgfx_texture_handle_t *terrain_layer_tex);
static void     sr_bind_material_cb(uint32_t material_key, void *user);
static void     sr_inline_bind_pbr_global(JceSceneRenderer *sr,
                                          const JcePbrMaterial *pbr,
                                          uint16_t view_id,
                                          JceScene *scene, EntityList *list);
static void     sr_extract_frustum_planes(const jce_mat4 *m, jce_vec4 planes[6]);

static void sr_rq_flush_queue_and_collect(JceSceneRenderer *sr,
                                          JceRenderQueue *q)
{
    if (!sr || !q) return;
    jce_rq_flush(q, sr->renderer);
    JceRenderQueueStats s;
    jce_rq_last_stats(q, &s);
    sr->stat_rq.commands_in     += s.commands_in;
    sr->stat_rq.submits_out     += s.submits_out;
    sr->stat_rq.batches_merged  += s.batches_merged;
    sr->stat_rq.instances_total += s.instances_total;
    sr->stat_rq_active           = true;
}

static void sr_rq_flush_and_collect(JceSceneRenderer *sr)
{
    if (!sr) return;
    sr_rq_flush_queue_and_collect(sr, sr->render_queue);
}

static bool entity_enabled(JceScene *scene, JceEntity e)
{
    if (jce_scene_has_editor_meta(scene, e)) {
        JceEditorMeta *m = jce_scene_get_editor_meta(scene, e);
        if (m && !m->enabled) return false;
    }
    return true;
}

/* ── Texture cache (resolved paths) ───────────────────────────────── */

static JceTexture sr_resolve_texture2(JceSceneRenderer *sr,
                                       const char *material_path,
                                       const char *mesh_path);

static JceTexture sr_resolve_texture(JceSceneRenderer *sr, const char *path)
{
    return sr_resolve_texture2(sr, path, NULL);
}

static JceTexture sr_resolve_texture2(JceSceneRenderer *sr,
                                       const char *material_path,
                                       const char *mesh_path)
{
    JceTexture invalid = { UINT16_MAX };
    bool have_mat  = material_path && material_path[0] != '\0';
    bool have_mesh = mesh_path     && mesh_path[0]     != '\0';
    if (!sr || (!have_mat && !have_mesh)) return invalid;

    bool have_cb = sr->has_cbs && sr->cbs.load_texture;

    /* Editor mode: the callback (asset cache) already maintains its own
     * deduplicated cache AND can recreate texture handles on async reloads.
     * Caching the handle locally would pin a stale (destroyed) bgfx handle
     * and cause the texture to render BLACK after any invalidation. So we
     * always re-query through the callback in editor mode. */
    if (have_cb)
        return sr->cbs.load_texture(have_mat ? material_path : NULL,
                                    have_mesh ? mesh_path : NULL,
                                    sr->cbs.userdata);

    /* Runtime mode: PAK-only loader is stable; cache for performance.
     * Runtime never uses mesh_path fallback (PAK has no MTL parser). */
    const char *path = have_mat ? material_path : mesh_path;
    for (int i = 0; i < sr->tex_cache_count; i++) {
        if (sr->tex_cache[i].used &&
            strncmp(sr->tex_cache[i].path, path,
                    sizeof(sr->tex_cache[i].path)) == 0)
        {
            return sr->tex_cache[i].failed ? invalid : sr->tex_cache[i].tex;
        }
    }

    JceTexture tex = invalid;
    if (sr->pak) tex = jce_texture_load(sr->pak, path);
    bool ok = jce_texture_valid(tex);

    if (sr->tex_cache_count < SR_TEX_CACHE_MAX) {
        int idx = sr->tex_cache_count++;
        snprintf(sr->tex_cache[idx].path, sizeof(sr->tex_cache[idx].path),
                 "%s", path);
        sr->tex_cache[idx].tex    = tex;
        sr->tex_cache[idx].used   = true;
        sr->tex_cache[idx].failed = !ok;
    }
    return tex;
}

/* ── Shader Graph custom-program cache ─────────────────────────────── */

static bool sr_ends_with_ci(const char *s, const char *suffix);

/* Resolve (and cache) the custom shader program declared by a material
 * file's Shader Graph reference.  Returns UINT16_MAX when the material has
 * no custom shader (the common case) or cannot be resolved.  Cached by
 * path so the .mat.json is parsed at most once per unique material. */
static JceShaderHandle sr_resolve_custom_program(JceSceneRenderer *sr,
                                                 const char *material_path)
{
    JceShaderHandle none = { UINT16_MAX };
    if (!sr || !material_path || !material_path[0]) return none;
    if (!sr_ends_with_ci(material_path, ".mat.json")) return none;

    for (int i = 0; i < sr->prog_cache_count; i++) {
        if (sr->prog_cache[i].used &&
            strncmp(sr->prog_cache[i].path, material_path,
                    sizeof(sr->prog_cache[i].path)) == 0)
            return sr->prog_cache[i].program;
    }

    /* Parse the material file; the loader links any persisted graph shader
     * into custom_program for us.  Only the program handle is kept here. */
    JceShaderHandle prog = none;
    if (jce_fs_host_exists_file(material_path)) {
        JcePbrMaterial m;
        char tex_paths[5][256];
        if (jce_pbr_material_load_json(material_path, &m, tex_paths) &&
            m.custom_program != UINT16_MAX)
            prog.idx = m.custom_program;
    }

    if (sr->prog_cache_count < SR_MAT_PROG_CACHE_MAX) {
        int idx = sr->prog_cache_count++;
        snprintf(sr->prog_cache[idx].path, sizeof(sr->prog_cache[idx].path),
                 "%s", material_path);
        sr->prog_cache[idx].program  = prog;
        sr->prog_cache[idx].used     = true;
        sr->prog_cache[idx].resolved = true;
    }
    return prog;
}

/* ── Model cache ──────────────────────────────────────────────────── */

static char sr_ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool sr_ends_with_ci(const char *s, const char *suffix)
{
    if (!s || !suffix) return false;
    size_t slen = strlen(s);
    size_t tlen = strlen(suffix);
    if (tlen > slen) return false;
    s += slen - tlen;
    for (size_t i = 0; i < tlen; i++) {
        if (sr_ascii_lower(s[i]) != sr_ascii_lower(suffix[i]))
            return false;
    }
    return true;
}

static bool sr_is_gltf_model_path(const char *path)
{
    return sr_ends_with_ci(path, ".gltf") || sr_ends_with_ci(path, ".glb");
}

static SrModelCache *sr_get_model(JceSceneRenderer *sr, const char *path,
                                  uint32_t entity_id)
{
    (void)entity_id;   /* model is shared by path; instance state is per-entity */
    if (!path || path[0] == '\0') return NULL;

    int free_slot = -1;
    for (int i = 0; i < SR_MODEL_CACHE_MAX; i++) {
        SrModelCache *e = &sr->model_cache[i];
        if (e->used && strcmp(e->path, path) == 0)
            return e;
        if (!e->used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return NULL;

    JceModel *model = NULL;
    if (sr->has_cbs && sr->cbs.load_model) {
        model = sr->cbs.load_model(path, sr->cbs.userdata);
    } else {
        model = jce_model_load_gltf(sr->pak, path);
    }
    if (!model) {
        LOG_WARN(LOG_TAG, "model cache: cannot load %s", path);
        return NULL;
    }

    SrModelCache *e = &sr->model_cache[free_slot];
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->model = model;
    e->used  = true;
    return e;
}

/* Find an existing per-entity animation instance (no creation). */
static SrAnimInstance *sr_find_anim_instance(JceSceneRenderer *sr, uint32_t entity)
{
    for (int i = 0; i < SR_ANIM_INSTANCE_MAX; i++) {
        SrAnimInstance *a = &sr->anim_inst[i];
        if (a->used && a->entity == entity) return a;
    }
    return NULL;
}

/* Get-or-create the per-entity animation instance for `entity` bound to the
   shared `model`. Creates a fresh player (from the model's read-only skeleton)
   and rebuilds it if the entity's model changed (skeleton_path reassigned). */
/* Release a frame-event pool and reset the dispatch state so the sidecar is
 * re-loaded lazily on the next frame (used on slot reclaim / model swap). */
static void sr_anim_events_reset(SrAnimInstance *a)
{
    if (!a) return;
    if (a->ev_pool) { JCE_FREE(a->ev_pool); a->ev_pool = NULL; }
    a->ev_pool_count  = 0;
    a->ev_track_count = 0;
    a->ev_loaded      = false;
    a->ev_clip        = -1;
    a->ev_prev_time   = 0.0f;
    memset(a->ev_tracks, 0, sizeof(a->ev_tracks));
}

static SrAnimInstance *sr_get_anim_instance(JceSceneRenderer *sr,
                                            uint32_t entity, JceModel *model)
{
    int free_slot = -1;
    for (int i = 0; i < SR_ANIM_INSTANCE_MAX; i++) {
        SrAnimInstance *a = &sr->anim_inst[i];
        if (a->used && a->entity == entity) {
            if (a->model != model) {
                if (a->player) jce_anim_player_destroy(a->player);
                a->player      = NULL;
                a->model       = model;
                a->active_clip = -1;
                /* Clip set changed — force the blend tree to rebuild against
                   the new model's clips. */
                if (a->blend_tree) {
                    jce_anim_blend_tree_destroy(a->blend_tree);
                    a->blend_tree = NULL;
                }
                a->bt_count = 0;
                /* New model = new clip set: drop the event pool so the new
                   skeleton's sidecar is re-loaded against the new clips. */
                sr_anim_events_reset(a);
                JceSkeleton *sk = jce_model_get_skeleton(model);
                if (sk && jce_model_anim_count(model) > 0)
                    a->player = jce_anim_player_create(sk);
            }
            return a;
        }
        if (!a->used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        LOG_WARN(LOG_TAG, "anim instance cache full (%d) — some skinned "
                 "entities will not animate", SR_ANIM_INSTANCE_MAX);
        return NULL;
    }
    SrAnimInstance *a = &sr->anim_inst[free_slot];
    if (a->sm_binding) jce_anim_sm_binding_destroy(a->sm_binding); /* reclaimed slot */
    if (a->blend_tree) jce_anim_blend_tree_destroy(a->blend_tree);
    if (a->ev_pool)    JCE_FREE(a->ev_pool);                       /* reclaimed slot */
    memset(a, 0, sizeof(*a));
    a->entity      = entity;
    a->model       = model;
    a->active_clip = -1;
    a->ev_clip     = -1;
    a->speed       = 1.0f;
    a->paused      = true;
    a->used        = true;
    JceSkeleton *sk = jce_model_get_skeleton(model);
    if (sk && jce_model_anim_count(model) > 0)
        a->player = jce_anim_player_create(sk);
    return a;
}

/* ── 2D sprite animator (P1 #16) ───────────────────────────────────── */

/* Find the per-entity sprite-animator slot (no creation). */
static int sr_find_sprite_anim(JceSceneRenderer *sr, uint32_t entity)
{
    for (int i = 0; i < SR_SPRITE_ANIM_MAX; i++)
        if (sr->sprite_anim[i].used && sr->sprite_anim[i].entity == entity)
            return i;
    return -1;
}

/* Build (or rebuild) the sprite sheet + player for `sa` on this slot. The
 * sheet comes from the JSON atlas when atlas_path is set, otherwise a uniform
 * grid sized from the texture (sheet_path is the image) and frame_w/frame_h. */
static void sr_sprite_anim_build(JceSceneRenderer *sr, int slot,
                                 const JceSpriteAnimatorComponent *sa)
{
    SrSpriteAnim *s = &sr->sprite_anim[slot];

    if (s->player) { jce_sprite_player_destroy(s->player); s->player = NULL; }
    if (s->sheet)  { jce_sprite_sheet_destroy(s->sheet);   s->sheet  = NULL; }

    if (sa->atlas_path[0]) {
        s->sheet = jce_sprite_sheet_load_json(
            sa->atlas_path, sa->sheet_path[0] ? sa->sheet_path : NULL);
    } else if (sa->sheet_path[0] && sa->frame_width > 0 && sa->frame_height > 0) {
        JceTexture tex = sr_resolve_texture(sr, sa->sheet_path);
        uint32_t iw = 0, ih = 0;
        if (jce_texture_valid(tex)) jce_texture_get_size(tex, &iw, &ih);
        if (iw > 0 && ih > 0) {
            s->sheet = jce_sprite_sheet_create_grid(
                sa->sheet_path, iw, ih,
                (uint32_t)sa->frame_width, (uint32_t)sa->frame_height, 100.0f);
        }
    }
    if (s->sheet) {
        s->player = jce_sprite_player_create(s->sheet);
        if (s->player && sa->current_anim[0])
            jce_sprite_player_set_anim(s->player, sa->current_anim);
    }

    snprintf(s->sheet_path, sizeof(s->sheet_path), "%s", sa->sheet_path);
    snprintf(s->atlas_path, sizeof(s->atlas_path), "%s", sa->atlas_path);
    snprintf(s->cur_anim,   sizeof(s->cur_anim),   "%s", sa->current_anim);
    s->frame_w = sa->frame_width;
    s->frame_h = sa->frame_height;
}

/* Advance every SpriteAnimator entity's frame time once per frame (mirrors the
 * skeletal "tick once" rule). The cached player is consumed in the entity draw
 * loop where the current frame's UV sub-rect is submitted to the sprite batch. */
static void sr_update_sprite_anims(JceSceneRenderer *sr, JceScene *scene,
                                   EntityList *list, float dt_sec)
{
    for (int i = 0; i < list->count; i++) {
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_sprite_animator(scene, e)) continue;
        if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SPRITE_ANIMATOR)) continue;

        JceSpriteAnimatorComponent *sa = jce_scene_get_sprite_animator(scene, e);
        if (!sa) continue;

        int slot = sr_find_sprite_anim(sr, (uint32_t)e);
        if (slot < 0) {
            for (int k = 0; k < SR_SPRITE_ANIM_MAX; k++) {
                if (!sr->sprite_anim[k].used) { slot = k; break; }
            }
            if (slot < 0) continue;   /* cache full */
            sr->sprite_anim[slot].used   = true;
            sr->sprite_anim[slot].entity = (uint32_t)e;
            sr->sprite_anim[slot].sheet  = NULL;
            sr->sprite_anim[slot].player = NULL;
            sr->sprite_anim[slot].sheet_path[0] = '\0';
        }

        /* Rebuild on authoring change (sheet/atlas/frame size). */
        if (strcmp(sr->sprite_anim[slot].sheet_path, sa->sheet_path) != 0 ||
            strcmp(sr->sprite_anim[slot].atlas_path, sa->atlas_path) != 0 ||
            sr->sprite_anim[slot].frame_w != sa->frame_width ||
            sr->sprite_anim[slot].frame_h != sa->frame_height) {
            sr_sprite_anim_build(sr, slot, sa);
        }

        JceSpritePlayer *pl = sr->sprite_anim[slot].player;
        if (!pl) continue;

        /* Animation switch at runtime. */
        if (strcmp(sr->sprite_anim[slot].cur_anim, sa->current_anim) != 0) {
            if (sa->current_anim[0])
                jce_sprite_player_set_anim(pl, sa->current_anim);
            snprintf(sr->sprite_anim[slot].cur_anim,
                     sizeof(sr->sprite_anim[slot].cur_anim), "%s",
                     sa->current_anim);
        }

        if (sa->playing) {
            float sp = sa->speed > 0.0f ? sa->speed : 1.0f;
            jce_sprite_player_update(pl, dt_sec, sp);
        }
    }
}

/* ── Frame events (P1 #16) ─────────────────────────────────────────── */

/* Dispatch sink for animation frame events. This is the documented hook
 * point: by default it logs the fired event; a game would route id-based
 * events to script / audio (e.g. footstep -> play sfx, hitbox-on -> enable
 * collider). Kept in-engine and side-effect-free beyond logging so the wiring
 * is safe even before any project authors events. */
static void sr_anim_event_dispatch(const JceAnimEvent *ev, void *user)
{
    uint32_t entity = (uint32_t)(uintptr_t)user;
    if (!ev) return;
    LOG_DEBUG(LOG_TAG, "anim event: entity=%u id=%u t=%.3f f0=%.3f f1=%.3f i0=%d",
              entity, ev->id, ev->time, ev->f0, ev->f1, ev->i0);
}

static int sr_anim_event_cmp(const void *a, const void *b)
{
    float ta = ((const JceAnimEvent *)a)->time;
    float tb = ((const JceAnimEvent *)b)->time;
    return (ta < tb) ? -1 : (ta > tb) ? 1 : 0;
}

/* Lazily load <skeleton_path>.anim.json once per instance. The sidecar maps
 * clip names to event arrays:
 *   { "Run": [ { "time": 0.25, "id": 1, "f0": 0, "f1": 0, "i0": 0 }, ... ] }
 * Each clip's events are stored contiguously in ev_pool and exposed as a
 * per-clip JceAnimEventTrack indexed by the model's clip index. Absent or
 * malformed sidecars leave zero tracks (events simply never fire). */
static void sr_anim_events_load(SrAnimInstance *ai, const char *skeleton_path,
                                JceModel *model)
{
    if (!ai || ai->ev_loaded) return;
    ai->ev_loaded = true;          /* one-shot: never re-attempt */
    if (!skeleton_path || !skeleton_path[0] || !model) return;

    char sidecar[300];
    snprintf(sidecar, sizeof(sidecar), "%s.anim.json", skeleton_path);
    JceJson *root = jce_json_parse_file(sidecar);
    if (!root) return;             /* no sidecar — common case, not an error */

    int clip_count = (int)jce_model_anim_count(model);
    if (clip_count > 16) clip_count = 16;

    /* Pass 1: count total events across the clips we know about. */
    int total = 0;
    for (int c = 0; c < clip_count; c++) {
        const char *cn = jce_anim_clip_name(jce_model_get_anim(model, (uint32_t)c));
        if (!cn) continue;
        JceJson *arr = jce_json_get(root, cn);
        if (jce_json_is_array(arr)) total += jce_json_array_size(arr);
    }
    if (total <= 0) { jce_json_free(root); return; }

    ai->ev_pool = (JceAnimEvent *)JCE_CALLOC((size_t)total, sizeof(JceAnimEvent));
    if (!ai->ev_pool) { jce_json_free(root); return; }

    /* Pass 2: fill per-clip tracks (contiguous slices of ev_pool). */
    int write = 0;
    for (int c = 0; c < clip_count; c++) {
        const JceAnimClip *clip = jce_model_get_anim(model, (uint32_t)c);
        const char *cn = jce_anim_clip_name(clip);
        JceAnimEventTrack *trk = &ai->ev_tracks[c];
        trk->events        = NULL;
        trk->count         = 0;
        trk->clip_duration = clip ? jce_anim_clip_duration(clip) : 0.0f;
        if (!cn) continue;
        JceJson *arr = jce_json_get(root, cn);
        if (!jce_json_is_array(arr)) continue;

        JceAnimEvent *first = &ai->ev_pool[write];
        int n = 0;
        for (JceJson *it = jce_json_first_child(arr); it && write < total;
             it = jce_json_next_sibling(it)) {
            JceAnimEvent *ev = &ai->ev_pool[write++];
            ev->time = (float)jce_json_get_number(it, "time", 0.0);
            ev->id   = (uint32_t)jce_json_get_int(it, "id", 0);
            ev->f0   = (float)jce_json_get_number(it, "f0", 0.0);
            ev->f1   = (float)jce_json_get_number(it, "f1", 0.0);
            ev->i0   = jce_json_get_int(it, "i0", 0);
            n++;
        }
        if (n > 0) {
            qsort(first, (size_t)n, sizeof(JceAnimEvent), sr_anim_event_cmp);
            trk->events = first;
            trk->count  = n;
        }
    }
    ai->ev_pool_count  = write;
    ai->ev_track_count = clip_count;
    jce_json_free(root);
    LOG_INFO(LOG_TAG, "anim events: loaded %d events for %s", write, sidecar);
}

/* Advance frame events for the clip the player is currently on, firing every
 * event in the (prev,cur] clip-time window (with loop wrap handled by
 * jce_anim_events_advance). Called after the pose has been advanced so the
 * player's time reflects this frame. */
static void sr_anim_events_advance(SrAnimInstance *ai, int clip_index)
{
    if (!ai || !ai->player) return;
    float cur = jce_anim_player_get_time(ai->player);

    /* Clip switch (or first sample): reset baseline, fire nothing this frame. */
    if (clip_index != ai->ev_clip) {
        ai->ev_clip      = clip_index;
        ai->ev_prev_time = cur;
        return;
    }
    if (clip_index < 0 || clip_index >= ai->ev_track_count) {
        ai->ev_prev_time = cur;
        return;
    }
    const JceAnimEventTrack *trk = &ai->ev_tracks[clip_index];
    if (trk->count > 0) {
        jce_anim_events_advance(trk, ai->ev_prev_time, cur,
                                sr_anim_event_dispatch,
                                (void *)(uintptr_t)ai->entity);
    }
    ai->ev_prev_time = cur;
}

/* Case-insensitive, basename-aware clip-name match (mirrors the SM binding):
 * robust to inconsistent casing / paths across different models. */
static int sr_lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static bool sr_clip_name_match(const char *a, const char *b)
{
    if (!a || !b) return false;
    const char *ba = a, *bb = b;
    for (const char *p = a; *p; ++p) if (*p == '/' || *p == '\\') ba = p + 1;
    for (const char *p = b; *p; ++p) if (*p == '/' || *p == '\\') bb = p + 1;
    for (; *ba && *bb; ++ba, ++bb)
        if (sr_lc((unsigned char)*ba) != sr_lc((unsigned char)*bb)) return false;
    return *ba == '\0' && *bb == '\0';
}

/* Compute the entity's planar (XZ) movement speed from the frame-to-frame
 * Transform delta (works for physics, kinematic, and pure-animated entities),
 * EMA-smoothed so render-rate jitter doesn't make a downstream SM/blend flip.
 * The CALLER feeds it into the SM "Speed" param and/or the blend tree's
 * blend_param — this is the generic locomotion driver. */
static float sr_compute_entity_speed(SrAnimInstance *ai, const JceTransform *tc,
                                     float dt_sec)
{
    float raw = 0.0f;
    if (tc && ai->sm_have_prev && dt_sec > 0.0001f) {
        jce_vec3 d = jce_v3_sub(tc->position, ai->sm_prev_pos);
        d.y = 0.0f;                       /* planar locomotion speed */
        raw = jce_v3_len(d) / dt_sec;
    }
    if (tc) { ai->sm_prev_pos = tc->position; ai->sm_have_prev = true; }
    ai->sm_speed += (raw - ai->sm_speed) * 0.18f;   /* EMA smooth */
    if (ai->sm_speed < 0.0f) ai->sm_speed = 0.0f;
    return ai->sm_speed;
}

/* Evaluate every skeletal-animator entity's pose ONCE per frame, BEFORE any
 * render pass.  The resulting world-bone palette is cached on the model
 * entry so the shadow pass (which the CPU records before the color pass) and
 * the color pass both consume the identical pose — animation time is advanced
 * exactly once, never per-pass.  This is the "skin once, draw many" rule and
 * is the only ordering consistent with shadow producers preceding the color
 * consumer view. */
static void sr_update_skinned_anims(JceSceneRenderer *sr, JceScene *scene,
                                    EntityList *list, float dt_sec)
{
    for (int i = 0; i < list->count; i++) {
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_skeletal_animator(scene, e)) continue;
        if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SKELETAL_ANIMATOR)) continue;

        JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
        if (!sa || !sa->skeleton_path[0]) continue;

        SrModelCache *mc = sr_get_model(sr, sa->skeleton_path, (uint32_t)e);
        if (!mc || !mc->model) continue;

        SrAnimInstance *ai = sr_get_anim_instance(sr, (uint32_t)e, mc->model);
        if (!ai) continue;
        ai->skin_palette_count = 0;
        if (!ai->player) continue;

        /* Frame events: one-shot lazy load of the <skeleton>.anim.json sidecar. */
        sr_anim_events_load(ai, sa->skeleton_path, mc->model);

        int ac = sa->active_clip;
        /* OPT-IN auto-locomotion: only when the component sets auto_speed do we
         * compute movement speed and feed it into the SM "Speed" param /
         * blend_param. Default off → the engine stays generic (params and
         * blend_param are driven by game code or authored values; the renderer
         * only EVALUATES the SM/blend tree). Also gated to Play. */
        float move_speed = 0.0f;
        if (sr->anim_sm_active && sa->auto_speed)
            move_speed = sr_compute_entity_speed(
                ai, jce_scene_get_transform(scene, e), dt_sec);

        /* State-machine override: when an .anim_sm.json is bound, tick it and
           let it pick the active clip by name (parameter-driven transitions).
           The binding is per-instance runtime state, (re)created when the path
           changes; falls through to single-clip playback when unbound. */
        if (sa->sm_path[0]) {
            if (!ai->sm_binding || strcmp(ai->sm_path, sa->sm_path) != 0) {
                if (ai->sm_binding) jce_anim_sm_binding_destroy(ai->sm_binding);
                ai->sm_binding = jce_anim_sm_binding_create(sa->sm_path);
                snprintf(ai->sm_path, sizeof(ai->sm_path), "%s", sa->sm_path);
            }
            if (ai->sm_binding && sr->anim_sm_active) {
                /* Only DRIVE the SM in Play; in the editor the binding stays
                 * idle so manual clip selection previews normally. The engine
                 * auto-feeds "Speed" ONLY when auto_speed is set — otherwise the
                 * SM's params are whatever game code set (engine stays generic);
                 * tick still runs so those game-driven params take effect. */
                if (sa->auto_speed)
                    jce_anim_sm_binding_set_float(ai->sm_binding, "Speed", move_speed);
                jce_anim_sm_binding_tick(ai->sm_binding, dt_sec);
                int an = (int)jce_model_anim_count(mc->model);
                if (an > 0) {
                    const char *names[64];
                    if (an > 64) an = 64;
                    for (int ci = 0; ci < an; ci++)
                        names[ci] = jce_anim_clip_name(
                            jce_model_get_anim(mc->model, (uint32_t)ci));
                    int sm_clip = jce_anim_sm_binding_resolve_clip_index(
                        ai->sm_binding, names, an);
                    if (sm_clip >= 0) ac = sm_clip;
                }
            }
        } else if (ai->sm_binding) {
            /* sm_path cleared at runtime — drop the stale binding. */
            jce_anim_sm_binding_destroy(ai->sm_binding);
            ai->sm_binding = NULL;
            ai->sm_path[0] = '\0';
        }
        float sp = sa->speed > 0.0f ? sa->speed : 1.0f;

        /* Blend-tree path: cross-blend the two clips bracketing blend_param.
           Takes precedence over the SM / single-clip path. The 1D tree is
           cached on the instance and rebuilt only when the clip set or the
           thresholds change. */
        if (sa->use_blend_tree) {
            /* Opt-in locomotion: auto-drive the blend by movement speed only
             * when auto_speed is set (and in Play). Otherwise blend_param is
             * whatever game code / the author set it to — so the blend tree is
             * generic (can be driven by direction, lean, etc., not just speed). */
            if (sr->anim_sm_active && sa->auto_speed) sa->blend_param = move_speed;
            int cc = sa->clip_count;
            if (cc < 0) cc = 0;
            if (cc > 8) cc = 8;
            bool rebuild = (!ai->blend_tree) || (ai->bt_count != cc);
            for (int t = 0; t < cc && !rebuild; t++)
                if (ai->bt_thresh[t] != sa->blend_thresholds[t]) rebuild = true;
            if (rebuild && cc > 0) {
                if (ai->blend_tree) jce_anim_blend_tree_destroy(ai->blend_tree);
                ai->blend_tree = jce_anim_blend_tree_create_1d((uint32_t)cc);
                ai->bt_count = cc;
                int an = (int)jce_model_anim_count(mc->model);
                for (int t = 0; t < cc; t++) {
                    ai->bt_thresh[t] = sa->blend_thresholds[t];
                    jce_anim_blend_tree_add(ai->blend_tree, sa->clip_names[t],
                                            sa->blend_thresholds[t]);
                    bool matched = false;
                    for (int k = 0; k < an; k++) {
                        const char *cn = jce_anim_clip_name(
                            jce_model_get_anim(mc->model, (uint32_t)k));
                        if (sr_clip_name_match(cn, sa->clip_names[t])) {
                            jce_anim_blend_tree_set_clip(ai->blend_tree,
                                sa->clip_names[t],
                                jce_model_get_anim(mc->model, (uint32_t)k));
                            matched = true;
                            break;
                        }
                    }
                    if (!matched && sa->clip_names[t][0])
                        LOG_WARN(LOG_TAG, "blend tree: clip '%s' not found in "
                                 "model '%s' (%d clips) — check name/casing",
                                 sa->clip_names[t], sa->skeleton_path, an);
                }
            }
            if (ai->blend_tree && ai->bt_count > 0) {
                JceAnimBlendTreeEval bt;
                jce_anim_blend_tree_evaluate(ai->blend_tree, sa->blend_param, &bt);
                ai->bt_time += dt_sec * sp;          /* shared phase */
                float da = bt.clip_a ? jce_anim_clip_duration(bt.clip_a) : 0.0f;
                float db = bt.clip_b ? jce_anim_clip_duration(bt.clip_b) : 0.0f;
                float ta = da > 0.0001f ? fmodf(ai->bt_time, da) : 0.0f;
                float tb = db > 0.0001f ? fmodf(ai->bt_time, db) : 0.0f;
                ai->skin_palette_count = jce_anim_player_blend(ai->player,
                    bt.clip_a, ta, bt.weight_a,
                    bt.clip_b, tb, bt.weight_b,
                    ai->skin_palette, JCE_MAX_BONES);
                ai->active_clip = -1;   /* tree owns the pose this frame */
                ai->loop   = sa->loop;
                ai->speed  = sp;
                ai->paused = false;
                continue;               /* skip the single-clip path */
            }
        }

        JceAnimClip *clip = NULL;
        if (ac >= 0 && ac < (int)jce_model_anim_count(mc->model))
            clip = jce_model_get_anim(mc->model, (uint32_t)ac);

        bool comp_playing = sa->playing;
        bool clip_changed = (ai->active_clip != ac);
        bool loop_changed = (ai->loop != sa->loop);
        bool speed_changed = fabsf(ai->speed - sp) > 0.0001f;
        bool paused_changed = (ai->paused == comp_playing);

        if (comp_playing && clip) {
            if (!jce_anim_player_is_playing(ai->player)
                || clip_changed || loop_changed) {
                jce_anim_player_play(ai->player, clip, sa->loop, sp);
            } else if (speed_changed || paused_changed) {
                jce_anim_player_set_speed(ai->player, sp);
            }
            jce_anim_player_pause(ai->player, false);
            jce_anim_player_set_speed(ai->player, sp);
        } else {
            if (clip && (clip_changed || loop_changed)) {
                jce_anim_player_play(ai->player, clip, sa->loop, sp);
                jce_anim_player_set_time(ai->player, 0.0f);
            }
            if (jce_anim_player_is_playing(ai->player))
                jce_anim_player_pause(ai->player, true);
        }

        ai->active_clip = ac;
        ai->loop = sa->loop;
        ai->speed = sp;
        ai->paused = !comp_playing;

        ai->skin_palette_count = jce_anim_player_update(ai->player, dt_sec,
                                                        ai->skin_palette,
                                                        JCE_MAX_BONES);

        /* Frame events: fire over (prev,cur] now that the player time has
           advanced. SM-driven clip switches reset the baseline cleanly. */
        sr_anim_events_advance(ai, ac);

        /* Two-bone IK (P1 #16): the analytic solver
           (jce_anim_ik_two_bone_solve) is ready to run here, after the pose
           is evaluated and before the palette is consumed by the shadow/color
           passes. It is intentionally NOT invoked yet: there is no IK
           rig-target authoring data (no JceIkConstraint component carrying the
           root/mid/end joint indices, target entity and pole). Once that
           component lands, resolve the three joint world positions from the
           palette, call the solver per chain (gated by LOD/cull), and write
           the corrected mid/end back into the palette. */
    }
}

/* If entity e is a skeletal-animator model, draw its skinned silhouette into
 * the given shadow view using the palette cached by sr_update_skinned_anims,
 * and return true so the caller skips the static-mesh shadow path (mirrors
 * the color pass, which routes such entities through jce_model_draw and skips
 * their MeshRenderer).  Returns false for non-skinned entities. */
static bool sr_try_submit_skinned_shadow(JceSceneRenderer *sr, JceScene *scene,
                                         JceEntity e, uint16_t view_id)
{
    if (!jce_scene_has_skeletal_animator(scene, e)) return false;
    JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
    if (!sa || !sa->skeleton_path[0]) return false;

    SrModelCache *mc = sr_get_model(sr, sa->skeleton_path, (uint32_t)e);
    if (!mc || !mc->model) return false;

    JceTransform *t = jce_scene_get_transform(scene, e);
    if (!t) return false;
    float sx = (t->scale.x != 0.0f) ? t->scale.x : 1.0f;
    float sy = (t->scale.y != 0.0f) ? t->scale.y : 1.0f;
    float sz = (t->scale.z != 0.0f) ? t->scale.z : 1.0f;
    jce_mat4 model = jce_m4_from_trs(t->position, t->rotation, jce_v3(sx, sy, sz));

    SrAnimInstance *ai = sr_find_anim_instance(sr, (uint32_t)e);
    const jce_mat4 *pal = (ai && ai->skin_palette_count > 0) ? ai->skin_palette : NULL;
    uint32_t pal_n = ai ? ai->skin_palette_count : 0;
    jce_model_draw_shadow(mc->model, sr->renderer, view_id, &model, pal, pal_n);
    return true;
}

static const char *sr_mesh_renderer_model_path(JceScene *scene, JceEntity e)
{
    if (!jce_scene_has_mesh_renderer(scene, e)) return NULL;
    if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_MESH_RENDERER)) return NULL;
    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
    if (!mr || !mr->visible || !mr->mesh_path[0]) return NULL;
    return sr_is_gltf_model_path(mr->mesh_path) ? mr->mesh_path : NULL;
}

static bool sr_try_submit_mesh_renderer_model_shadow(JceSceneRenderer *sr,
                                                     JceScene *scene,
                                                     JceEntity e,
                                                     uint16_t view_id)
{
    const char *path = sr_mesh_renderer_model_path(scene, e);
    if (!path || !jce_scene_has_transform(scene, e)) return false;

    SrModelCache *mc = sr_get_model(sr, path, (uint32_t)e);
    if (!mc || !mc->model) return false;

    jce_mat4 model = jce_scene_get_world_matrix(scene, e);
    jce_model_draw_shadow(mc->model, sr->renderer, view_id, &model, NULL, 0);
    return true;
}

static bool sr_try_draw_mesh_renderer_model(JceSceneRenderer *sr,
                                            JceScene *scene,
                                            JceEntity e,
                                            uint16_t view_id,
                                            const jce_mat4 *model)
{
    const char *path = sr_mesh_renderer_model_path(scene, e);
    if (!path) return false;

    SrModelCache *mc = sr_get_model(sr, path, (uint32_t)e);
    if (!mc || !mc->model) return false;

    jce_model_draw(mc->model, sr->renderer, view_id, model, NULL, 0);
    return true;
}

/* ── Mesh resolution ──────────────────────────────────────────────── */

static JceMesh *sr_resolve_mesh(JceSceneRenderer *sr, const JceMeshRenderer *mr)
{
    if (!mr) return NULL;
    JceMesh *mesh = NULL;
    if (mr->mesh_path[0] != '\0') {
        if (sr_is_gltf_model_path(mr->mesh_path))
            return NULL;
        if (sr->has_cbs && sr->cbs.load_mesh)
            mesh = sr->cbs.load_mesh(mr->mesh_path, sr->cbs.userdata);
        else
            mesh = jce_mesh_load(sr->pak, mr->mesh_path);
    }
    if (mesh) return mesh;

    switch (mr->mesh_shape) {
    default:
    case 0: return sr->cube_mesh;
    case 1: return sr->sphere_mesh;
    case 2: return sr->plane_mesh;
    case 3: return sr->capsule_mesh;
    case 4: return sr->cylinder_mesh;
    }
}

/* ── Terrain per-chunk LOD cache (P1-terrain-lod) ─────────────────────
 *
 * sr_terrain_get_slot() find-or-loads a terrain by path and lazily fills the
 * per-chunk metadata (chunk count + each chunk's terrain-LOCAL AABB).  The
 * actual GPU chunk meshes are built on demand by sr_terrain_chunk_mesh() at
 * whatever LOD the camera-distance test asks for, and cached until the
 * required LOD changes (or the terrain is invalidated).  Nothing here merges
 * chunks: the renderer issues one draw per visible chunk. */

/* Compute chunk (cx,cz)'s terrain-local AABB by scanning its height samples.
 * Cheap (runs once per chunk at load) and gives a snug Y range for culling. */
static void sr_terrain_chunk_local_aabb(const JceTerrain *t, int cx, int cz,
                                        jce_vec3 *out_min, jce_vec3 *out_max)
{
    int w  = jce_terrain_width(t);
    int h  = jce_terrain_height(t);
    int cs = jce_terrain_chunk_size(t);
    float wsx = jce_terrain_world_size_x(t);
    float wsz = jce_terrain_world_size_z(t);
    float mh  = jce_terrain_max_height(t);
    const float *heights = jce_terrain_heights(t);

    int x0 = cx * cs, z0 = cz * cs;
    int x1 = x0 + cs, z1 = z0 + cs;
    if (x1 > w - 1) x1 = w - 1;
    if (z1 > h - 1) z1 = h - 1;

    float dx = (w > 1) ? wsx / (float)(w - 1) : 0.0f;
    float dz = (h > 1) ? wsz / (float)(h - 1) : 0.0f;

    float hmin = +FLT_MAX, hmax = -FLT_MAX;
    if (heights) {
        for (int zz = z0; zz <= z1; zz++)
        for (int xx = x0; xx <= x1; xx++) {
            float hv = heights[(size_t)zz * (size_t)w + (size_t)xx] * mh;
            if (hv < hmin) hmin = hv;
            if (hv > hmax) hmax = hv;
        }
    }
    if (hmin > hmax) { hmin = 0.0f; hmax = mh; }

    out_min->x = (float)x0 * dx; out_max->x = (float)x1 * dx;
    out_min->z = (float)z0 * dz; out_max->z = (float)z1 * dz;
    out_min->y = hmin;           out_max->y = hmax;
}

/* Free all per-chunk cache arrays + GPU meshes for one terrain slot. */
static void sr_terrain_free_chunks(JceSceneRenderer *sr, int slot)
{
    if (slot < 0 || slot >= 16) return;
    if (sr->terrain_cache[slot].chunk_meshes) {
        for (int i = 0; i < sr->terrain_cache[slot].chunk_count; i++)
            if (sr->terrain_cache[slot].chunk_meshes[i])
                jce_mesh_destroy(sr->terrain_cache[slot].chunk_meshes[i]);
        JCE_FREE(sr->terrain_cache[slot].chunk_meshes);
        sr->terrain_cache[slot].chunk_meshes = NULL;
    }
    if (sr->terrain_cache[slot].chunk_lod) {
        JCE_FREE(sr->terrain_cache[slot].chunk_lod);
        sr->terrain_cache[slot].chunk_lod = NULL;
    }
    if (sr->terrain_cache[slot].chunk_min) {
        JCE_FREE(sr->terrain_cache[slot].chunk_min);
        sr->terrain_cache[slot].chunk_min = NULL;
    }
    if (sr->terrain_cache[slot].chunk_max) {
        JCE_FREE(sr->terrain_cache[slot].chunk_max);
        sr->terrain_cache[slot].chunk_max = NULL;
    }
    sr->terrain_cache[slot].chunk_count = 0;
}

/* Allocate + fill the per-chunk metadata for a freshly loaded terrain. */
static bool sr_terrain_init_chunks(JceSceneRenderer *sr, int slot)
{
    JceTerrain *terr = sr->terrain_cache[slot].terrain;
    if (!terr) return false;
    int ncx = jce_terrain_chunk_count_x(terr);
    int ncz = jce_terrain_chunk_count_z(terr);
    int n   = ncx * ncz;
    if (n <= 0 || n > SR_TERRAIN_MAX_CHUNKS) return false;

    JceMesh  **meshes = (JceMesh **)JCE_MALLOC(sizeof(JceMesh *) * (size_t)n);
    int8_t    *lods   = (int8_t  *)JCE_MALLOC(sizeof(int8_t)    * (size_t)n);
    jce_vec3  *mins   = (jce_vec3 *)JCE_MALLOC(sizeof(jce_vec3) * (size_t)n);
    jce_vec3  *maxs   = (jce_vec3 *)JCE_MALLOC(sizeof(jce_vec3) * (size_t)n);
    if (!meshes || !lods || !mins || !maxs) {
        if (meshes) JCE_FREE(meshes);
        if (lods)   JCE_FREE(lods);
        if (mins)   JCE_FREE(mins);
        if (maxs)   JCE_FREE(maxs);
        return false;
    }
    for (int cz = 0; cz < ncz; cz++)
    for (int cx = 0; cx < ncx; cx++) {
        int idx = cz * ncx + cx;
        meshes[idx] = NULL;
        lods[idx]   = -1;
        sr_terrain_chunk_local_aabb(terr, cx, cz, &mins[idx], &maxs[idx]);
    }
    sr->terrain_cache[slot].chunk_meshes = meshes;
    sr->terrain_cache[slot].chunk_lod    = lods;
    sr->terrain_cache[slot].chunk_min    = mins;
    sr->terrain_cache[slot].chunk_max    = maxs;
    sr->terrain_cache[slot].chunk_count  = n;
    sr->terrain_cache[slot].chunk_nx     = ncx;
    sr->terrain_cache[slot].chunk_nz     = ncz;
    return true;
}

/* Build (or rebuild) chunk `idx`'s GPU mesh at `lod`, with a downward skirt
 * around its outer ring to hide T-junction cracks against neighbouring chunks
 * drawn at a different LOD.  Returns the cached mesh, or NULL on failure.
 * No-op (returns the cached mesh) when the chunk is already built at `lod`. */
static JceMesh *sr_terrain_chunk_mesh(JceSceneRenderer *sr, int slot,
                                      int cx, int cz, int lod)
{
    JceTerrain *terr = sr->terrain_cache[slot].terrain;
    if (!terr) return NULL;
    int ncx = sr->terrain_cache[slot].chunk_nx;
    int idx = cz * ncx + cx;
    if (idx < 0 || idx >= sr->terrain_cache[slot].chunk_count) return NULL;

    if (sr->terrain_cache[slot].chunk_meshes[idx] &&
        sr->terrain_cache[slot].chunk_lod[idx] == (int8_t)lod)
        return sr->terrain_cache[slot].chunk_meshes[idx];

    int core_v = 0, core_i = 0;
    jce_terrain_chunk_mesh_size(terr, cx, cz, lod, &core_v, &core_i);
    if (core_v <= 0 || core_i <= 0) return NULL;

    /* Skirt adds at most a full ring of duplicated edge verts plus two
     * triangles per edge segment.  nx == nz == sqrt(core_v) for square
     * chunks, but allocate the worst case: 4 edges of `core_v` verts. */
    int skirt_v_cap = 4 * core_v;
    int skirt_i_cap = 4 * core_v * 6;
    int cap_v = core_v + skirt_v_cap;
    int cap_i = core_i + skirt_i_cap;

    JceTerrainVertex *vb = (JceTerrainVertex *)
        JCE_MALLOC(sizeof(JceTerrainVertex) * (size_t)cap_v);
    uint32_t *ib = (uint32_t *)JCE_MALLOC(sizeof(uint32_t) * (size_t)cap_i);
    if (!vb || !ib) { if (vb) JCE_FREE(vb); if (ib) JCE_FREE(ib); return NULL; }

    int wrote_v = 0, wrote_i = 0;
    jce_terrain_chunk_build_mesh(terr, cx, cz, lod,
                                 vb, core_v, ib, core_i, &wrote_v, &wrote_i);
    if (wrote_v <= 0 || wrote_i <= 0) { JCE_FREE(vb); JCE_FREE(ib); return NULL; }

    /* The core grid is row-major nx*nz (see jce_terrain_chunk_build_mesh).
     * Derive nx/nz from the chunk extents at this LOD to walk its border. */
    int w  = jce_terrain_width(terr);
    int h  = jce_terrain_height(terr);
    int cs = jce_terrain_chunk_size(terr);
    int x0 = cx * cs, z0 = cz * cs;
    int x1 = x0 + cs, z1 = z0 + cs;
    if (x1 > w - 1) x1 = w - 1;
    if (z1 > h - 1) z1 = h - 1;
    int step = (lod <= 0) ? 1 : (1 << lod);
    int nx = (x1 - x0) / step + 1;
    int nz = (z1 - z0) / step + 1;

    float skirt = jce_terrain_max_height(terr) * SR_TERRAIN_SKIRT_FRAC;
    if (skirt < 0.01f) skirt = 0.01f;

    /* Append a skirt strip along one chunk border.  edge_ids[0..count-1] are
     * the core border vertex indices in order; for each we add a duplicated
     * vertex dropped by `skirt`, then stitch vertical quads between the core
     * ring and the apron.  `flip` selects the winding so all four edges face
     * outward consistently with the terrain's CW core winding. */
    {
        /* nx and nz are at most chunk_size+1; cap the border walk so an
         * unusually large chunk_size cannot overflow this scratch ring. */
        enum { SR_TC_EDGE_CAP = 4097 };
        int edge_ids[SR_TC_EDGE_CAP];
        for (int e4 = 0; e4 < 4; e4++) {
            int count, flip;
            switch (e4) {
            case 0: /* north (j=0)      */ count = nx; flip = 0; break;
            case 1: /* south (j=nz-1)   */ count = nx; flip = 1; break;
            case 2: /* west  (i=0)      */ count = nz; flip = 1; break;
            default:/* east  (i=nx-1)   */ count = nz; flip = 0; break;
            }
            if (count > SR_TC_EDGE_CAP) count = SR_TC_EDGE_CAP;
            for (int s = 0; s < count; s++) {
                switch (e4) {
                case 0:  edge_ids[s] = s;                       break;
                case 1:  edge_ids[s] = (nz - 1) * nx + s;       break;
                case 2:  edge_ids[s] = s * nx;                  break;
                default: edge_ids[s] = s * nx + (nx - 1);       break;
                }
            }
            int first_apron = wrote_v;
            for (int s = 0; s < count; s++) {
                int cvid = edge_ids[s];
                if (cvid < 0 || cvid >= core_v) continue;
                if (wrote_v >= cap_v) break;
                JceTerrainVertex a = vb[cvid];
                a.py -= skirt;
                vb[wrote_v++] = a;
            }
            for (int s = 0; s + 1 < count; s++) {
                if (wrote_i + 6 > cap_i) break;
                int top0 = edge_ids[s];
                int top1 = edge_ids[s + 1];
                int bot0 = first_apron + s;
                int bot1 = first_apron + s + 1;
                if (top0 < 0 || top1 < 0 ||
                    top0 >= core_v || top1 >= core_v ||
                    bot1 >= wrote_v) continue;
                if (flip) {
                    ib[wrote_i++] = (uint32_t)top0; ib[wrote_i++] = (uint32_t)bot0;
                    ib[wrote_i++] = (uint32_t)top1; ib[wrote_i++] = (uint32_t)top1;
                    ib[wrote_i++] = (uint32_t)bot0; ib[wrote_i++] = (uint32_t)bot1;
                } else {
                    ib[wrote_i++] = (uint32_t)top0; ib[wrote_i++] = (uint32_t)top1;
                    ib[wrote_i++] = (uint32_t)bot0; ib[wrote_i++] = (uint32_t)bot1;
                    ib[wrote_i++] = (uint32_t)bot0; ib[wrote_i++] = (uint32_t)top1;
                }
            }
        }
    }

    if (sr->terrain_cache[slot].chunk_meshes[idx]) {
        jce_mesh_destroy(sr->terrain_cache[slot].chunk_meshes[idx]);
        sr->terrain_cache[slot].chunk_meshes[idx] = NULL;
    }
    /* JceTerrainVertex layout matches JceMeshVertex exactly. */
    JceMesh *m = jce_mesh_create((const JceMeshVertex *)vb, (uint32_t)wrote_v,
                                 ib, (uint32_t)wrote_i);
    JCE_FREE(vb); JCE_FREE(ib);
    if (!m) return NULL;
    sr->terrain_cache[slot].chunk_meshes[idx] = m;
    sr->terrain_cache[slot].chunk_lod[idx]    = (int8_t)lod;
    return m;
}

/* Find (or lazily load) the terrain cache slot for `path`, initialising the
 * per-chunk metadata on first load.  Returns the slot index, or -1 on failure
 * (no free slot, load failed, or chunk init failed). */
static int sr_terrain_find_or_load_slot(JceSceneRenderer *sr, const char *path)
{
    if (!sr || !path || !path[0]) return -1;
    int slot = -1, free_slot = -1;
    for (int i = 0; i < 16; i++) {
        if (sr->terrain_cache[i].used &&
            strncmp(sr->terrain_cache[i].path, path,
                    sizeof sr->terrain_cache[i].path) == 0) {
            slot = i; break;
        }
        if (!sr->terrain_cache[i].used && free_slot < 0) free_slot = i;
    }
    if (slot >= 0)
        return sr->terrain_cache[slot].failed ? -1 : slot;
    if (free_slot < 0) return -1;

    slot = free_slot;
    memset(&sr->terrain_cache[slot], 0, sizeof sr->terrain_cache[slot]);
    jce_strlcpy(sr->terrain_cache[slot].path, path,
                sizeof sr->terrain_cache[slot].path);
    sr->terrain_cache[slot].used = true;
    sr->terrain_cache[slot].splat_tex.idx = UINT16_MAX;

    /* PAK-first: deployed bundles overlay sr->pak, so a bundled terrain
     * meta+bin loads with zero host filesystem access.  If the path isn't in
     * the PAK, fall back to the host-resolved path (editor / loose files). */
    JceTerrain *terr = jce_terrain_load_from_pak(sr->pak, path);
    char        resolved[1024];
    const char *load_path = path;
    if (!terr) {
        if (sr->has_cbs && sr->cbs.resolve_path &&
            sr->cbs.resolve_path(path, resolved, (int)sizeof(resolved),
                                 sr->cbs.userdata)) {
            load_path = resolved;
        }
        terr = jce_terrain_load_file(load_path);
    }
    if (!terr) {
        sr->terrain_cache[slot].failed = true;
        LOG_WARN(LOG_TAG, "terrain load failed: '%s' (from '%s')",
                 load_path, path);
        return -1;
    }
    sr->terrain_cache[slot].terrain = terr;
    if (!sr_terrain_init_chunks(sr, slot)) {
        sr->terrain_cache[slot].failed = true;
        return -1;
    }
    return slot;
}

static bool sr_build_entity_model(JceSceneRenderer *sr, JceScene *scene,
                                  JceEntity e, jce_mat4 *out_model,
                                  JceMesh **out_mesh)
{
    if (!scene || e == JCE_ENTITY_INVALID) return false;
    if (!jce_scene_has_transform(scene, e)) return false;

    /* Compose the full world matrix up the parent chain (roots → local). */
    *out_model = jce_scene_get_world_matrix(scene, e);

    if (out_mesh) {
        *out_mesh = NULL;
        if (jce_scene_has_mesh_renderer(scene, e) &&
            jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_MESH_RENDERER)) {
            JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
            *out_mesh = sr_resolve_mesh(sr, mr);
        }
        /* Terrain entities are NOT merged into a single mesh any more — they
         * are drawn chunk-by-chunk (sr_draw_terrain_chunks) with per-chunk
         * frustum culling + distance LOD.  Here we only ensure the cache slot
         * is loaded; *out_mesh stays NULL so the caller routes to the terrain
         * branch instead of the generic mesh path. */
        if (!*out_mesh && jce_scene_has_terrain(scene, e)) {
            JceTerrainComponent *tc = jce_scene_get_terrain(scene, e);
            if (tc && tc->visible && tc->terrain_path[0])
                (void)sr_terrain_find_or_load_slot(sr, tc->terrain_path);
        }
    }
    return true;
}

/* ── Terrain per-chunk draw (P1-terrain-lod) ──────────────────────────
 *
 * Replaces the old "merge every chunk into one ~16M-vert mesh" draw.  For each
 * terrain entity we walk its chunk grid and, per chunk:
 *   1. transform the chunk's local AABB by the entity world matrix,
 *   2. frustum-cull it against the camera (skip if fully outside),
 *   3. pick a LOD from the camera→chunk-centre distance,
 *   4. build/cache the chunk mesh (with skirts) at that LOD,
 *   5. re-bind transform + terrain textures/params and submit one draw.
 * Step 5 is repeated per chunk because jce_mesh_submit_terrain discards all
 * bound state (BGFX_DISCARD_ALL) after each submit. */

/* AABB-vs-frustum: returns true if the box is at least partially inside. */
static bool sr_aabb_in_frustum(const jce_vec4 planes[6],
                               jce_vec3 mn, jce_vec3 mx)
{
    for (int p = 0; p < 6; p++) {
        /* Positive vertex (farthest along the plane normal). If even that is
         * outside (signed distance < 0), the whole box is outside this plane. */
        float px = (planes[p].x >= 0.0f) ? mx.x : mn.x;
        float py = (planes[p].y >= 0.0f) ? mx.y : mn.y;
        float pz = (planes[p].z >= 0.0f) ? mx.z : mn.z;
        float d  = planes[p].x * px + planes[p].y * py + planes[p].z * pz
                 + planes[p].w;
        if (d < 0.0f) return false;
    }
    return true;
}

/* World-space AABB of a local box transformed by `m`. */
static void sr_transform_aabb(const jce_mat4 *m, jce_vec3 lmn, jce_vec3 lmx,
                              jce_vec3 *out_mn, jce_vec3 *out_mx)
{
    jce_vec3 wmn = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
    jce_vec3 wmx = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (int c = 0; c < 8; c++) {
        jce_vec4 lv = {
            (c & 1) ? lmx.x : lmn.x,
            (c & 2) ? lmx.y : lmn.y,
            (c & 4) ? lmx.z : lmn.z,
            1.0f
        };
        jce_vec4 wv = jce_m4_mul_v4(m, lv);
        if (wv.x < wmn.x) wmn.x = wv.x; if (wv.x > wmx.x) wmx.x = wv.x;
        if (wv.y < wmn.y) wmn.y = wv.y; if (wv.y > wmx.y) wmx.y = wv.y;
        if (wv.z < wmn.z) wmn.z = wv.z; if (wv.z > wmx.z) wmx.z = wv.z;
    }
    *out_mn = wmn;
    *out_mx = wmx;
}

static void sr_draw_terrain_chunks(JceSceneRenderer *sr, JceScene *scene,
                                   EntityList *list,
                                   const JceCamera *camera, uint16_t view_id,
                                   int slot, JceTerrainComponent *tc,
                                   const jce_mat4 *model,
                                   const JcePbrMaterial *pbr,
                                   const bgfx_texture_handle_t layer_tex[4])
{
    if (!sr || slot < 0 || slot >= 16) return;
    JceTerrain *terr = sr->terrain_cache[slot].terrain;
    if (!terr || sr->terrain_cache[slot].chunk_count <= 0) return;

    /* Lazy splat texture upload (once per terrain). */
    if (!sr->terrain_cache[slot].splat_uploaded) {
        int tw = jce_terrain_width(terr);
        int th = jce_terrain_height(terr);
        const uint32_t *splat = jce_terrain_splat(terr);
        if (splat && tw > 0 && th > 0) {
            const bgfx_memory_t *mem = bgfx_copy(splat, (uint32_t)(tw * th * 4));
            sr->terrain_cache[slot].splat_tex =
                bgfx_create_texture_2d((uint16_t)tw, (uint16_t)th, false, 1,
                    BGFX_TEXTURE_FORMAT_RGBA8,
                    BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem);
        }
        sr->terrain_cache[slot].splat_uploaded = true;
    }
    bgfx_texture_handle_t splat_h = sr->terrain_cache[slot].splat_tex;
    if (!BGFX_HANDLE_IS_VALID(splat_h)) splat_h = sr->white_tex;

    float tparams[4] = {
        (tc && tc->tile_scale > 0.0f) ? tc->tile_scale : 10.0f,
        (tc && tc->splat_enabled) ? 1.0f : 0.0f,
        0.0f, 0.0f
    };

    /* Frustum planes from the camera (independent of the entity-level cull
     * toggle so terrain always benefits from per-chunk culling). */
    jce_vec4 planes[6];
    bool have_planes = false;
    if (camera) {
        const jce_mat4 v  = jce_camera_view(camera);
        const jce_mat4 p  = jce_camera_proj(camera, 16.0f / 9.0f,
                                            sr->homogeneous_depth);
        const jce_mat4 vp = jce_m4_multiply(&p, &v);
        sr_extract_frustum_planes(&vp, planes);
        have_planes = true;
    }
    jce_vec3 cam_pos = camera ? jce_camera_get_position(camera)
                              : jce_v3(0, 0, 0);

    int ncx = sr->terrain_cache[slot].chunk_nx;
    int ncz = sr->terrain_cache[slot].chunk_nz;

    sr->stat_terrain_chunks_total   += ncx * ncz;

    for (int cz = 0; cz < ncz; cz++)
    for (int cx = 0; cx < ncx; cx++) {
        int idx = cz * ncx + cx;
        jce_vec3 lmn = sr->terrain_cache[slot].chunk_min[idx];
        jce_vec3 lmx = sr->terrain_cache[slot].chunk_max[idx];

        jce_vec3 wmn, wmx;
        sr_transform_aabb(model, lmn, lmx, &wmn, &wmx);

        if (have_planes && !sr_aabb_in_frustum(planes, wmn, wmx)) {
            sr->stat_terrain_chunks_culled++;
            continue;
        }

        /* LOD from camera distance to the chunk's world-space centre. */
        jce_vec3 ctr = { 0.5f * (wmn.x + wmx.x),
                         0.5f * (wmn.y + wmx.y),
                         0.5f * (wmn.z + wmx.z) };
        float dx = ctr.x - cam_pos.x;
        float dy = ctr.y - cam_pos.y;
        float dz = ctr.z - cam_pos.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        int lod = (int)(dist / SR_TERRAIN_LOD_DIST);
        if (lod < 0) lod = 0;
        if (lod > SR_TERRAIN_MAX_LOD) lod = SR_TERRAIN_MAX_LOD;

        JceMesh *cm = sr_terrain_chunk_mesh(sr, slot, cx, cz, lod);
        if (!cm) {
            /* Fall back to LOD 0 if the requested LOD collapsed (tiny chunk). */
            if (lod != 0) cm = sr_terrain_chunk_mesh(sr, slot, cx, cz, 0);
            if (!cm) continue;
        }

        /* Per-chunk state (re-bound every submit; BGFX_DISCARD_ALL clears it). */
        sr_inline_bind_pbr_global(sr, pbr, view_id, scene, list);
        bgfx_set_transform(model->raw[0], 1);
        bgfx_set_texture(0,  sr->s_terrain_layer0, layer_tex[0], UINT32_MAX);
        bgfx_set_texture(4,  sr->s_terrain_layer3, layer_tex[3], UINT32_MAX);
        bgfx_set_texture(13, sr->s_terrain_splat,  splat_h,      UINT32_MAX);
        bgfx_set_texture(14, sr->s_terrain_layer1, layer_tex[1], UINT32_MAX);
        bgfx_set_texture(15, sr->s_terrain_layer2, layer_tex[2], UINT32_MAX);
        bgfx_set_uniform(sr->u_terrain_params, tparams, 1);

        jce_mesh_submit_terrain(cm, sr->renderer, view_id);
        sr->stat_terrain_chunks_drawn++;
    }
}

/* If entity e is a terrain, submit its chunks as depth-only shadow casters
 * into `view_id` and return true (so the shadow loop skips the generic mesh
 * path).  Mirrors sr_try_submit_mesh_renderer_model_shadow.  Reuses any chunk
 * mesh already cached by the colour pass; otherwise builds it at the coarse
 * shadow LOD.  Returns false for non-terrain entities. */
static bool sr_try_submit_terrain_shadow(JceSceneRenderer *sr, JceScene *scene,
                                         JceEntity e, uint16_t view_id)
{
    if (!jce_scene_has_terrain(scene, e)) return false;
    JceTerrainComponent *tc = jce_scene_get_terrain(scene, e);
    if (!tc || !tc->visible || !tc->terrain_path[0]) return false;
    int slot = sr_terrain_find_or_load_slot(sr, tc->terrain_path);
    if (slot < 0 || sr->terrain_cache[slot].chunk_count <= 0) return false;
    if (!jce_scene_has_transform(scene, e)) return false;

    jce_mat4 model = jce_scene_get_world_matrix(scene, e);
    int ncx = sr->terrain_cache[slot].chunk_nx;
    int ncz = sr->terrain_cache[slot].chunk_nz;

    for (int cz = 0; cz < ncz; cz++)
    for (int cx = 0; cx < ncx; cx++) {
        int cidx = cz * ncx + cx;
        JceMesh *cm = sr->terrain_cache[slot].chunk_meshes[cidx];
        if (!cm) cm = sr_terrain_chunk_mesh(sr, slot, cx, cz,
                                            SR_TERRAIN_SHADOW_LOD);
        if (!cm) continue;
        bgfx_set_transform(model.raw[0], 1);
        jce_mesh_submit_shadow(cm, sr->renderer, view_id);
    }
    return true;
}

/* ── Light direction helpers ──────────────────────────────────────── */

static jce_vec3 sr_light_shine_direction(const jce_vec3 *comp_dir)
{
    jce_vec3 fallback = jce_v3(0.0f, -1.0f, 0.0f);
    if (!comp_dir) return fallback;
    float len2 = comp_dir->x * comp_dir->x +
                 comp_dir->y * comp_dir->y +
                 comp_dir->z * comp_dir->z;
    if (len2 < 1e-8f) return fallback;
    return jce_v3_scale(*comp_dir, 1.0f / sqrtf(len2));
}

static jce_vec3 sr_light_world_shine_direction(const jce_vec3 *comp_dir,
                                               const JceTransform *xf)
{
    jce_vec3 dir = sr_light_shine_direction(comp_dir);
    if (xf) {
        jce_quat q = jce_q_normalize(xf->rotation);
        dir = jce_q_rotate(q, dir);
    }
    return sr_light_shine_direction(&dir);
}

static bool sr_resolve_primary_dir_light(JceSceneRenderer *sr,
                                         JceScene *scene,
                                         EntityList *list,
                                         bool shadow_only,
                                         jce_vec3 *out_to_light,
                                         jce_vec3 *out_color,
                                         float *out_intensity)
{
    bool have_any = false; uint32_t dir_seen = 0;

    if (scene && list) {
        for (int i = 0; i < list->count; i++) {
            JceEntity e = list->entities[i];
            if (!entity_enabled(scene, e)) continue;
            if (!jce_scene_has_dir_light(scene, e)) continue;
            if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_DIR_LIGHT)) continue;
            if (dir_seen++ >= JCE_MAX_DIR_LIGHTS) continue;
            JceDirectionalLight *dl = jce_scene_get_dir_light(scene, e);
            if (!dl) continue;
            have_any = true;
            if (shadow_only && !dl->casts_shadow) continue;

            JceTransform *xf = jce_scene_get_transform(scene, e);
            jce_vec3 shine = sr_light_world_shine_direction(&dl->direction, xf);
            if (out_to_light) *out_to_light = jce_v3_scale(shine, -1.0f);
            if (out_color) *out_color = dl->color;
            if (out_intensity) *out_intensity = dl->intensity > 0.0f ? dl->intensity : 1.0f;
            return true;
        }
    }

    if (!have_any && sr && sr->tod_active) {
        /* tod_state.sun_direction is already a to-light unit vector. */
        if (out_to_light) *out_to_light = sr->tod_state.sun_direction;
        if (out_color) *out_color = sr->tod_state.sun_color;
        if (out_intensity) *out_intensity = 1.0f;
        return true;
    }

    return false;
}

/* ── Shadow VP / CSM helpers ──────────────────────────────────────── */

static void sr_compute_shadow_vp(JceSceneRenderer *sr, const jce_vec3 *light_dir,
                                 float shadow_vp[16])
{
    jce_vec3 center = jce_v3(0.0f, 0.0f, 0.0f);
    jce_vec3 ld = jce_v3_normalize(*light_dir);
    jce_vec3 light_pos = jce_v3_scale(ld, 80.0f);
    jce_vec3 up = (fabsf(ld.y) > 0.99f) ? jce_v3(0, 0, 1) : jce_v3(0, 1, 0);

    jce_mat4 view = jce_m4_look_at(light_pos, center, up);
    float S = SHADOW_ORTHO_SIZE;
    jce_mat4 proj = jce_m4_ortho(-S, S, -S, S, 0.1f, 200.0f, sr->homogeneous_depth);
    jce_mat4 vp = jce_m4_multiply(&proj, &view);
    memcpy(shadow_vp, vp.raw, 16 * sizeof(float));
}

static void sr_fill_csm_bias_scales(const JceCsmData *csm, float out_scales[4])
{
    float base_range = 0.1f;
    if (csm->cascade_count > 0) {
        base_range = csm->splits[1] - csm->splits[0];
        if (base_range < 0.0001f) base_range = 0.1f;
    }
    float last_scale = 1.0f;
    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
        float scale = last_scale;
        if (i < csm->cascade_count) {
            float range = csm->splits[i + 1] - csm->splits[i];
            if (range < 0.0001f) range = base_range;
            scale = range / base_range;
            if (scale < 1.0f)  scale = 1.0f;
            if (scale > 20.0f) scale = 20.0f;
            last_scale = scale;
        }
        out_scales[i] = scale;
    }
}

static void sr_bind_shadow_params(JceSceneRenderer *sr, float inv_map_size)
{
    if (!sr) return;
    float disabled_splits[4] = { 0, 0, 0, 0 };
    float params[4] = { inv_map_size, sr->csm_blend_ratio,
                        sr->csm_normal_bias, sr->csm_filter_radius };
    float bias_scales[4] = { 1, 1, 1, 1 };
    bgfx_set_uniform(sr->u_csm_splits, disabled_splits, 1);
    bgfx_set_uniform(sr->u_csm_params, params, 1);
    bgfx_set_uniform(sr->u_csm_bias_scales, bias_scales, 1);
}

static void sr_bind_shadow_uniforms_disabled(JceSceneRenderer *sr)
{
    sr_bind_shadow_params(sr, 0.0f);
}

/* P1 — bind the local (spot) shadow atlas + per-spot slot table. Bound for
 * every material run alongside the directional shadow state; slots default to
 * -1 (shader skips) when no spot casts a shadow. Stage 15 is shared with
 * terrain's layer2 — terrain rebinds 15 after this, so terrain receives only
 * directional shadows (fs_pbr.sc samples local shadows; fs_terrain.sc does not). */
static void sr_bind_local_shadow_state(JceSceneRenderer *sr)
{
    if (!BGFX_HANDLE_IS_VALID(sr->u_local_shadow_map)) return;

    bgfx_texture_handle_t tex = (sr->local_atlas_valid
                                 && BGFX_HANDLE_IS_VALID(sr->local_atlas_tex))
        ? sr->local_atlas_tex : sr->shadow_tex;
    if (BGFX_HANDLE_IS_VALID(tex))
        bgfx_set_texture(15, sr->u_local_shadow_map, tex, UINT32_MAX);

    bgfx_set_uniform(sr->u_local_shadow_vp, sr->frame_local_vp[0].raw[0],
                     JCE_MAX_LOCAL_SHADOWS);

    float slots[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    if (sr->frame_local_active) {
        for (int i = 0; i < JCE_MAX_SPOT_LIGHTS && i < 4; i++)
            slots[i] = sr->frame_spot_slot[i];
    }
    bgfx_set_uniform(sr->u_spot_shadow_slot, slots, 1);

    float pslots[8];   /* 8 point lanes -> 2 vec4 */
    for (int i = 0; i < JCE_MAX_POINT_LIGHTS && i < 8; i++)
        pslots[i] = sr->frame_local_active ? sr->frame_point_slot[i] : -1.0f;
    bgfx_set_uniform(sr->u_point_shadow_slot, pslots, 2);

    float inv_atlas = sr->shadow_map_size > 0
        ? 1.0f / (float)sr->shadow_map_size : 0.0f;
    float params[4] = { (float)JCE_LOCAL_SHADOW_TILES, inv_atlas,
                        sr->frame_local_bias, inv_atlas };
    bgfx_set_uniform(sr->u_local_shadow_params, params, 1);
}

static void sr_bind_frame_shadow_state(JceSceneRenderer *sr)
{
    if (!sr) return;
    sr_bind_local_shadow_state(sr);
    if (!sr->frame_shadow_active) {
        sr_bind_shadow_uniforms_disabled(sr);
        return;
    }

    if (sr->shadow_use_csm && sr->last_csm_valid) {
        const JceCsmData *csm = &sr->last_csm;
        bgfx_set_uniform(sr->u_csm_vp, csm->vp[0].raw[0], (uint16_t)csm->cascade_count);
        float splits_v4[4] = { 0, 0, 0, 0 };
        for (uint32_t ci = 0; ci < csm->cascade_count && ci < 4; ci++)
            splits_v4[ci] = csm->splits[ci + 1];
        bgfx_set_uniform(sr->u_csm_splits, splits_v4, 1);

        float csm_params[4] = { sr->shadow_map_size > 0 ? 1.0f / (float)sr->shadow_map_size : 0.0f,
            sr->csm_blend_ratio, sr->csm_normal_bias, sr->csm_filter_radius };
        bgfx_set_uniform(sr->u_csm_params, csm_params, 1);

        float bias_scales[4];
        sr_fill_csm_bias_scales(csm, bias_scales);
        bgfx_set_uniform(sr->u_csm_bias_scales, bias_scales, 1);

        for (uint32_t ci = 0;
             ci < sr->csm_cascade_count && ci < JCE_CSM_MAX_CASCADES; ci++)
            bgfx_set_texture((uint8_t)(9 + ci), sr->u_csm_samplers[ci], sr->csm_tex[ci], UINT32_MAX);
        return;
    }

    if (!sr->shadow_use_csm && sr->frame_shadow_vp_valid && BGFX_HANDLE_IS_VALID(sr->shadow_tex)) {
        bgfx_set_texture(5, sr->u_shadowMap, sr->shadow_tex, UINT32_MAX);
        bgfx_set_uniform(sr->u_shadowVP, sr->frame_shadow_vp, 1);
        sr_bind_shadow_params(sr, sr->shadow_map_size > 0
                              ? 1.0f / (float)sr->shadow_map_size : 0.0f);
        return;
    }

    sr_bind_shadow_uniforms_disabled(sr);
}

static void sr_apply_view_order(uint16_t view_id_base,
                                const JceSceneRenderConfig *cfg,
                                uint32_t csm_cascade_count)
{
    JceSceneRendererViewOrder order;
    bool include_fog_views = cfg && cfg->fog_enabled;
    uint8_t cascades = csm_cascade_count > JCE_CSM_MAX_CASCADES
        ? JCE_CSM_MAX_CASCADES : (uint8_t)csm_cascade_count;

    if (!jce_scene_renderer_view_order_build(
            view_id_base,
            cfg && cfg->draw_shadows,
            cascades,
            include_fog_views,
            &order))
        return;

    bgfx_set_view_order(order.first, order.count, order.order);
}

/* ── Sky pass ─────────────────────────────────────────────────────── */

static void sr_draw_sky_gradient(JceSceneRenderer *sr, uint16_t view_id)
{
    if (!BGFX_HANDLE_IS_VALID(sr->prog_sky)) return;

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;
    if (!bgfx_alloc_transient_buffers(&tvb, &sr->sky_layout, 4, &tib, 6, false))
        return;

    float *v = (float *)tvb.data;
    uint16_t *ix = (uint16_t *)tib.data;

    v[0] = -1.0f; v[1] = -1.0f; v[2]  = 0.0f;
    v[3] =  1.0f; v[4] = -1.0f; v[5]  = 0.0f;
    v[6] =  1.0f; v[7] =  1.0f; v[8]  = 0.0f;
    v[9] = -1.0f; v[10]=  1.0f; v[11] = 0.0f;

    ix[0] = 0; ix[1] = 1; ix[2] = 2;
    ix[3] = 0; ix[4] = 2; ix[5] = 3;

    float sky_colors[12] = {
        0.25f, 0.45f, 0.80f, 1.0f,
        0.65f, 0.78f, 0.92f, 1.0f,
        0.22f, 0.22f, 0.28f, 1.0f,
    };
    if (sr->tod_active) {
        const JceTimeOfDayState *t = &sr->tod_state;
        sky_colors[0]  = t->sky_top.x;     sky_colors[1]  = t->sky_top.y;
        sky_colors[2]  = t->sky_top.z;     sky_colors[3]  = 1.0f;
        sky_colors[4]  = t->sky_horizon.x; sky_colors[5]  = t->sky_horizon.y;
        sky_colors[6]  = t->sky_horizon.z; sky_colors[7]  = 1.0f;
        sky_colors[8]  = t->sky_ground.x;  sky_colors[9]  = t->sky_ground.y;
        sky_colors[10] = t->sky_ground.z;  sky_colors[11] = 1.0f;
    }
    bgfx_set_uniform(sr->u_sky_colors, sky_colors, 3);

    bgfx_texture_handle_t equirect_tex = { UINT16_MAX };
    float sky_params[4] = { 0.0f, 1.0f, 0.0f, 0.0f };

    if (sr->skybox_active && sr->skybox) {
        JceTexture jet = jce_skybox_get_equirect_texture(sr->skybox);
        equirect_tex.idx = jet.idx;
        if (BGFX_HANDLE_IS_VALID(equirect_tex)) {
            sky_params[0] = 1.0f;
            sky_params[1] = sr->skybox_exposure;
            sky_params[2] = sr->skybox_rotation * 0.0174533f;
            bgfx_set_texture(0, sr->u_sky_equirect, equirect_tex, UINT32_MAX);
        }
    }
    bgfx_set_uniform(sr->u_sky_params, sky_params, 1);

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, 4);
    bgfx_set_transient_index_buffer(&tib, 0, 6);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA, 0);

    jce_mat4 identity = jce_m4_identity();
    bgfx_set_transform(identity.raw[0], 1);
    bgfx_submit(view_id, sr->prog_sky, 0, BGFX_DISCARD_ALL);
}

/* ── Cloth pass (P3-C.4: render the soft-body grid in-game) ───────────
 *
 * Builds a transient pos+normal+uv mesh from the live solver node positions
 * each frame and submits it with the simple mesh program.  Runs in the color
 * view AFTER the entity pass, so it inherits the lighting uniforms.  Cloth
 * node positions are world-space, so the model transform is identity. */

typedef struct { JceSceneRenderer *sr; uint16_t view_id; } SrClothDrawCtx;

static void sr_draw_one_cloth(JceScene *scene, JceEntity e, void *ud)
{
    SrClothDrawCtx *ctx = (SrClothDrawCtx *)ud;
    JceSceneRenderer *sr = ctx->sr;

    JceClothComponent *cl = jce_scene_get_cloth(scene, e);
    if (!cl || cl->handle == 0 || cl->res_u < 2 || cl->res_v < 2) return;

    const uint32_t ru = cl->res_u, rv = cl->res_v;
    const uint32_t nodes = ru * rv;
    /* uint16 transient indices cap the grid; skip oversized patches. */
    if (nodes > 65535u) return;
    if (jce_cloth_node_count((JceClothHandle)cl->handle) != nodes) return;

    float *pos = (float *)JCE_MALLOC((size_t)nodes * 3u * sizeof(float));
    if (!pos) return;
    if (!jce_cloth_get_positions((JceClothHandle)cl->handle, pos, nodes * 3u)) {
        JCE_FREE(pos);
        return;
    }

    const uint32_t quads       = (ru - 1u) * (rv - 1u);
    const uint32_t num_indices = quads * 6u;

    bgfx_vertex_layout_t layout;
    bgfx_vertex_layout_begin(&layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_POSITION,  3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_NORMAL,    3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&layout);

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;
    if (!bgfx_alloc_transient_buffers(&tvb, &layout, nodes, &tib, num_indices, false)) {
        JCE_FREE(pos);
        return;
    }

    float *vtx = (float *)tvb.data;   /* 8 floats/vertex: pos(3) normal(3) uv(2) */
    for (uint32_t v = 0; v < rv; ++v) {
        for (uint32_t u = 0; u < ru; ++u) {
            const uint32_t i = v * ru + u;
            /* Central-difference grid normal (clamped at edges). */
            const uint32_t iu0 = (u > 0) ? i - 1u : i;
            const uint32_t iu1 = (u + 1u < ru) ? i + 1u : i;
            const uint32_t iv0 = (v > 0) ? i - ru : i;
            const uint32_t iv1 = (v + 1u < rv) ? i + ru : i;
            const float dux = pos[iu1*3+0] - pos[iu0*3+0];
            const float duy = pos[iu1*3+1] - pos[iu0*3+1];
            const float duz = pos[iu1*3+2] - pos[iu0*3+2];
            const float dvx = pos[iv1*3+0] - pos[iv0*3+0];
            const float dvy = pos[iv1*3+1] - pos[iv0*3+1];
            const float dvz = pos[iv1*3+2] - pos[iv0*3+2];
            float nx = duy*dvz - duz*dvy;
            float ny = duz*dvx - dux*dvz;
            float nz = dux*dvy - duy*dvx;
            const float len = sqrtf(nx*nx + ny*ny + nz*nz);
            if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
            else { nx = 0.0f; ny = 1.0f; nz = 0.0f; }

            float *o = vtx + (size_t)i * 8u;
            o[0] = pos[i*3+0]; o[1] = pos[i*3+1]; o[2] = pos[i*3+2];
            o[3] = nx; o[4] = ny; o[5] = nz;
            o[6] = (float)u / (float)(ru - 1u);
            o[7] = (float)v / (float)(rv - 1u);
        }
    }

    uint16_t *idx = (uint16_t *)tib.data;
    uint32_t k = 0;
    for (uint32_t v = 0; v + 1u < rv; ++v) {
        for (uint32_t u = 0; u + 1u < ru; ++u) {
            const uint16_t i00 = (uint16_t)(v * ru + u);
            const uint16_t i10 = (uint16_t)(v * ru + u + 1u);
            const uint16_t i01 = (uint16_t)((v + 1u) * ru + u);
            const uint16_t i11 = (uint16_t)((v + 1u) * ru + u + 1u);
            idx[k++] = i00; idx[k++] = i01; idx[k++] = i10;
            idx[k++] = i10; idx[k++] = i01; idx[k++] = i11;
        }
    }

    JCE_FREE(pos);

    JceUniformHandle uh = jce_renderer_get_tex_uniform(sr->renderer);
    bgfx_uniform_handle_t su = { uh.idx };
    if (BGFX_HANDLE_IS_VALID(su) && BGFX_HANDLE_IS_VALID(sr->white_tex))
        bgfx_set_texture(0, su, sr->white_tex, UINT32_MAX);

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, nodes);
    bgfx_set_transient_index_buffer(&tib, 0, num_indices);
    /* Double-sided (no cull) — a cloth sheet is visible from both faces. */
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z
                 | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA, 0);

    jce_mat4 identity = jce_m4_identity();
    bgfx_set_transform(identity.raw[0], 1);

    JceShaderHandle sh = jce_renderer_get_program_mesh(sr->renderer);
    bgfx_program_handle_t prog = { sh.idx };
    if (BGFX_HANDLE_IS_VALID(prog))
        bgfx_submit(ctx->view_id, prog, 0, BGFX_DISCARD_ALL);
}

static void sr_draw_cloth(JceSceneRenderer *sr, JceScene *scene, uint16_t view_id)
{
    if (!sr || !scene) return;
    SrClothDrawCtx ctx = { sr, view_id };
    jce_scene_each_entity(scene, sr_draw_one_cloth, &ctx);
}

/* ── Shadow pass ──────────────────────────────────────────────────── */

static void sr_destroy_shadow_targets(JceSceneRenderer *sr)
{
    if (!sr) return;
    if (BGFX_HANDLE_IS_VALID(sr->shadow_fbo)) {
        bgfx_destroy_frame_buffer(sr->shadow_fbo);
        sr->shadow_fbo.idx = UINT16_MAX;
    }
    if (BGFX_HANDLE_IS_VALID(sr->shadow_tex)) {
        bgfx_destroy_texture(sr->shadow_tex);
        sr->shadow_tex.idx = UINT16_MAX;
    }
    if (BGFX_HANDLE_IS_VALID(sr->local_atlas_fbo)) {
        bgfx_destroy_frame_buffer(sr->local_atlas_fbo);
        sr->local_atlas_fbo.idx = UINT16_MAX;
    }
    if (BGFX_HANDLE_IS_VALID(sr->local_atlas_tex)) {
        bgfx_destroy_texture(sr->local_atlas_tex);
        sr->local_atlas_tex.idx = UINT16_MAX;
    }
    sr->local_atlas_valid = false;
    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
        if (BGFX_HANDLE_IS_VALID(sr->csm_fbo[i])) {
            bgfx_destroy_frame_buffer(sr->csm_fbo[i]);
            sr->csm_fbo[i].idx = UINT16_MAX;
        }
        if (BGFX_HANDLE_IS_VALID(sr->csm_tex[i])) {
            bgfx_destroy_texture(sr->csm_tex[i]);
            sr->csm_tex[i].idx = UINT16_MAX;
        }
    }
    sr->shadow_valid = false;
    sr->csm_valid = false;
    sr->last_csm_valid = false;
}

static void sr_create_shadow_targets(JceSceneRenderer *sr)
{
    if (!sr || sr->shadow_map_size == 0) return;

    const uint16_t sz = sr->shadow_map_size;
    const bgfx_texture_format_t depth_fmt = sr->shadow_depth_fmt;
    bgfx_attachment_t at;

    sr->shadow_tex = bgfx_create_texture_2d(sz, sz, false, 1, depth_fmt,
        BGFX_TEXTURE_RT
        | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
        | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
        NULL);
    memset(&at, 0, sizeof(at));
    bgfx_attachment_init(&at, sr->shadow_tex, BGFX_ACCESS_WRITE,
                         0, 1, 0, BGFX_RESOLVE_NONE);
    sr->shadow_fbo = bgfx_create_frame_buffer_from_attachment(1, &at, false);
    sr->shadow_valid = BGFX_HANDLE_IS_VALID(sr->shadow_fbo);

    sr->csm_valid = true;
    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
        sr->csm_tex[i] = bgfx_create_texture_2d(sz, sz, false, 1, depth_fmt,
            BGFX_TEXTURE_RT
            | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
            | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
            NULL);
        memset(&at, 0, sizeof(at));
        bgfx_attachment_init(&at, sr->csm_tex[i], BGFX_ACCESS_WRITE,
                             0, 1, 0, BGFX_RESOLVE_NONE);
        sr->csm_fbo[i] = bgfx_create_frame_buffer_from_attachment(1, &at, false);
        if (!BGFX_HANDLE_IS_VALID(sr->csm_fbo[i]))
            sr->csm_valid = false;
    }

    /* Local (spot/point) shadow atlas: one square depth texture, NxN tiles. */
    sr->local_atlas_tex = bgfx_create_texture_2d(sz, sz, false, 1, depth_fmt,
        BGFX_TEXTURE_RT
        | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
        | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
        NULL);
    memset(&at, 0, sizeof(at));
    bgfx_attachment_init(&at, sr->local_atlas_tex, BGFX_ACCESS_WRITE,
                         0, 1, 0, BGFX_RESOLVE_NONE);
    sr->local_atlas_fbo = bgfx_create_frame_buffer_from_attachment(1, &at, false);
    sr->local_atlas_valid = BGFX_HANDLE_IS_VALID(sr->local_atlas_fbo);
}

static void sr_ensure_shadow_map_size(JceSceneRenderer *sr, uint16_t size)
{
    if (!sr || size == 0) return;
    if (size < 512) size = 512;
    if (size > 4096) size = 4096;
    if (sr->shadow_map_size == size &&
        BGFX_HANDLE_IS_VALID(sr->shadow_fbo) &&
        BGFX_HANDLE_IS_VALID(sr->csm_fbo[0]))
        return;

    sr_destroy_shadow_targets(sr);
    sr->shadow_map_size = size;
    sr_create_shadow_targets(sr);
    sr->shadow_near_valid = false;
    sr->shadow_far_valid = false;
}

static void sr_draw_shadow_pass(JceSceneRenderer *sr, JceScene *scene,
                                const JceCamera *camera, EntityList *list,
                                uint16_t view_id_base, uint32_t vp_w,
                                uint32_t vp_h, float shadow_distance,
                                float split_lambda)
{
    sr->shadow_use_csm = false; sr->last_csm_valid = false;
    sr->frame_shadow_active = false; sr->frame_shadow_vp_valid = false;
    sr_bind_shadow_uniforms_disabled(sr);

    if (!sr->shadow_valid) return;

    JceShaderHandle shadow_sh = jce_renderer_get_program_shadow(sr->renderer);
    if (shadow_sh.idx == UINT16_MAX) return;

    JceShaderHandle shadow_inst_sh = jce_renderer_get_program_shadow_inst(sr->renderer);

    /* Queue path: enabled by default (JCE_USE_RQ=0 disables) + render_queue +
     * instanced shadow program available. Pushes per-entity per-cascade
     * entries to the queue with view_id as part of the batch key — auto-
     * batched into one submit per (view_id, mesh) pair. */
    static int s_use_rq_shadow_env = -1;
    if (s_use_rq_shadow_env < 0) {
        const char *v = getenv("JCE_USE_RQ");
        s_use_rq_shadow_env = (v && v[0] == '0') ? 0 : 1;
    }
    bool use_rq_shadow = s_use_rq_shadow_env
                      && sr->render_queue
                      && shadow_inst_sh.idx != UINT16_MAX;

    const bool use_csm = sr->csm_valid && sr->csm_cascade_count > 0
                      && jce_render_pipeline_is_feature_enabled("csm");
    jce_vec3 shadow_dir;
    if (!sr_resolve_primary_dir_light(sr, scene, list, true, &shadow_dir, NULL, NULL))
        return;

    /* Compute shadow view IDs from base. */
    const uint16_t shadow_view_0 = (uint16_t)(view_id_base + 10);

    if (!use_csm) {
        float shadow_vp[16];
        sr_compute_shadow_vp(sr, &shadow_dir, shadow_vp);

        bgfx_set_view_rect(shadow_view_0, 0, 0,
                           sr->shadow_map_size, sr->shadow_map_size);
        bgfx_set_view_frame_buffer(shadow_view_0, sr->shadow_fbo);
        bgfx_set_view_clear(shadow_view_0, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);

        float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        bgfx_set_view_transform(shadow_view_0, identity, shadow_vp);
        bgfx_set_uniform(sr->u_shadowVP, shadow_vp, 1);
        bgfx_touch(shadow_view_0);

        memcpy(sr->frame_shadow_vp, shadow_vp, sizeof(shadow_vp));
        sr->frame_shadow_vp_valid = true; sr->frame_shadow_active = true;

        if (use_rq_shadow) {
            jce_rq_clear(sr->render_queue);
            jce_rq_set_material_binder(sr->render_queue, NULL, NULL);
            for (int i = 0; i < list->count; i++) {
                JceEntity e = list->entities[i];
                if (!entity_enabled(scene, e)) continue;
                if (sr_try_submit_skinned_shadow(sr, scene, e, shadow_view_0))
                    continue;
                if (sr_try_submit_mesh_renderer_model_shadow(sr, scene, e, shadow_view_0))
                    continue;
                if (sr_try_submit_terrain_shadow(sr, scene, e, shadow_view_0))
                    continue;
                jce_mat4 model;
                JceMesh *mesh = NULL;
                if (!sr_build_entity_model(sr, scene, e, &model, &mesh)) continue;
                if (!mesh) continue;
                JceDrawCmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.view_id        = shadow_view_0;
                cmd.program        = (uint16_t)shadow_inst_sh.idx;
                cmd.program_single = (uint16_t)shadow_sh.idx;
                cmd.mesh_vbh       = jce_mesh_get_vbh(mesh);
                cmd.mesh_ibh       = jce_mesh_get_ibh(mesh);
                cmd.index_count    = jce_mesh_index_count(mesh);
                cmd.transform      = model;
                cmd.depth          = 0.0f;
                cmd.material_key   = 1u; /* single shadow material */
                /* Depth-only shadow state (matches jce_mesh_submit_shadow). */
                cmd.state          = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                                   | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA;
                jce_rq_push(sr->render_queue, &cmd);
            }
            if (jce_rq_count(sr->render_queue) > 0) {
                jce_rq_sort(sr->render_queue, JCE_SORT_FOR_INSTANCING);
                sr_rq_flush_and_collect(sr);
            }
            return;
        }

        for (int i = 0; i < list->count; i++) {
            JceEntity e = list->entities[i];
            if (!entity_enabled(scene, e)) continue;
            if (sr_try_submit_skinned_shadow(sr, scene, e, shadow_view_0))
                continue;
            if (sr_try_submit_mesh_renderer_model_shadow(sr, scene, e, shadow_view_0))
                continue;
            if (sr_try_submit_terrain_shadow(sr, scene, e, shadow_view_0))
                continue;
            jce_mat4 model;
            JceMesh *mesh = NULL;
            if (!sr_build_entity_model(sr, scene, e, &model, &mesh)) continue;
            if (!mesh) continue;
            bgfx_set_transform(model.raw[0], 1);
            jce_mesh_submit_shadow(mesh, sr->renderer, shadow_view_0);
        }
        return;
    }

    sr->shadow_use_csm = true;

    /* CSM cascades. */
    float cam_near = camera ? jce_camera_get_near(camera) : 0.1f;
    float cam_far  = camera ? jce_camera_get_far(camera)  : 200.0f;
    float cam_fov  = camera ? jce_camera_get_fov(camera)  : 45.0f;
    float aspect   = (vp_w > 0 && vp_h > 0)
        ? ((float)vp_w / (float)vp_h) : (16.0f / 9.0f);

    if (cam_near <= 0.0f) cam_near = 0.1f;
    if (cam_far <= cam_near) cam_far = cam_near + 200.0f;

    float shadow_near_target = cam_near;
    if (shadow_near_target < 0.001f) shadow_near_target = 0.001f;
    if (shadow_near_target > 1.0f)   shadow_near_target = 1.0f;

    if (!sr->shadow_near_valid) {
        float bucket = 0.005f;
        sr->shadow_near_cached =
            ceilf(shadow_near_target / bucket) * bucket;
        sr->shadow_near_valid = true;
    } else {
        float diff = fabsf(shadow_near_target - sr->shadow_near_cached);
        float trigger = fmaxf(0.01f, sr->shadow_near_cached * 0.10f);
        if (diff > trigger) {
            float bucket = 0.01f;
            sr->shadow_near_cached =
                ceilf(shadow_near_target / bucket) * bucket;
        }
    }
    float shadow_near = sr->shadow_near_cached;
    if (shadow_near < 0.001f) shadow_near = 0.001f;

    float shadow_far_target = cam_far;
    if (shadow_distance > 0.0f) {
        shadow_far_target = fminf(shadow_far_target, shadow_distance);
        shadow_far_target = fmaxf(shadow_far_target, shadow_near + 1.0f);
    } else {
        shadow_far_target = fminf(shadow_far_target,
            fmaxf(shadow_near * CSM_DIST_SCALE, CSM_DIST_MAX));
        shadow_far_target = fmaxf(shadow_far_target,
                                  shadow_near + CSM_DIST_MIN);
    }

    if (!sr->shadow_far_valid) {
        sr->shadow_far_cached = shadow_far_target;
        sr->shadow_far_valid = true;
    } else {
        /* Deadband + snap-to-target tracking.  Earlier code applied an
         * exponential lerp every frame which meant the cached value
         * micro-wobbled forever — that propagates into per-cascade radius
         * and texel_size, breaking the texel-snap stability and producing
         * the parallel-stripe shimmer the user reported.
         *
         * Strategy: only update when the target moves by >5% (or >2 m).
         * On update, snap to a coarse 1 m bucket so the cached far moves
         * in discrete steps.  This is what stabilises CSM during free
         * camera movement (Unity's CullingResults.shadowDistance uses a
         * similar coarse bucketing). */
        float diff = fabsf(shadow_far_target - sr->shadow_far_cached);
        float trigger = fmaxf(2.0f, sr->shadow_far_cached * 0.05f);
        if (diff > trigger) {
            float bucket = 1.0f;
            sr->shadow_far_cached = ceilf(shadow_far_target / bucket) * bucket;
        }
    }
    float shadow_far = sr->shadow_far_cached;
    if (shadow_far <= shadow_near)
        shadow_far = shadow_near + CSM_DIST_MIN;

    jce_mat4 cam_view = camera ? jce_camera_view(camera) : jce_m4_identity();
    jce_vec3 light_dir = shadow_dir;

    /* Skip the per-cascade frustum/sphere/matrix recompute when none of the
       inputs changed since last frame (static camera + light). The key compare
       (~25 float eqs + one mat4 memcmp) is far cheaper than jce_csm_compute().
       Exact compare is safe: identical inputs are bit-identical (jce_camera_view
       is deterministic, shadow_far is coarse-bucketed) and any change recomputes. */
    JceCsmData csm;
    bool csm_changed =
        !sr->csm_key.valid ||
        sr->csm_key.cascades != sr->csm_cascade_count ||
        sr->csm_key.map_size != sr->shadow_map_size ||
        sr->csm_key.homog    != sr->homogeneous_depth ||
        sr->csm_key.znear    != shadow_near ||
        sr->csm_key.zfar     != shadow_far ||
        sr->csm_key.fov      != cam_fov ||
        sr->csm_key.aspect   != aspect ||
        sr->csm_key.lambda   != split_lambda ||
        sr->csm_key.light_dir.x != light_dir.x ||
        sr->csm_key.light_dir.y != light_dir.y ||
        sr->csm_key.light_dir.z != light_dir.z ||
        memcmp(&sr->csm_key.view, &cam_view, sizeof(jce_mat4)) != 0;

    if (csm_changed) {
        jce_csm_compute(&csm, sr->csm_cascade_count,
                        shadow_near, shadow_far, cam_fov, aspect,
                        &cam_view, &light_dir,
                        sr->homogeneous_depth, sr->shadow_map_size,
                        split_lambda);
        sr->csm_key.valid     = true;
        sr->csm_key.cascades  = sr->csm_cascade_count;
        sr->csm_key.map_size  = sr->shadow_map_size;
        sr->csm_key.homog     = sr->homogeneous_depth;
        sr->csm_key.znear     = shadow_near;
        sr->csm_key.zfar      = shadow_far;
        sr->csm_key.fov       = cam_fov;
        sr->csm_key.aspect    = aspect;
        sr->csm_key.lambda    = split_lambda;
        sr->csm_key.light_dir = light_dir;
        sr->csm_key.view      = cam_view;
    } else {
        csm = sr->last_csm;
    }

    sr->last_csm = csm;
    sr->last_csm_valid = true;
    sr->frame_shadow_active = true;

    if (use_rq_shadow) {
        jce_rq_clear(sr->render_queue);
        jce_rq_set_material_binder(sr->render_queue, NULL, NULL);
        for (uint32_t c = 0; c < csm.cascade_count && c < JCE_CSM_MAX_CASCADES; c++) {
            uint16_t cv = (uint16_t)(view_id_base + 11 + c);

            bgfx_set_view_rect(cv, 0, 0, sr->shadow_map_size, sr->shadow_map_size);
            bgfx_set_view_frame_buffer(cv, sr->csm_fbo[c]);
            bgfx_set_view_clear(cv, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);

            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            bgfx_set_view_transform(cv, identity, csm.vp[c].raw[0]);
            bgfx_touch(cv);

            for (int i = 0; i < list->count; i++) {
                JceEntity e = list->entities[i];
                if (!entity_enabled(scene, e)) continue;
                if (sr_try_submit_skinned_shadow(sr, scene, e, cv))
                    continue;
                if (sr_try_submit_mesh_renderer_model_shadow(sr, scene, e, cv))
                    continue;
                if (sr_try_submit_terrain_shadow(sr, scene, e, cv))
                    continue;
                jce_mat4 model;
                JceMesh *mesh = NULL;
                if (!sr_build_entity_model(sr, scene, e, &model, &mesh)) continue;
                if (!mesh) continue;
                JceDrawCmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.view_id        = cv;
                cmd.program        = (uint16_t)shadow_inst_sh.idx;
                cmd.program_single = (uint16_t)shadow_sh.idx;
                cmd.mesh_vbh       = jce_mesh_get_vbh(mesh);
                cmd.mesh_ibh       = jce_mesh_get_ibh(mesh);
                cmd.index_count    = jce_mesh_index_count(mesh);
                cmd.transform      = model;
                cmd.depth          = 0.0f;
                cmd.material_key   = 1u;
                /* Depth-only shadow state (matches jce_mesh_submit_shadow). */
                cmd.state          = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                                   | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA;
                jce_rq_push(sr->render_queue, &cmd);
            }
        }
        if (jce_rq_count(sr->render_queue) > 0) {
            jce_rq_sort(sr->render_queue, JCE_SORT_FOR_INSTANCING);
            sr_rq_flush_and_collect(sr);
        }
    } else {
        for (uint32_t c = 0; c < csm.cascade_count && c < JCE_CSM_MAX_CASCADES; c++) {
            uint16_t cv = (uint16_t)(view_id_base + 11 + c);

            bgfx_set_view_rect(cv, 0, 0, sr->shadow_map_size, sr->shadow_map_size);
            bgfx_set_view_frame_buffer(cv, sr->csm_fbo[c]);
            bgfx_set_view_clear(cv, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);

            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            bgfx_set_view_transform(cv, identity, csm.vp[c].raw[0]);
            bgfx_touch(cv);

            for (int i = 0; i < list->count; i++) {
                JceEntity e = list->entities[i];
                if (!entity_enabled(scene, e)) continue;
                if (sr_try_submit_skinned_shadow(sr, scene, e, cv))
                    continue;
                if (sr_try_submit_mesh_renderer_model_shadow(sr, scene, e, cv))
                    continue;
                if (sr_try_submit_terrain_shadow(sr, scene, e, cv))
                    continue;
                jce_mat4 model;
                JceMesh *mesh = NULL;
                if (!sr_build_entity_model(sr, scene, e, &model, &mesh)) continue;
                if (!mesh) continue;
                bgfx_set_transform(model.raw[0], 1);
                jce_mesh_submit_shadow(mesh, sr->renderer, cv);
            }
        }
    }

    sr_bind_frame_shadow_state(sr);
}

/* Render one local-shadow tile: lazily clear the whole atlas once (D3D clears
 * the full target, so per-tile clears would wipe earlier tiles), set up the
 * tile's view (rect + light VP), and submit all casters (reusing the skinned +
 * static depth helpers so animated casters deform their local shadows too).
 * Stores the VP into frame_local_vp[slot]. */
static void sr_local_shadow_render_tile(JceSceneRenderer *sr, JceScene *scene,
                                        EntityList *list, uint16_t view_id_base,
                                        const jce_mat4 *vp, uint32_t slot,
                                        const float *ident, bool *cleared)
{
    uint16_t tx, ty, tsz;
    if (!jce_local_shadow_atlas_tile(slot, sr->shadow_map_size,
                                     JCE_LOCAL_SHADOW_TILES, &tx, &ty, &tsz))
        return;

    if (!*cleared) {
        const uint16_t clear_view =
            (uint16_t)(view_id_base + JCE_VIEW_LOCAL_SHADOW_OFFSET);
        bgfx_set_view_rect(clear_view, 0, 0,
                           sr->shadow_map_size, sr->shadow_map_size);
        bgfx_set_view_frame_buffer(clear_view, sr->local_atlas_fbo);
        bgfx_set_view_clear(clear_view, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
        bgfx_set_view_transform(clear_view, ident, ident);
        bgfx_touch(clear_view);
        *cleared = true;
    }

    const uint16_t lv = (uint16_t)(view_id_base
                          + JCE_VIEW_LOCAL_SHADOW_OFFSET + 1 + slot);
    bgfx_set_view_rect(lv, tx, ty, tsz, tsz);
    bgfx_set_view_frame_buffer(lv, sr->local_atlas_fbo);
    bgfx_set_view_clear(lv, 0, 0, 1.0f, 0);   /* cleared once above */
    bgfx_set_view_transform(lv, ident, vp->raw[0]);
    bgfx_touch(lv);

    for (int j = 0; j < list->count; j++) {
        JceEntity ee = list->entities[j];
        if (!entity_enabled(scene, ee)) continue;
        if (sr_try_submit_skinned_shadow(sr, scene, ee, lv)) continue;
        if (sr_try_submit_mesh_renderer_model_shadow(sr, scene, ee, lv)) continue;
        if (sr_try_submit_terrain_shadow(sr, scene, ee, lv)) continue;
        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (!sr_build_entity_model(sr, scene, ee, &model, &mesh)) continue;
        if (!mesh) continue;
        bgfx_set_transform(model.raw[0], 1);
        jce_mesh_submit_shadow(mesh, sr->renderer, lv);
    }

    sr->frame_local_vp[slot] = *vp;
}

/* P1 — local (spot + point) shadow producer pass. Renders up to
 * JCE_MAX_LOCAL_SHADOWS shadow-casting SPOT and POINT lights as perspective
 * depth tiles sharing ONE shadow atlas (1 full-atlas clear at view base+4 +
 * one view per tile at base+5+slot). Spots use their cone FOV aimed along the
 * spot direction; points use a single wide-FOV frustum aimed straight DOWN
 * (v1 hemisphere approximation — good for elevated point lights). Spot/point
 * index alignment with the shader's u_spotLights[]/u_pointLights[] is
 * guaranteed by iterating `list` in the SAME order + caps as the light gather.
 * Reuses the depth submit helpers, so animated casters deform their local
 * shadows too (skinned path). */
static void sr_draw_local_shadow_pass(JceSceneRenderer *sr, JceScene *scene,
                                      EntityList *list, uint16_t view_id_base)
{
    sr->frame_local_active = false;
    sr->frame_local_count  = 0;
    for (uint32_t i = 0; i < JCE_MAX_SPOT_LIGHTS; i++)
        sr->frame_spot_slot[i] = -1.0f;
    for (uint32_t i = 0; i < JCE_MAX_POINT_LIGHTS; i++)
        sr->frame_point_slot[i] = -1.0f;

    if (!sr->local_atlas_valid || !list) return;
    if (jce_renderer_get_program_shadow(sr->renderer).idx == UINT16_MAX) return;

    const bool homog = sr->homogeneous_depth;
    float ident[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    uint32_t slot    = 0;   /* shared spot+point atlas slot pool */
    bool     cleared = false;

    /* Spots — index aligns with u_spotLights[]. */
    uint32_t spot_idx = 0;
    for (int li = 0; li < list->count && slot < JCE_MAX_LOCAL_SHADOWS; li++) {
        JceEntity e = list->entities[li];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_spot_light(scene, e)) continue;
        JceSpotLight *sl = jce_scene_get_spot_light(scene, e);
        if (!sl) continue;
        const uint32_t my_spot = spot_idx++;
        if (my_spot >= JCE_MAX_SPOT_LIGHTS) break;
        if (!sl->casts_shadow) continue;

        JceTransform *xf = jce_scene_get_transform(scene, e);
        jce_vec3 pos    = xf ? xf->position : sl->position;
        jce_vec3 dir    = sr_light_world_shine_direction(&sl->direction, xf);
        float    radius = sl->radius > 0.0f ? sl->radius : 10.0f;
        float    fov    = 2.0f * acosf(sl->outer_cone_cos);
        jce_mat4 vp = jce_local_shadow_vp(pos, dir, fov,
                                          0.05f * radius, radius, homog);
        sr_local_shadow_render_tile(sr, scene, list, view_id_base,
                                    &vp, slot, ident, &cleared);
        sr->frame_spot_slot[my_spot] = (float)slot;
        slot++;
    }

    /* Points — index aligns with u_pointLights[]; share the slot pool. v1 aims
       a single wide-FOV frustum straight down (hemisphere approximation). */
    uint32_t point_idx = 0;
    for (int li = 0; li < list->count && slot < JCE_MAX_LOCAL_SHADOWS; li++) {
        JceEntity e = list->entities[li];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_point_light(scene, e)) continue;
        JcePointLight *pl = jce_scene_get_point_light(scene, e);
        if (!pl) continue;
        const uint32_t my_point = point_idx++;
        if (my_point >= JCE_MAX_POINT_LIGHTS) break;
        if (!pl->casts_shadow) continue;

        JceTransform *xf = jce_scene_get_transform(scene, e);
        jce_vec3 pos    = xf ? xf->position : pl->position;
        float    radius = pl->radius > 0.0f ? pl->radius : 10.0f;
        jce_mat4 vp = jce_local_shadow_vp(pos, jce_v3(0.0f, -1.0f, 0.0f),
                                          JCE_POINT_SHADOW_FOV,
                                          0.05f * radius, radius, homog);
        sr_local_shadow_render_tile(sr, scene, list, view_id_base,
                                    &vp, slot, ident, &cleared);
        sr->frame_point_slot[my_point] = (float)slot;
        slot++;
    }

    sr->frame_local_count  = slot;
    sr->frame_local_active = slot > 0;
    sr->frame_local_bias   = 0.0015f;   /* default depth bias; tune by eye */
}

/* ── Skybox scan ──────────────────────────────────────────────────── */

static void sr_scan_skybox(JceSceneRenderer *sr, JceScene *scene, EntityList *list)
{
    const char *hdr_path = NULL;
    float rotation = 0.0f;
    float exposure = 1.0f;

    for (int i = 0; i < list->count && !hdr_path; i++) {
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_skybox(scene, e)) continue;
        JceSkyboxComponent *c = jce_scene_get_skybox(scene, e);
        if (!c || c->hdr_path[0] == '\0') continue;
        hdr_path = c->hdr_path;
        rotation = c->rotation;
        exposure = c->exposure > 0.0f ? c->exposure : 1.0f;
    }

    if (hdr_path && strcmp(hdr_path, sr->skybox_hdr_path) != 0) {
        if (sr->ibl_data) { jce_ibl_destroy(sr->ibl_data); sr->ibl_data = NULL; }
        if (sr->skybox)   { jce_skybox_destroy(sr->skybox); sr->skybox = NULL; }
        /* Try PAK chain first (engine + bundle overlays) so a bundled
         * HDR works without a sidecar file on disk.  Fall back to the
         * host filesystem for user-authored / loose HDRs. */
        const JcePakAsset *hdr_asset = jce_pak_find(sr->pak, hdr_path);
        if (hdr_asset && hdr_asset->original_size > 0) {
            void *hdr_buf = JCE_MALLOC((size_t)hdr_asset->original_size);
            if (hdr_buf) {
                size_t got = jce_pak_decompress_ex(sr->pak, hdr_asset,
                                                  hdr_buf,
                                                  (size_t)hdr_asset->original_size);
                if (got == (size_t)hdr_asset->original_size)
                    sr->skybox = jce_skybox_create_from_hdr_memory(
                                     hdr_buf, (uint32_t)got, 512);
                JCE_FREE(hdr_buf);
            }
        }
        if (!sr->skybox)
            sr->skybox = jce_skybox_create_from_hdr_file(hdr_path, 512);
        if (sr->skybox) {
            snprintf(sr->skybox_hdr_path, sizeof(sr->skybox_hdr_path),
                     "%s", hdr_path);
            sr->skybox_active = true;
            /* Resurrect runtime IBL: convolve the skybox's already-decoded
             * CPU HDR pixels into irradiance + specular-prefilter cubemaps.
             * bgfx GPU textures can't be read back, so we feed the CPU
             * pixel buffer directly (the GPU-texture path returns NULL). */
            uint32_t eqw = 0, eqh = 0;
            const float *eqpx =
                jce_skybox_get_equirect_pixels(sr->skybox, &eqw, &eqh);
            if (eqpx && eqw > 0 && eqh > 0)
                sr->ibl_data =
                    jce_ibl_generate_from_pixels(eqpx, eqw, eqh, 32, 128);
            if (sr->ibl_data)
                LOG_INFO(LOG_TAG, "skybox loaded + IBL ready: %s", hdr_path);
            else
                LOG_INFO(LOG_TAG, "skybox loaded (no IBL): %s", hdr_path);
        } else {
            sr->skybox_hdr_path[0] = '\0';
            sr->skybox_active = false;
        }
    } else if (!hdr_path && sr->skybox_active) {
        if (sr->ibl_data) { jce_ibl_destroy(sr->ibl_data); sr->ibl_data = NULL; }
        if (sr->skybox)   { jce_skybox_destroy(sr->skybox); sr->skybox = NULL; }
        sr->skybox_hdr_path[0] = '\0';
        sr->skybox_active = false;
    }

    sr->skybox_exposure = exposure;
    sr->skybox_rotation = rotation;
}

/* ── Frustum culling (uniform-grid broadphase) ────────────────────── */

/* Extract 6 frustum planes from a column-major view*proj matrix.
 * Convention: plane.xyz = normal, plane.w = signed distance such that
 *             dot(plane.xyz, p) + plane.w >= 0  iff  p is INSIDE the frustum.
 * Works for D3D-style NDC ([0,1] depth) and OpenGL-style ([-1,1]) alike for
 * left/right/top/bottom; near plane uses (m3 + m2) which is correct for
 * GL and conservative (looser) for D3D — fine for broadphase culling. */
static void sr_extract_frustum_planes(const jce_mat4 *m, jce_vec4 planes[6])
{
    /* m->raw[col][row] (column-major); rows of the matrix = m->raw[*][row]. */
    #define RC(col, row) m->raw[(col)][(row)]
    /* Left:  row3 + row0 ; Right: row3 - row0 */
    for (int i = 0; i < 6; i++) {
        const int row  = i / 2;        /* 0..2 */
        const int sign = (i & 1) ? -1 : 1;
        planes[i].x = RC(0, 3) + sign * RC(0, row);
        planes[i].y = RC(1, 3) + sign * RC(1, row);
        planes[i].z = RC(2, 3) + sign * RC(2, row);
        planes[i].w = RC(3, 3) + sign * RC(3, row);
        const float L = sqrtf(planes[i].x * planes[i].x +
                              planes[i].y * planes[i].y +
                              planes[i].z * planes[i].z);
        if (L > 1e-6f) {
            const float inv = 1.0f / L;
            planes[i].x *= inv;
            planes[i].y *= inv;
            planes[i].z *= inv;
            planes[i].w *= inv;
        }
    }
    #undef RC
}

/* Build a transient grid from the entity list and frustum-cull it.
 * Output: visible[i] = true if entity list->entities[i] passes culling.
 * Returns the visible entity count. */
static uint32_t sr_compute_visible(JceSceneRenderer *sr,
                                    JceScene *scene,
                                    const EntityList *list,
                                    const jce_vec4 planes[6],
                                    bool *visible)
{
    /* Grow persistent AABB array if needed. */
    if (sr->cull_aabb_cap < (uint32_t)list->count) {
        uint32_t new_cap = sr->cull_aabb_cap ? sr->cull_aabb_cap * 2u : 64u;
        while (new_cap < (uint32_t)list->count) new_cap *= 2u;
        JceAABB *grown = (JceAABB *)JCE_REALLOC(sr->cull_aabbs,
                                                 new_cap * sizeof(JceAABB));
        if (!grown) {
            for (int i = 0; i < list->count; i++) visible[i] = true;
            return (uint32_t)list->count;
        }
        sr->cull_aabbs    = grown;
        sr->cull_aabb_cap = new_cap;
    }
    JceAABB *aabbs = sr->cull_aabbs;

    /* World bounds derived from a quick scan — we want a snug grid. */
    jce_vec3 wmin = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
    jce_vec3 wmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

    for (int i = 0; i < list->count; i++) {
        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (!sr_build_entity_model(sr, scene, list->entities[i], &model, &mesh)) {
            aabbs[i].min = aabbs[i].max = jce_v3(0, 0, 0);
            visible[i] = true; /* keep entities without transform/model */
            continue;
        }

        /* Local-space AABB. Fall back to a unit cube around the origin
         * if no mesh resolved (terrain / future component types). */
        float lmn[3], lmx[3];
        if (mesh) {
            jce_mesh_get_aabb(mesh, lmn, lmx);
        } else {
            lmn[0] = lmn[1] = lmn[2] = -0.5f;
            lmx[0] = lmx[1] = lmx[2] =  0.5f;
        }

        /* Degenerate AABB safeguard. */
        if (lmx[0] - lmn[0] < 1e-4f && lmx[1] - lmn[1] < 1e-4f &&
            lmx[2] - lmn[2] < 1e-4f) {
            lmn[0] = lmn[1] = lmn[2] = -0.5f;
            lmx[0] = lmx[1] = lmx[2] =  0.5f;
        }

        /* Transform 8 corners by world matrix and refit. */
        const jce_vec3 corners[8] = {
            { lmn[0], lmn[1], lmn[2] }, { lmx[0], lmn[1], lmn[2] },
            { lmn[0], lmx[1], lmn[2] }, { lmx[0], lmx[1], lmn[2] },
            { lmn[0], lmn[1], lmx[2] }, { lmx[0], lmn[1], lmx[2] },
            { lmn[0], lmx[1], lmx[2] }, { lmx[0], lmx[1], lmx[2] },
        };
        jce_vec3 bmn = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
        jce_vec3 bmx = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (int c = 0; c < 8; c++) {
            const jce_vec4 cv = { corners[c].x, corners[c].y, corners[c].z, 1.0f };
            const jce_vec4 wv = jce_m4_mul_v4(&model, cv);
            const float wx = wv.x, wy = wv.y, wz = wv.z;
            if (wx < bmn.x) bmn.x = wx; if (wx > bmx.x) bmx.x = wx;
            if (wy < bmn.y) bmn.y = wy; if (wy > bmx.y) bmx.y = wy;
            if (wz < bmn.z) bmn.z = wz; if (wz > bmx.z) bmx.z = wz;
        }
        aabbs[i].min = bmn;
        aabbs[i].max = bmx;
        if (bmn.x < wmin.x) wmin.x = bmn.x;
        if (bmn.y < wmin.y) wmin.y = bmn.y;
        if (bmn.z < wmin.z) wmin.z = bmn.z;
        if (bmx.x > wmax.x) wmax.x = bmx.x;
        if (bmx.y > wmax.y) wmax.y = bmx.y;
        if (bmx.z > wmax.z) wmax.z = bmx.z;
        visible[i] = false;   /* will be flipped to true by the query below */
    }

    /* Pad to avoid degenerate dimensions. */
    const float pad = 1.0f;
    wmin.x -= pad; wmin.y -= pad; wmin.z -= pad;
    wmax.x += pad; wmax.y += pad; wmax.z += pad;

    JceAABB world = { wmin, wmax };

    if (!sr->cull_space) {
        JceSpaceConfig cfg = { 0 };
        cfg.type             = JCE_SPACE_GRID;
        cfg.world_bounds     = world;
        cfg.max_objects      = (uint32_t)list->count;
        sr->cull_space = jce_space_create(&cfg);
    } else {
        jce_space_reset(sr->cull_space, &world);
    }

    if (!sr->cull_space) {
        for (int i = 0; i < list->count; i++) visible[i] = true;
        return (uint32_t)list->count;
    }

    for (int i = 0; i < list->count; i++) {
        if (visible[i]) continue;     /* transform-less; already kept */
        jce_space_insert(sr->cull_space, aabbs[i], (uint32_t)i);
    }

    uint32_t hit_buf[SR_MAX_ENTITIES];
    const uint32_t hits = jce_space_query_frustum(sr->cull_space, planes,
                                                   hit_buf, SR_MAX_ENTITIES);
    for (uint32_t k = 0; k < hits; k++) {
        if (hit_buf[k] < (uint32_t)list->count) visible[hit_buf[k]] = true;
    }

    return hits;
}

/* ── Baked GI consumption (P1-baked-gi-consume) ───────────────────────
 *
 * Reflection probe cubemaps are loaded once and cached by path; the SH9
 * coefficients live directly in the LightProbeGroup component. Each frame
 * sr_gather_baked_gi() picks the dominant probe / group nearest the camera
 * and the bind callbacks consume them. */

/* Find (or lazily load) a baked reflection-probe cubemap by path. Returns
 * the cache slot index, or -1 on failure. The cache is small and probes
 * are few, so a linear scan is fine. */
static int sr_rprobe_cache_get(JceSceneRenderer *sr, const char *path)
{
    if (!path || !path[0]) return -1;

    for (int i = 0; i < sr->rprobe_cache_count; i++) {
        if (strncmp(sr->rprobe_cache[i].path, path,
                    sizeof(sr->rprobe_cache[i].path)) == 0) {
            return sr->rprobe_cache[i].failed ? -1 : i;
        }
    }
    if (sr->rprobe_cache_count >= (int)(sizeof(sr->rprobe_cache) /
                                        sizeof(sr->rprobe_cache[0])))
        return -1;

    int slot = sr->rprobe_cache_count++;
    snprintf(sr->rprobe_cache[slot].path, sizeof(sr->rprobe_cache[slot].path),
             "%s", path);
    sr->rprobe_cache[slot].spec.idx = UINT16_MAX;
    sr->rprobe_cache[slot].irr.idx  = UINT16_MAX;
    sr->rprobe_cache[slot].used     = true;
    sr->rprobe_cache[slot].failed   = false;

    uint16_t spec = jce__ktx_load_cubemap(path);
    if (spec == UINT16_MAX) {
        sr->rprobe_cache[slot].failed = true;
        LOG_WARN(LOG_TAG, "reflection probe cubemap load failed: %s", path);
        return -1;
    }
    sr->rprobe_cache[slot].spec.idx = spec;
    /* The bake currently emits single-mip KTX cubemaps; a future specular
     * mip-chain bake should plumb the real count here so glossy reflections
     * pick the correct prefilter LOD. */
    sr->rprobe_cache[slot].spec_mips = 1;

    /* Irradiance sidecar: <stem>.irr.ktx (optional — fall back to the
     * specular cube for diffuse when absent). */
    char irr_path[256];
    snprintf(irr_path, sizeof(irr_path), "%s", path);
    char *dot = strrchr(irr_path, '.');
    if (dot && (size_t)(dot - irr_path) + 9u < sizeof(irr_path)) {
        memcpy(dot, ".irr.ktx", 9u); /* includes NUL */
        uint16_t irr = jce__ktx_load_cubemap(irr_path);
        if (irr != UINT16_MAX) sr->rprobe_cache[slot].irr.idx = irr;
    }
    return slot;
}

/* Per-frame: select the dominant baked reflection probe + light-probe SH9
 * group (nearest to the camera) and stash them on sr for the bind cbs. */
static void sr_gather_baked_gi(JceSceneRenderer *sr, JceScene *scene,
                               const JceCamera *camera, EntityList *list)
{
    sr->gi_probe_active = false;
    sr->gi_sh9_active   = false;
    sr->gi_probe_spec.idx = UINT16_MAX;
    sr->gi_probe_irr.idx  = UINT16_MAX;

    jce_vec3 cam = camera ? jce_camera_get_position(camera) : jce_v3(0, 0, 0);

    float best_probe_d2 = FLT_MAX;
    float best_sh9_d2   = FLT_MAX;

    for (int i = 0; i < list->count; i++) {
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;
        JceTransform *xf = jce_scene_get_transform(scene, e);

        /* Reflection probe with a baked cubemap on disk. */
        if (jce_scene_has_reflection_probe(scene, e)) {
            JceReflectionProbeComponent *rp =
                jce_scene_get_reflection_probe(scene, e);
            if (rp && rp->baked_cubemap_path[0]) {
                jce_vec3 p = xf ? xf->position : jce_v3(0, 0, 0);
                p.x += rp->box_offset[0];
                p.y += rp->box_offset[1];
                p.z += rp->box_offset[2];
                jce_vec3 d = jce_v3_sub(p, cam);
                float d2 = jce_v3_dot(d, d);
                if (d2 < best_probe_d2) {
                    int slot = sr_rprobe_cache_get(sr, rp->baked_cubemap_path);
                    if (slot >= 0) {
                        best_probe_d2 = d2;
                        sr->gi_probe_spec = sr->rprobe_cache[slot].spec;
                        sr->gi_probe_irr  = sr->rprobe_cache[slot].irr;
                        sr->gi_probe_spec_mips = sr->rprobe_cache[slot].spec_mips;
                        sr->gi_probe_intensity =
                            rp->intensity > 0.0f ? rp->intensity : 1.0f;
                        sr->gi_probe_active = true;
                    }
                }
            }
        }

        /* Light probe group with baked SH9. */
        if (jce_scene_has_light_probe_group(scene, e)) {
            JceLightProbeGroupComponent *lpg =
                jce_scene_get_light_probe_group(scene, e);
            if (lpg && lpg->sh9_baked && lpg->probe_count > 0) {
                /* Nearest individual probe within the group. */
                jce_vec3 base = xf ? xf->position : jce_v3(0, 0, 0);
                int best_pi = -1;
                float best_pd2 = FLT_MAX;
                for (int pi = 0; pi < lpg->probe_count &&
                                 pi < JCE_LIGHT_PROBE_MAX; pi++) {
                    jce_vec3 p = jce_v3(base.x + lpg->positions[pi][0],
                                        base.y + lpg->positions[pi][1],
                                        base.z + lpg->positions[pi][2]);
                    jce_vec3 d = jce_v3_sub(p, cam);
                    float d2 = jce_v3_dot(d, d);
                    if (d2 < best_pd2) { best_pd2 = d2; best_pi = pi; }
                }
                if (best_pi >= 0 && best_pd2 < best_sh9_d2) {
                    best_sh9_d2 = best_pd2;
                    for (int c = 0; c < 9; c++) {
                        sr->gi_sh9[c][0] = lpg->sh9[best_pi][c][0];
                        sr->gi_sh9[c][1] = lpg->sh9[best_pi][c][1];
                        sr->gi_sh9[c][2] = lpg->sh9[best_pi][c][2];
                    }
                    sr->gi_sh9_active = true;
                }
            }
        }
    }
}

/* Apply baked-GI uniforms + (optional) reflection-probe cubemap override.
 * Shared by the queue and inline bind paths. Must run AFTER the sky-IBL
 * bind so the probe overrides stages 6/7 when present. ibl_params is the
 * 4-float vector the caller is about to upload as u_iblParams; this routine
 * forces IBL on (x=1) when a probe is active so fs_pbr takes the IBL path. */
static void sr_bind_baked_gi(JceSceneRenderer *sr, float ibl_params[4])
{
    /* Reflection probe overrides the sky prefilter / irradiance. */
    if (sr->gi_probe_active && BGFX_HANDLE_IS_VALID(sr->gi_probe_spec) &&
        BGFX_HANDLE_IS_VALID(sr->brdf_lut)) {
        bgfx_texture_handle_t irr = sr->gi_probe_irr;
        if (!BGFX_HANDLE_IS_VALID(irr)) irr = sr->gi_probe_spec;
        bgfx_set_texture(6, sr->u_ibl_irradiance, irr,               UINT32_MAX);
        bgfx_set_texture(7, sr->u_ibl_prefilter,  sr->gi_probe_spec, UINT32_MAX);
        bgfx_set_texture(8, sr->u_ibl_brdf_lut,   sr->brdf_lut,      UINT32_MAX);
        ibl_params[0] = 1.0f;
        if (sr->gi_probe_spec_mips > 1)
            ibl_params[1] = (float)(sr->gi_probe_spec_mips - 1);
    }

    /* SH9 ambient + GI params (x=sh9 enabled, y=reflection-probe intensity). */
    float gi_params[4] = {
        sr->gi_sh9_active ? 1.0f : 0.0f,
        sr->gi_probe_active ? sr->gi_probe_intensity : 1.0f,
        0.0f, 0.0f
    };
    if (BGFX_HANDLE_IS_VALID(sr->u_gi_params))
        bgfx_set_uniform(sr->u_gi_params, gi_params, 1);

    if (BGFX_HANDLE_IS_VALID(sr->u_sh9)) {
        float sh9[9][4];
        for (int c = 0; c < 9; c++) {
            sh9[c][0] = sr->gi_sh9_active ? sr->gi_sh9[c][0] : 0.0f;
            sh9[c][1] = sr->gi_sh9_active ? sr->gi_sh9[c][1] : 0.0f;
            sh9[c][2] = sr->gi_sh9_active ? sr->gi_sh9[c][2] : 0.0f;
            sh9[c][3] = 0.0f;
        }
        bgfx_set_uniform(sr->u_sh9, sh9, 9);
    }
}

/* ── Entity rendering ─────────────────────────────────────────────── */

static void sr_draw_entities(JceSceneRenderer *sr, JceScene *scene,
                             const JceCamera *camera, EntityList *list,
                             uint16_t view_id, float dt_sec,
                             const JceSceneRenderConfig *cfg)
{
    /* Animation is advanced in sr_update_skinned_anims (before the shadow
       pass); the color pass only consumes the cached palette. */
    (void)dt_sec;

    if (list->count == 0) return;

    /* Gather lights. */
    if (sr->light_env) {
        jce_light_env_clear(sr->light_env);
        if (sr->ambient_override_active) {
            jce_light_env_set_ambient(sr->light_env,
                                       sr->ambient_override_color,
                                       sr->ambient_override_intensity);
        } else if (jce_scene_has_rendering_settings(scene)) {
            const JceSceneRenderingSettings *r =
                jce_scene_get_rendering_settings(scene);
            jce_vec3 color = jce_v3(r->ambient_color[0],
                                    r->ambient_color[1],
                                    r->ambient_color[2]);
            jce_light_env_set_ambient(sr->light_env, color,
                                       r->ambient_intensity);
        } else {
            jce_light_env_set_ambient(sr->light_env, jce_v3(1, 1, 1), 0.15f);
        }

        bool has_any_light = false;
        for (int i = 0; i < list->count; i++) {
            JceEntity e = list->entities[i];
            if (!entity_enabled(scene, e)) continue;
            JceTransform *xf = jce_scene_get_transform(scene, e);

            if (jce_scene_has_dir_light(scene, e) &&
                jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_DIR_LIGHT)) {
                JceDirectionalLight *dlc = jce_scene_get_dir_light(scene, e);
                if (dlc) {
                    JceDirLightDesc dl;
                    memset(&dl, 0, sizeof(dl));
                    dl.color = dlc->color;
                    dl.intensity = dlc->intensity > 0.0f ? dlc->intensity : 1.0f; dl.casts_shadow = dlc->casts_shadow;
                    dl.direction =
                        sr_light_world_shine_direction(&dlc->direction, xf);
                    /* P3-E.5 — propagate optional cookie. */
                    dl.cookie_texture  = dlc->cookie_texture;
                    dl.cookie_strength = dlc->cookie_strength;
                    jce_light_env_add_dir_light(sr->light_env, &dl);
                    has_any_light = true;
                }
            }
            if (jce_scene_has_point_light(scene, e) &&
                jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_POINT_LIGHT)) {
                JcePointLight *plc = jce_scene_get_point_light(scene, e);
                if (plc) {
                    JcePointLightDesc pl;
                    memset(&pl, 0, sizeof(pl));
                    pl.color = plc->color;
                    pl.intensity = plc->intensity > 0.0f ? plc->intensity : 1.0f;
                    pl.radius    = plc->radius    > 0.0f ? plc->radius    : 10.0f;
                    pl.position  = xf ? xf->position : plc->position;
                    pl.casts_shadow = plc->casts_shadow;
                    pl.shadow_bias  = plc->shadow_bias;
                    jce_light_env_add_point_light(sr->light_env, &pl);
                    has_any_light = true;
                }
            }
            if (jce_scene_has_spot_light(scene, e) &&
                jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SPOT_LIGHT)) {
                JceSpotLight *slc = jce_scene_get_spot_light(scene, e);
                if (slc) {
                    JceSpotLightDesc sl;
                    memset(&sl, 0, sizeof(sl));
                    sl.color = slc->color;
                    sl.intensity = slc->intensity > 0.0f ? slc->intensity : 1.0f;
                    sl.radius    = slc->radius    > 0.0f ? slc->radius    : 10.0f;
                    sl.inner_cone_cos = slc->inner_cone_cos;
                    sl.outer_cone_cos = slc->outer_cone_cos;
                    sl.position  = xf ? xf->position : slc->position;
                    sl.direction =
                        sr_light_world_shine_direction(&slc->direction, xf);
                    /* P3-E.5 — propagate cookie + IES profile bindings. */
                    sl.cookie_texture  = slc->cookie_texture;
                    sl.ies_lut_texture = slc->ies_lut_texture;
                    sl.cookie_strength = slc->cookie_strength;
                    sl.casts_shadow = slc->casts_shadow;
                    sl.shadow_bias  = slc->shadow_bias;
                    jce_light_env_add_spot_light(sr->light_env, &sl);
                    has_any_light = true;
                }
            }
        }

        if (!has_any_light && sr->tod_active) {
            JceDirLightDesc dl;
            memset(&dl, 0, sizeof(dl));
            dl.direction = jce_v3_scale(sr->tod_state.sun_direction, -1.0f);
            dl.color     = sr->tod_state.sun_color;
            dl.intensity = 1.0f; dl.casts_shadow = true;
            jce_light_env_add_dir_light(sr->light_env, &dl);
        }

        if (sr->tod_active)
            jce_light_env_set_ambient(sr->light_env, sr->tod_state.ambient_color, 1.0f);

        if (camera) {
            jce_vec3 cp = jce_camera_get_position(camera);
            jce_light_env_set_camera_pos(sr->light_env, cp);
        }
        jce_light_env_apply(sr->light_env, sr->renderer);
    }

    /* Baked GI: pick the dominant reflection probe + light-probe SH9 for
     * this frame; the per-material bind cbs consume sr->gi_*. */
    sr_gather_baked_gi(sr, scene, camera, list);

    jce_vec3 legacy_dir, legacy_color;
    float legacy_intensity = 1.0f;
    bool legacy_has_dir = sr_resolve_primary_dir_light(sr, scene, list, false,
        &legacy_dir, &legacy_color, &legacy_intensity);
    if (legacy_has_dir) {
        JceDirLight sun = jce_dir_light_default();
        sun.direction = legacy_dir;
        sun.color = jce_v3_scale(legacy_color, legacy_intensity);
        jce_lighting_apply(sr->renderer, &sun);
    } else {
        float raw_dir[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
        float raw_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        bgfx_set_uniform(sr->u_light_dir, raw_dir, 1);
        bgfx_set_uniform(sr->u_light_color, raw_color, 1);
    }

    /* WIREFRAME_TEXTURED debug view: trigger fs_mesh.sc hue-Lambert branch by
     * setting u_lightDir.w = 0.25 (matches 0.5.7 behaviour). Done after
     * jce_lighting_apply since that overwrites these uniforms. */
    if (cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME_TEXTURED &&
        sr->has_cbs && sr->cbs.load_texture)
    {
        jce_vec3 sd = legacy_has_dir ? legacy_dir : jce_v3(0.0f, 1.0f, 0.0f);
        float len = sqrtf(sd.x * sd.x + sd.y * sd.y + sd.z * sd.z);
        if (len > 1e-6f) { sd.x /= len; sd.y /= len; sd.z /= len; }
        float wf_dir[4]   = { sd.x, sd.y, sd.z, 0.25f };
        float wf_color[4] = { 1.0f, 1.0f, 1.0f, 0.2f };
        bgfx_set_uniform(sr->u_light_dir,   wf_dir,   1);
        bgfx_set_uniform(sr->u_light_color, wf_color, 1);
    }

    /* Begin sprite batch. */
    if (sr->sprite_batch && cfg->draw_sprites)
        jce_sprite_batch_begin(sr->sprite_batch);

    /* Optional broadphase frustum culling.  When disabled, every entity
     * is treated as visible (matches legacy behaviour). */
    bool visible_buf[SR_MAX_ENTITIES];
    bool *visible = visible_buf;
    sr->stat_total_entities  = (uint32_t)list->count;
    sr->stat_culling_enabled = cfg->frustum_culling;
    if (cfg->frustum_culling && camera && list->count > 0) {
        const jce_mat4 v = jce_camera_view(camera);
        const jce_mat4 p = jce_camera_proj(camera, 16.0f / 9.0f,
                                            sr->homogeneous_depth);
        const jce_mat4 vp = jce_m4_multiply(&p, &v);
        jce_vec4 planes[6];
        sr_extract_frustum_planes(&vp, planes);
        const uint32_t kept = sr_compute_visible(sr, scene, list, planes, visible);
        sr->stat_visible_entities = kept;
        sr->stat_culled_entities  = (uint32_t)list->count - kept;
    } else {
        for (int i = 0; i < list->count; i++) visible[i] = true;
        sr->stat_visible_entities = (uint32_t)list->count;
        sr->stat_culled_entities  = 0;
    }

    /* Reset per-frame LOD pick counters. */
    sr->stat_lod_enabled = (sr->global_lod && sr->global_lod->count > 0);
    for (int li = 0; li < JCE_LOD_MAX_LEVELS; li++) sr->stat_lod_picks[li] = 0;
    sr->stat_lod_culled = 0;

    /* Reset per-frame terrain chunk counters. */
    sr->stat_terrain_chunks_total  = 0;
    sr->stat_terrain_chunks_drawn  = 0;
    sr->stat_terrain_chunks_culled = 0;

    /* Reset per-frame render-queue stats (accumulated across all flushes). */
    memset(&sr->stat_rq, 0, sizeof(sr->stat_rq));
    sr->stat_rq_active = false;

    /* Begin occlusion culler frame (reads last-frame query results).
     * We compute view/proj here regardless of frustum culling so the
     * proxy depth-only pass sees the same camera as the main scene. */
    sr->stat_occlusion_enabled = (cfg->occlusion_culler != NULL);
    memset(&sr->stat_occlusion, 0, sizeof(sr->stat_occlusion));
    if (cfg->occlusion_culler && camera) {
        const jce_mat4 oc_v  = jce_camera_view(camera);
        const jce_mat4 oc_p  = jce_camera_proj(camera, 16.0f / 9.0f,
                                                sr->homogeneous_depth);
        jce_occlusion_culler_begin_frame(cfg->occlusion_culler,
                                          JCE_M4_PTR(oc_v), JCE_M4_PTR(oc_p));
        /* Set the view transform for the proxy pre-pass view (id 254) so
         * the depth-only proxy draws are in the correct camera space. */
        bgfx_set_view_transform(254, JCE_M4_PTR(oc_v), JCE_M4_PTR(oc_p));
    } else if (cfg->occlusion_culler) {
        jce_occlusion_culler_begin_frame(cfg->occlusion_culler, NULL, NULL);
    }
    jce_vec3 lod_cam_pos = (jce_vec3){0, 0, 0};
    if (sr->stat_lod_enabled && camera) lod_cam_pos = jce_camera_get_position(camera);

    /* ── Render-queue setup (Phase 3) ─────────────────────────────────
     * If the instanced PBR program is available we route non-terrain
     * mesh entities through the queue → auto-batched instanced submits.
     * Terrain, wireframe, sprite, skinned paths still submit inline. */
    JceShaderHandle prog_pbr_inst_h = jce_renderer_get_program_pbr_inst(sr->renderer);
    JceShaderHandle prog_pbr_h      = jce_renderer_get_program_pbr(sr->renderer);
    /* Queue path is on by default. Earlier PSO-compile glitches with
     * vs_pbr_inst on a few D3D12 setups have been resolved (see Wave 6/7);
     * leave an opt-out via JCE_USE_RQ=0 for forensic comparison. */
    static int s_use_rq_env = -1;
    if (s_use_rq_env < 0) {
        const char *v = getenv("JCE_USE_RQ");
        s_use_rq_env = (v && v[0] == '0') ? 0 : 1;
    }
    bool use_rq = s_use_rq_env && sr->render_queue && prog_pbr_inst_h.idx != UINT16_MAX;
    if (use_rq) {
        sr_reset_material_cache(sr);
        jce_rq_clear(sr->render_queue);
        if (sr->transparent_queue) jce_rq_clear(sr->transparent_queue);
        sr->frame_view_id = view_id;
        sr->frame_scene = scene;
    }

    /* Camera view matrix — used to compute real view-space depth so the
     * transparent queue can sort back-to-front (farthest drawn first). */
    const jce_mat4 cam_view_mat = camera ? jce_camera_view(camera)
                                         : jce_m4_identity();

    for (int i = 0; i < list->count; i++) {
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;

        /* ── Terrain path (P1-terrain-lod) ───────────────────────────
         * Terrain is drawn chunk-by-chunk with its OWN per-chunk frustum
         * culling + distance LOD, so it bypasses the entity-level cull
         * (whose position+scale AABB is meaningless for a large heightmap
         * whose origin may sit off-screen).  Preserves the legacy priority:
         * a resolvable MeshRenderer mesh still wins over the terrain fallback. */
        if (cfg->draw_opaque && jce_scene_has_terrain(scene, e) &&
            jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_TERRAIN) &&
            sr_resolve_mesh(sr, jce_scene_has_mesh_renderer(scene, e)
                                ? jce_scene_get_mesh_renderer(scene, e)
                                : NULL) == NULL) {
            JceTerrainComponent *tc = jce_scene_get_terrain(scene, e);
            if (tc && tc->visible && tc->terrain_path[0] &&
                jce_scene_has_transform(scene, e)) {
                int tslot = sr_terrain_find_or_load_slot(sr, tc->terrain_path);
                if (tslot >= 0) {
                    jce_mat4 tmodel = jce_scene_get_world_matrix(scene, e);
                    JcePbrMaterial tpbr = jce_pbr_material_default();
                    bgfx_texture_handle_t tlayers[4];
                    for (int li = 0; li < 4; li++) tlayers[li] = sr->white_tex;
                    for (int li = 0; li < 4; li++) {
                        if (!tc->layer_albedo_path[li][0]) continue;
                        JceTexture lt =
                            sr_resolve_texture(sr, tc->layer_albedo_path[li]);
                        if (jce_texture_valid(lt)) tlayers[li].idx = lt.idx;
                    }
                    sr_draw_terrain_chunks(sr, scene, list, camera, view_id,
                                           tslot, tc, &tmodel, &tpbr, tlayers);
                }
            }
            continue;  /* terrain fully handled (or skipped) */
        }

        if (!visible[i]) continue;

        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (!sr_build_entity_model(sr, scene, e, &model, &mesh)) continue;

        /* ── Occlusion culling (GPU-query, two-pass coherence) ───────
         * Derive a world-space radius from the entity scale (uniform).
         * Skip the draw if the entity was fully occluded last frame and
         * submit a depth-only proxy instead so the culler can update. */
        float occlusion_radius = 0.5f;
        bool occlusion_skip = false;
        if (cfg->occlusion_culler) {
            float sx = model.col[0].x, sy = model.col[1].y, sz = model.col[2].z;
            float sc = sx > sy ? (sx > sz ? sx : sz) : (sy > sz ? sy : sz);
            if (sc < 0.001f) sc = 0.001f;
            occlusion_radius = sc * 0.5f;

            jce_vec3 world_pos = { model.col[3].x, model.col[3].y, model.col[3].z };
            if (!jce_occlusion_culler_entity_visible(cfg->occlusion_culler,
                                                     (uint64_t)e, world_pos,
                                                     occlusion_radius)) {
                occlusion_skip = true;
                /* Still submit a proxy query so next frame is accurate. */
                jce_occlusion_culler_submit_query(cfg->occlusion_culler,
                                                  (uint64_t)e, world_pos,
                                                  occlusion_radius);
                continue;
            }
        }

        /* ── Global LOD substitution (MVP) ───────────────────────── */
        if (sr->stat_lod_enabled) {
            const float dx = model.col[3].x - lod_cam_pos.x;
            const float dy = model.col[3].y - lod_cam_pos.y;
            const float dz = model.col[3].z - lod_cam_pos.z;
            const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            const uint32_t slot = (uint32_t)e & 1023u;
            const int prev = (int)sr->lod_prev[slot];
            const int lvl  = jce_lod_pick(sr->global_lod, dist,
                                          prev > 0 ? prev - 1 : -1);
            if (lvl < 0) {
                sr->stat_lod_culled++;
                /* Park at last level so a return swing snaps cleanly. */
                sr->lod_prev[slot] = (int8_t)(sr->global_lod->count);
                continue;
            }
            sr->lod_prev[slot] = (int8_t)(lvl + 1);  /* 0 == "no record" */
            sr->stat_lod_picks[lvl]++;
            JceMesh *lod_mesh = sr->global_lod->levels[lvl].mesh;
            if (lod_mesh) mesh = lod_mesh;
        }

        /* ── Skinned/animated path ───────────────────────────────── */
        /* Pose was already advanced ONCE this frame by
           sr_update_skinned_anims (before the shadow pass); here we only
           consume the cached palette so the lit mesh and its cast shadow
           share the exact same pose.  skin_palette_count == 0 => bind pose. */
        if (jce_scene_has_skeletal_animator(scene, e)) {
            JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
            if (sa && sa->skeleton_path[0]) {
                SrModelCache *mc = sr_get_model(sr, sa->skeleton_path, (uint32_t)e);
                if (mc && mc->model) {
                    SrAnimInstance *ai = sr_find_anim_instance(sr, (uint32_t)e);
                    const jce_mat4 *pal =
                        (ai && ai->skin_palette_count > 0) ? ai->skin_palette : NULL;
                    uint32_t pal_n = ai ? ai->skin_palette_count : 0;
                    jce_model_draw(mc->model, sr->renderer, view_id, &model,
                                   pal, pal_n);
                    continue;
                }
            }
        }

        if (cfg->draw_opaque &&
            !jce_scene_has_skeletal_animator(scene, e) &&
            sr_try_draw_mesh_renderer_model(sr, scene, e, view_id, &model)) {
            continue;
        }

        /* ── Sprite animator path (2D, P1 #16) ───────────────────── */
        /* Frame time was advanced once this frame by sr_update_sprite_anims;
           here we consume the cached player's current frame, convert its pixel
           rect to a UV sub-rect, and submit the quad to the sprite batch. */
        if (cfg->draw_sprites && sr->sprite_batch &&
            jce_scene_has_sprite_animator(scene, e) &&
            jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SPRITE_ANIMATOR)) {
            int slot = sr_find_sprite_anim(sr, (uint32_t)e);
            JceSpritePlayer *pl =
                (slot >= 0) ? sr->sprite_anim[slot].player : NULL;
            const JceSpriteSheet *sheet =
                (slot >= 0) ? sr->sprite_anim[slot].sheet : NULL;
            const JceSpriteFrame *fr =
                pl ? jce_sprite_player_current_frame(pl) : NULL;
            if (fr) {
                JceSpriteAnimatorComponent *sac =
                    jce_scene_get_sprite_animator(scene, e);
                /* Resolve the sheet image: the component's explicit image path
                   wins; fall back to the atlas's meta.image. */
                const char *img = (sac && sac->sheet_path[0])
                    ? sac->sheet_path
                    : (sheet ? jce_sprite_sheet_image_path(sheet) : NULL);
                bgfx_texture_handle_t st = { UINT16_MAX };
                uint32_t iw = 0, ih = 0;
                if (img && img[0]) {
                    JceTexture t = sr_resolve_texture(sr, img);
                    if (jce_texture_valid(t)) {
                        st.idx = t.idx;
                        jce_texture_get_size(t, &iw, &ih);
                    }
                }
                if (!BGFX_HANDLE_IS_VALID(st)) { st = sr->white_tex; iw = ih = 1; }

                float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
                if (iw > 0 && ih > 0 && fr->w > 0 && fr->h > 0) {
                    u0 = (float)fr->x / (float)iw;
                    v0 = (float)fr->y / (float)ih;
                    u1 = (float)(fr->x + fr->w) / (float)iw;
                    v1 = (float)(fr->y + fr->h) / (float)ih;
                }

                /* SpriteAnimator has no tint/sort fields: white tint, sort 0. */
                JceTexture sjt; sjt.idx = st.idx;
                jce_sprite_batch_add(sr->sprite_batch, sjt, model.raw[0],
                                     u0, v0, u1, v1, 0xFFFFFFFFu, 0);
                continue;
            }
        }

        /* ── Sprite path ─────────────────────────────────────────── */
        if (cfg->draw_sprites && sr->sprite_batch &&
            jce_scene_has_sprite_renderer(scene, e) &&
            jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SPRITE_RENDERER)) {
            JceSpriteRendererComponent *spr = jce_scene_get_sprite_renderer(scene, e);
            if (spr) {
                bgfx_texture_handle_t st = { UINT16_MAX };
                if (spr->sprite_path[0]) {
                    JceTexture t = sr_resolve_texture(sr, spr->sprite_path);
                    if (jce_texture_valid(t)) st.idx = t.idx;
                }
                if (!BGFX_HANDLE_IS_VALID(st)) st = sr->white_tex;

                float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
                if (spr->flip_x) { float tmp = u0; u0 = u1; u1 = tmp; }
                if (spr->flip_y) { float tmp = v0; v0 = v1; v1 = tmp; }

                const float *sc = spr->color;
                uint8_t r8 = (uint8_t)(sc[0] * 255.0f);
                uint8_t g8 = (uint8_t)(sc[1] * 255.0f);
                uint8_t b8 = (uint8_t)(sc[2] * 255.0f);
                uint8_t a8 = (uint8_t)(sc[3] * 255.0f);
                uint32_t abgr = ((uint32_t)a8 << 24) | ((uint32_t)b8 << 16)
                              | ((uint32_t)g8 << 8) | (uint32_t)r8;

                JceTexture sjt; sjt.idx = st.idx;
                jce_sprite_batch_add(sr->sprite_batch, sjt, model.raw[0],
                                     u0, v0, u1, v1, abgr, spr->sorting_order);
                continue;
            }
        }

        if (!mesh || !cfg->draw_opaque) continue;
        bgfx_set_transform(model.raw[0], 1);

        /* ── PBR mesh path ───────────────────────────────────────── */
        JceMeshRenderer *mr_comp = jce_scene_has_mesh_renderer(scene, e)
            ? jce_scene_get_mesh_renderer(scene, e) : NULL;

        /* ── View-mode dispatch (mirrors the 0.5.7 editor exactly):
         *      SHADED                : PBR factors (no textures), full lighting
         *      TEXTURED              : PBR + all textures, checker fallback
         *      WIREFRAME             : simple-mesh shader, white tint, flat Lambert
         *      WIREFRAME_TEXTURED    : simple-mesh shader, albedo tint, hue-Lambert
         *
         * Only the editor (callback mode) uses these debug modes; runtime
         * always renders PBR with whatever textures the asset specifies. */
        bool editor_mode = (sr->has_cbs && sr->cbs.load_texture);
        bool wireframe_path = editor_mode &&
            (cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME ||
             cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME_TEXTURED);

        if (!wireframe_path) {
            JcePbrMaterial pbr = jce_pbr_material_default();
            bool use_checker_fallback = false;

            /* If this entity has no MeshRenderer component, fall back to a
             * neutral default material (mid-gray, fully rough, dielectric)
             * so the mesh still goes through the PBR shader and therefore
             * receives shadows / IBL exactly like fully-described entities.
             * Earlier this branch went through the legacy fs_mesh.sc which
             * has no shadow sampler, producing the well-known "object only
             * gets darker but never receives external shadow" symptom. */
            if (!mr_comp) {
                pbr.base_color_factor[0] = 0.7f;
                pbr.base_color_factor[1] = 0.7f;
                pbr.base_color_factor[2] = 0.7f;
                pbr.base_color_factor[3] = 1.0f;
                pbr.metallic_factor = 0.0f;
                pbr.roughness_factor = 0.85f;
                pbr.normal_scale = 1.0f;
                pbr.ao_strength = 1.0f;

                /* In editor TEXTURED mode the legacy path used a magenta
                 * checker to highlight missing assets; preserve that hint. */
                if (editor_mode && cfg->view_mode == JCE_SCENE_VIEW_TEXTURED)
                    use_checker_fallback = true;
            } else if (mr_comp->base_color[3] > 0.0f) {
                pbr.base_color_factor[0] = mr_comp->base_color[0];
                pbr.base_color_factor[1] = mr_comp->base_color[1];
                pbr.base_color_factor[2] = mr_comp->base_color[2];
                pbr.base_color_factor[3] = mr_comp->base_color[3];
            }
            if (mr_comp) {
                pbr.metallic_factor = mr_comp->metallic;
                pbr.roughness_factor = mr_comp->roughness;
                pbr.emissive_factor[0] = mr_comp->emissive[0];
                pbr.emissive_factor[1] = mr_comp->emissive[1];
                pbr.emissive_factor[2] = mr_comp->emissive[2];
                pbr.normal_scale = mr_comp->normal_scale;
                pbr.ao_strength = mr_comp->ao_strength;
                pbr.alpha_mode = (JceAlphaMode)mr_comp->alpha_mode;
                pbr.alpha_cutoff = mr_comp->alpha_cutoff;
                pbr.double_sided = mr_comp->double_sided;
            }

            /* Always load all texture maps in editor + runtime. The
             * previous "factors-only in SHADED" optimisation made every
             * baseColor=[1,1,1,1] material render full white because
             * jce_pbr_material_bind() falls back to s_white_tex when no
             * albedo handle is bound. We now keep textures loading in
             * SHADED too, and any missing/in-flight albedo triggers the
             * pink-black checker shader fallback below — never a flat
             * white "loading" surface. */
            bool load_tex = true;

            if (load_tex && mr_comp) {
                if (mr_comp->albedo_tex[0]) {
                    JceTexture t = sr_resolve_texture(sr, mr_comp->albedo_tex);
                    if (jce_texture_valid(t)) pbr.albedo_map = t;
                }
                /* 0.5.5-compatible fallback: derive the texture from the
                 * material file (.mat.json's albedoMap) or the mesh's
                 * sidecar (.obj's MTL map_Kd). Both paths go into ONE
                 * cache entry to avoid duplicate async filesystem walks. */
                if (!jce_texture_valid(pbr.albedo_map) &&
                    (mr_comp->material_path[0] || mr_comp->mesh_path[0])) {
                    JceTexture t = sr_resolve_texture2(sr,
                        mr_comp->material_path[0] ? mr_comp->material_path : NULL,
                        mr_comp->mesh_path[0]     ? mr_comp->mesh_path     : NULL);
                    if (jce_texture_valid(t)) pbr.albedo_map = t;
                }
                /* Editor + missing albedo (in-flight OR failed) → pink-black
                 * checker shader fallback. Applies in BOTH SHADED and
                 * TEXTURED so the editor never displays the white loading
                 * texture while the async loader is resolving the asset. */
                if (editor_mode &&
                    (cfg->view_mode == JCE_SCENE_VIEW_SHADED ||
                     cfg->view_mode == JCE_SCENE_VIEW_TEXTURED) &&
                    !jce_texture_valid(pbr.albedo_map)) {
                    use_checker_fallback = true;
                }
                if (mr_comp->mr_tex[0]) {
                    JceTexture t = sr_resolve_texture(sr, mr_comp->mr_tex);
                    if (jce_texture_valid(t)) pbr.metallic_roughness_map = t;
                }
                if (mr_comp->normal_tex[0]) {
                    JceTexture t = sr_resolve_texture(sr, mr_comp->normal_tex);
                    if (jce_texture_valid(t)) pbr.normal_map = t;
                }
                if (mr_comp->ao_tex[0]) {
                    JceTexture t = sr_resolve_texture(sr, mr_comp->ao_tex);
                    if (jce_texture_valid(t)) pbr.ao_map = t;
                }
                if (mr_comp->emissive_tex[0]) {
                    JceTexture t = sr_resolve_texture(sr, mr_comp->emissive_tex);
                    if (jce_texture_valid(t)) pbr.emissive_map = t;
                }
            }

            /* VideoPlayer-as-texture: a live decoded frame (uploaded by
             * jce_scene_video_update) overrides the entity's albedo so a
             * mesh plays the clip.  Drives both editor preview and runtime. */
            if (jce_scene_has_video_player(scene, e)) {
                JceVideoPlayerComponent *vp =
                    jce_scene_get_video_player(scene, e);
                if (vp && jce_texture_valid(vp->output_tex)) {
                    pbr.albedo_map = vp->output_tex;
                    use_checker_fallback = false;
                }
            }

            /* Encode the checker-fallback flag in normal_scale sign (the
             * fs_pbr.sc shader checks `u_normalScale.x < 0`). */
            if (use_checker_fallback)
                pbr.normal_scale = -fmaxf(fabsf(pbr.normal_scale), 0.0001f);
            else
                pbr.normal_scale = fabsf(pbr.normal_scale);

            /* Terrain is handled earlier by the dedicated per-chunk path
             * (sr_draw_terrain_chunks); a terrain entity never reaches here. */

            /* Material render state: blend (alpha BLEND), depth-write, and
             * back-face cull (double_sided drops CULL_CW).  Shared by the
             * queue and inline paths so both render transparency / two-
             * sided geometry identically. */
            const uint64_t mat_state = jce_pbr_material_render_state(&pbr);
            const bool is_transparent = jce_pbr_material_is_transparent(&pbr);

            /* Shader Graph custom shader: a .mat.json may persist a graph-
             * generated program (customProgramVs/Fs).  When present, route
             * the mesh through it instead of the default PBR program.  Custom
             * programs are not instanced, so we force single-submit. */
            JceShaderHandle custom_prog = mr_comp
                ? sr_resolve_custom_program(sr, mr_comp->material_path)
                : (JceShaderHandle){ UINT16_MAX };
            const bool has_custom = (custom_prog.idx != UINT16_MAX);

            /* ── Queue path: mesh entities go through the render queue →
             *     auto-batched instanced submit. Alpha-blend materials route
             *     to the back-to-front transparent queue. */
            if (use_rq) {
                uint32_t mat_key = sr_compute_material_key(&pbr, false, -1, NULL);
                uint32_t reg_key = sr_register_material(sr, mat_key, &pbr,
                                                        false, -1, 0.0f, false, NULL);
                JceRenderQueue *target_q = is_transparent
                    ? sr->transparent_queue : sr->render_queue;
                if (reg_key && target_q) {
                    JceDrawCmd cmd;
                    memset(&cmd, 0, sizeof(cmd));
                    cmd.view_id      = view_id;
                    if (has_custom) {
                        /* Single-submit only: no instance variant for graph
                         * shaders.  program_single drives the n=1 path. */
                        cmd.program        = custom_prog.idx;
                        cmd.program_single = custom_prog.idx;
                    } else {
                        cmd.program        = (uint16_t)prog_pbr_inst_h.idx;
                        cmd.program_single = (prog_pbr_h.idx != UINT16_MAX)
                                                ? (uint16_t)prog_pbr_h.idx
                                                : (uint16_t)UINT16_MAX;
                    }
                    cmd.mesh_vbh     = jce_mesh_get_vbh(mesh);
                    cmd.mesh_ibh     = jce_mesh_get_ibh(mesh);
                    cmd.index_count  = jce_mesh_index_count(mesh);
                    cmd.transform    = model;
                    /* Real view-space depth for transparent sorting: distance
                     * in front of the camera (larger = farther = drawn first
                     * in back-to-front order).  Opaque draws ignore this. */
                    if (is_transparent) {
                        jce_vec4 wp = jce_v4(model.col[3].x, model.col[3].y,
                                             model.col[3].z, 1.0f);
                        jce_vec4 vp = jce_m4_mul_v4(&cam_view_mat, wp);
                        cmd.depth    = -vp.z;
                    } else {
                        cmd.depth    = 0.0f;
                    }
                    cmd.material_key = reg_key;
                    cmd.state        = mat_state;
                    jce_rq_push(target_q, &cmd);
                    continue;  /* handled by queue flush below */
                }
                /* Registry overflow → fall through to inline path below. */
            }

            /* ── Inline path (queue overflow or queue disabled) ── */
            sr_inline_bind_pbr_global(sr, &pbr, view_id, scene, list);
            if (has_custom)
                jce_mesh_submit_pbr_with_program(mesh, sr->renderer,
                                                 view_id, custom_prog);
            else
                jce_mesh_submit_pbr_state(mesh, sr->renderer, view_id, mat_state);
        } else {
            /* Simple mesh shader path. Used for:
             *   - WIREFRAME / WIREFRAME_TEXTURED (editor debug views)
             *   - entities without a MeshRenderer component (white blob) */
            bgfx_texture_handle_t bind_tex = sr->white_tex;

            if (cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME_TEXTURED && mr_comp)
            {
                JceTexture t = { UINT16_MAX };
                if (mr_comp->albedo_tex[0])
                    t = sr_resolve_texture(sr, mr_comp->albedo_tex);
                if (!jce_texture_valid(t) &&
                    (mr_comp->material_path[0] || mr_comp->mesh_path[0]))
                    t = sr_resolve_texture2(sr,
                        mr_comp->material_path[0] ? mr_comp->material_path : NULL,
                        mr_comp->mesh_path[0]     ? mr_comp->mesh_path     : NULL);
                if (jce_texture_valid(t)) bind_tex.idx = t.idx;
            }

            /* TEXTURED mode + entity with no MR component (or no albedo path):
             * fall back to the magenta/yellow checker so missing assets are
             * visually obvious. */
            if (editor_mode && cfg->view_mode == JCE_SCENE_VIEW_TEXTURED &&
                !mr_comp && BGFX_HANDLE_IS_VALID(sr->checker_tex))
            {
                bind_tex = sr->checker_tex;
            }

            JceUniformHandle uh = jce_renderer_get_tex_uniform(sr->renderer);
            bgfx_uniform_handle_t su = { uh.idx };
            bgfx_set_texture(0, su, bind_tex, UINT32_MAX);
            jce_mesh_submit(mesh, sr->renderer, view_id);
        }

        /* Submit occlusion proxy query for this visible entity so the
         * culler can determine visibility for the NEXT frame. */
        if (cfg->occlusion_culler && !occlusion_skip) {
            jce_vec3 world_pos = { model.col[3].x, model.col[3].y, model.col[3].z };
            jce_occlusion_culler_submit_query(cfg->occlusion_culler,
                                              (uint64_t)e, world_pos,
                                              occlusion_radius);
        }
    }

    /* Collect occlusion stats from the culler after entity loop. */
    if (cfg->occlusion_culler)
        sr->stat_occlusion = jce_occlusion_culler_get_stats(cfg->occlusion_culler);

    /* ── Render-queue flush (Phase 3) ─────────────────────────────────
     * Issues auto-batched instanced submits for all PBR mesh entities
     * pushed during the loop. The binder rebinds material textures /
     * uniforms once per material run. */
    if (use_rq && jce_rq_count(sr->render_queue) > 0) {
        jce_rq_set_material_binder(sr->render_queue, sr_bind_material_cb, sr);
        jce_rq_sort(sr->render_queue, JCE_SORT_FOR_INSTANCING);
        sr_rq_flush_and_collect(sr);
    }

    /* ── Transparent queue flush ───────────────────────────────────────
     * Alpha-blended materials, sorted back-to-front by real view-space
     * depth, flushed AFTER the opaque pass so they composite over solid
     * geometry in correct order.  The view is in SEQUENTIAL submit mode,
     * so this submission order is preserved verbatim by bgfx.  Batching is
     * disabled on this queue (see jce_rq_set_no_batch in init) so per-draw
     * blend order stays exact. */
    if (use_rq && sr->transparent_queue &&
        jce_rq_count(sr->transparent_queue) > 0) {
        jce_rq_set_material_binder(sr->transparent_queue,
                                   sr_bind_material_cb, sr);
        jce_rq_sort(sr->transparent_queue, JCE_SORT_BACK_TO_FRONT);
        sr_rq_flush_queue_and_collect(sr, sr->transparent_queue);
    }

    /* Flush sprite batch. */
    if (sr->sprite_batch && cfg->draw_sprites &&
        jce_sprite_batch_count(sr->sprite_batch) > 0)
        jce_sprite_batch_flush(sr->sprite_batch, sr->renderer, view_id);
}

/* ── Public API ───────────────────────────────────────────────────── */

/* ── Material registry (Phase 2) ──────────────────────────────────────
 * Per-frame registry mapping a material_key → texture/uniform snapshot.
 * Built during scene_renderer's mesh walk, consumed by sr_bind_material_cb
 * once jce_render_queue starts a new material run. Wired into the queue
 * in Phase 3. */

static uint32_t sr_fnv1a_step(uint32_t h, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint32_t sr_compute_material_key(const JcePbrMaterial *pbr,
                                        bool is_terrain,
                                        int terrain_slot,
                                        const bgfx_texture_handle_t *terrain_layer_tex)
{
    uint32_t h = 2166136261u;
    /* Texture handles (idx is enough — invalid = UINT16_MAX). */
    h = sr_fnv1a_step(h, &pbr->albedo_map.idx,             sizeof(uint16_t));
    h = sr_fnv1a_step(h, &pbr->metallic_roughness_map.idx, sizeof(uint16_t));
    h = sr_fnv1a_step(h, &pbr->normal_map.idx,             sizeof(uint16_t));
    h = sr_fnv1a_step(h, &pbr->ao_map.idx,                 sizeof(uint16_t));
    h = sr_fnv1a_step(h, &pbr->emissive_map.idx,           sizeof(uint16_t));
    /* Factors. */
    h = sr_fnv1a_step(h, pbr->base_color_factor, sizeof(pbr->base_color_factor));
    h = sr_fnv1a_step(h, &pbr->metallic_factor,  sizeof(float));
    h = sr_fnv1a_step(h, &pbr->roughness_factor, sizeof(float));
    h = sr_fnv1a_step(h, pbr->emissive_factor,   sizeof(pbr->emissive_factor));
    h = sr_fnv1a_step(h, &pbr->normal_scale,     sizeof(float));
    h = sr_fnv1a_step(h, &pbr->ao_strength,      sizeof(float));
    /* State. */
    uint32_t am = (uint32_t)pbr->alpha_mode;
    h = sr_fnv1a_step(h, &am,                  sizeof(am));
    h = sr_fnv1a_step(h, &pbr->alpha_cutoff,   sizeof(float));
    uint8_t ds = pbr->double_sided ? 1u : 0u;
    h = sr_fnv1a_step(h, &ds,                  sizeof(ds));
    /* Terrain pseudo-fields (slot ensures distinct splat/layer textures). */
    uint8_t it = is_terrain ? 1u : 0u;
    h = sr_fnv1a_step(h, &it, sizeof(it));
    int32_t ts = (int32_t)terrain_slot;
    h = sr_fnv1a_step(h, &ts, sizeof(ts));
    /* Terrain layer texture handles also folded in so two terrain entities
     * with the same slot but different runtime layer textures still split
     * into separate batches (rare today, but keeps key correctness). */
    if (terrain_layer_tex) {
        for (int li = 0; li < 4; li++)
            h = sr_fnv1a_step(h, &terrain_layer_tex[li].idx, sizeof(uint16_t));
    }
    return h ? h : 1u;
}

/* Resets the per-frame registry. Call at the top of each sr_render. */
static void sr_reset_material_cache(JceSceneRenderer *sr)
{
    sr->mat_count = 0;
}

/* Look up an existing entry by key, or append a new one. Returns the
 * resolved key (== input on success). On overflow returns 0 and emits
 * a one-shot warning per renderer; caller treats key=0 as "skip queue,
 * submit directly" (Phase 3 fallback). */
static uint32_t sr_register_material(JceSceneRenderer *sr,
                                     uint32_t key,
                                     const JcePbrMaterial *pbr,
                                     bool is_terrain,
                                     int terrain_slot,
                                     float terrain_tile_scale,
                                     bool terrain_splat_enabled,
                                     const bgfx_texture_handle_t *terrain_layer_tex)
{
    for (uint32_t i = 0; i < sr->mat_count; i++) {
        if (sr->mat_cache[i].key == key) return key;
    }
    if (sr->mat_count >= SR_MAT_CACHE_MAX) {
        static bool warned = false;
        if (!warned) {
            LOG_WARN(LOG_TAG, "material cache overflow (>%d unique materials/frame); "
                              "instancing disabled for excess", SR_MAT_CACHE_MAX);
            warned = true;
        }
        return 0u;
    }
    SrMaterialEntry *e = &sr->mat_cache[sr->mat_count++];
    e->key                   = key;
    e->pbr                   = *pbr;
    e->is_terrain            = is_terrain;
    e->terrain_slot          = terrain_slot;
    e->terrain_tile_scale    = terrain_tile_scale;
    e->terrain_splat_enabled = terrain_splat_enabled;
    if (terrain_layer_tex) {
        for (int li = 0; li < 4; li++) e->terrain_layer_tex[li] = terrain_layer_tex[li];
    } else {
        for (int li = 0; li < 4; li++) e->terrain_layer_tex[li] = sr->white_tex;
    }
    return key;
}

/* Render-queue binder callback. Invoked once per material run during
 * jce_rq_flush. Re-binds all textures / uniforms that jce_mesh_submit_pbr
 * USED to bind inline before it was decomposed. Frame-level state
 * (shadow VP, IBL handles) lives on `sr` directly. */
static void sr_bind_material_cb(uint32_t material_key, void *user)
{
    JceSceneRenderer *sr = (JceSceneRenderer *)user;
    if (!sr || material_key == 0u) return;

    SrMaterialEntry *e = NULL;
    for (uint32_t i = 0; i < sr->mat_count; i++) {
        if (sr->mat_cache[i].key == material_key) { e = &sr->mat_cache[i]; break; }
    }
    if (!e) return;

    /* PBR textures + factor uniforms. */
    jce_pbr_material_bind(&e->pbr, sr->renderer, sr->frame_view_id);

    /* Lighting uniforms (u_dirLights / u_pointLights / u_spotLights /
     * u_lightCounts / u_ambientColor / u_cameraPos). bgfx clears uniform
     * state after every submit, so we MUST re-apply the light env per
     * material run — otherwise only the first submitted entity in the
     * frame samples real lights and everything after renders unlit
     * (the symptom users report as "shaded looks identical to textured"). */
    if (sr->light_env)
        jce_light_env_apply(sr->light_env, sr->renderer);

    sr_bind_frame_shadow_state(sr);

    /* IBL. */
    float ibl_params[4] = {
        0.0f, 4.0f, 0.0f,
        sr->postfx_tonemap_active ? 1.0f : 0.0f
    };
    if (sr->skybox_active && sr->ibl_data) {
        JceTexture irr = jce_ibl_get_irradiance(sr->ibl_data);
        JceTexture pf  = jce_ibl_get_prefilter(sr->ibl_data);
        bgfx_texture_handle_t hi = { irr.idx };
        bgfx_texture_handle_t hp = { pf.idx };
        if (BGFX_HANDLE_IS_VALID(hi) && BGFX_HANDLE_IS_VALID(hp)
            && BGFX_HANDLE_IS_VALID(sr->brdf_lut)) {
            bgfx_set_texture(6, sr->u_ibl_irradiance, hi, UINT32_MAX);
            bgfx_set_texture(7, sr->u_ibl_prefilter,  hp, UINT32_MAX);
            bgfx_set_texture(8, sr->u_ibl_brdf_lut, sr->brdf_lut, UINT32_MAX);
            ibl_params[0] = 1.0f;
            /* y = max prefilter mip LEVEL = (mip count - 1); shader scales
             * perceptual roughness [0,1] by this. Use the ACTUAL count, not
             * a hardcoded 5.0 (the prefilter caps mips at 8 / by face size). */
            uint32_t mips = jce_ibl_get_prefilter_mips(sr->ibl_data);
            if (mips > 1) ibl_params[1] = (float)(mips - 1);
        }
    }
    /* Baked GI override: reflection-probe cubemap (stages 6/7) + SH9 ambient.
     * Must run after the sky-IBL bind so a local probe wins, and before
     * u_iblParams upload since it can force IBL on. */
    sr_bind_baked_gi(sr, ibl_params);
    bgfx_set_uniform(sr->u_ibl_params, ibl_params, 1);

    /* Terrain texture overrides + params (replaces stages 0/4 + adds 13/14/15). */
    if (e->is_terrain && e->terrain_slot >= 0 && e->terrain_slot < 16) {
        int ts = e->terrain_slot;
        bgfx_texture_handle_t splat_h = sr->terrain_cache[ts].splat_tex;
        if (!BGFX_HANDLE_IS_VALID(splat_h)) splat_h = sr->white_tex;
        bgfx_texture_handle_t l0 = e->terrain_layer_tex[0];
        bgfx_texture_handle_t l1 = e->terrain_layer_tex[1];
        bgfx_texture_handle_t l2 = e->terrain_layer_tex[2];
        bgfx_texture_handle_t l3 = e->terrain_layer_tex[3];
        if (!BGFX_HANDLE_IS_VALID(l0)) l0 = sr->white_tex;
        if (!BGFX_HANDLE_IS_VALID(l1)) l1 = sr->white_tex;
        if (!BGFX_HANDLE_IS_VALID(l2)) l2 = sr->white_tex;
        if (!BGFX_HANDLE_IS_VALID(l3)) l3 = sr->white_tex;
        bgfx_set_texture(0,  sr->s_terrain_layer0, l0,      UINT32_MAX);
        bgfx_set_texture(4,  sr->s_terrain_layer3, l3,      UINT32_MAX);
        bgfx_set_texture(13, sr->s_terrain_splat,  splat_h, UINT32_MAX);
        bgfx_set_texture(14, sr->s_terrain_layer1, l1,      UINT32_MAX);
        bgfx_set_texture(15, sr->s_terrain_layer2, l2,      UINT32_MAX);
        float tparams[4] = {
            e->terrain_tile_scale > 0.0f ? e->terrain_tile_scale : 10.0f,
            e->terrain_splat_enabled ? 1.0f : 0.0f,
            0.0f, 0.0f
        };
        bgfx_set_uniform(sr->u_terrain_params, tparams, 1);
    }
}

/* Suppress unused-static warnings until Phase 3 wires these in. */
/* Inline binding helper extracted from the legacy mesh main loop. Used
 * by the non-queue path (terrain, queue overflow, queue-disabled). */
static void sr_inline_bind_pbr_global(JceSceneRenderer *sr,
                                      const JcePbrMaterial *pbr,
                                      uint16_t view_id,
                                      JceScene *scene, EntityList *list)
{
    (void)scene;
    (void)list;

    jce_pbr_material_bind(pbr, sr->renderer, view_id);
    /* Re-apply lights per-entity — see sr_bind_material_cb for rationale. */
    if (sr->light_env)
        jce_light_env_apply(sr->light_env, sr->renderer);
    sr_bind_frame_shadow_state(sr);
    float ibl_params[4] = {
        0.0f, 4.0f, 0.0f,
        sr->postfx_tonemap_active ? 1.0f : 0.0f
    };
    if (sr->skybox_active && sr->ibl_data) {
        JceTexture irr = jce_ibl_get_irradiance(sr->ibl_data);
        JceTexture pf  = jce_ibl_get_prefilter(sr->ibl_data);
        bgfx_texture_handle_t hi = { irr.idx };
        bgfx_texture_handle_t hp = { pf.idx };
        if (BGFX_HANDLE_IS_VALID(hi) && BGFX_HANDLE_IS_VALID(hp)
            && BGFX_HANDLE_IS_VALID(sr->brdf_lut)) {
            bgfx_set_texture(6, sr->u_ibl_irradiance, hi, UINT32_MAX);
            bgfx_set_texture(7, sr->u_ibl_prefilter,  hp, UINT32_MAX);
            bgfx_set_texture(8, sr->u_ibl_brdf_lut, sr->brdf_lut, UINT32_MAX);
            ibl_params[0] = 1.0f;
            /* y = max prefilter mip LEVEL = (mip count - 1); see binder cb. */
            uint32_t mips = jce_ibl_get_prefilter_mips(sr->ibl_data);
            if (mips > 1) ibl_params[1] = (float)(mips - 1);
        }
    }
    /* Baked GI override (see sr_bind_material_cb). */
    sr_bind_baked_gi(sr, ibl_params);
    bgfx_set_uniform(sr->u_ibl_params, ibl_params, 1);
}

JceSceneRenderConfig jce_scene_render_config_default(void)
{
    JceSceneRenderConfig c;
    memset(&c, 0, sizeof(c));
    c.draw_skybox      = true;
    c.draw_shadows     = true;
    c.draw_opaque      = true;
    c.draw_sprites     = true;
    c.draw_transparent = true;
    c.apply_postfx     = true;
    c.postfx           = jce_postfx_default_params();
    c.shadow_map_size  = 0;
    c.csm_cascades     = 0;
    c.shadow_distance  = 0.0f;
    c.csm_split_lambda = -1.0f;
    c.fog_enabled            = false;
    c.fog                    = jce_volumetric_fog_default_params();
    c.fog_depth_tex_handle   = UINT16_MAX;
    return c;
}

JceSceneRenderer *jce_scene_renderer_create(JceRenderer *renderer,
                                            const JcePakArchive *pak,
                                            const JceSceneRendererCallbacks *cbs)
{
    if (!renderer) return NULL;

    JceSceneRenderer *sr = (JceSceneRenderer *)JCE_CALLOC(1, sizeof(JceSceneRenderer));
    if (!sr) return NULL;

    sr->renderer = renderer;
    sr->pak = pak;
    if (cbs) { sr->cbs = *cbs; sr->has_cbs = true; }

    /* Invalidate handles. */
    sr->white_tex.idx       = UINT16_MAX;
    sr->checker_tex.idx     = UINT16_MAX;
    sr->prog_sky.idx        = UINT16_MAX;
    sr->u_sky_colors.idx    = UINT16_MAX;
    sr->u_sky_params.idx    = UINT16_MAX;
    sr->u_sky_equirect.idx  = UINT16_MAX;
    sr->u_light_dir.idx     = UINT16_MAX;
    sr->u_light_color.idx   = UINT16_MAX;
    sr->shadow_tex.idx      = UINT16_MAX;
    sr->shadow_fbo.idx      = UINT16_MAX;
    sr->u_shadowMap.idx     = UINT16_MAX;
    sr->u_shadowVP.idx      = UINT16_MAX;
    sr->local_atlas_tex.idx       = UINT16_MAX;
    sr->local_atlas_fbo.idx       = UINT16_MAX;
    sr->u_local_shadow_map.idx    = UINT16_MAX;
    sr->u_local_shadow_vp.idx     = UINT16_MAX;
    sr->u_local_shadow_params.idx = UINT16_MAX;
    sr->u_spot_shadow_slot.idx    = UINT16_MAX;
    sr->u_point_shadow_slot.idx   = UINT16_MAX;
    sr->u_csm_vp.idx        = UINT16_MAX;
    sr->u_csm_splits.idx    = UINT16_MAX;
    sr->u_csm_params.idx    = UINT16_MAX;
    sr->u_csm_bias_scales.idx = UINT16_MAX;
    sr->brdf_lut.idx        = UINT16_MAX;
    sr->u_ibl_irradiance.idx = UINT16_MAX;
    sr->u_ibl_prefilter.idx  = UINT16_MAX;
    sr->u_ibl_brdf_lut.idx   = UINT16_MAX;
    sr->u_ibl_params.idx     = UINT16_MAX;
    sr->u_sh9.idx            = UINT16_MAX;
    sr->u_gi_params.idx      = UINT16_MAX;
    sr->gi_probe_spec.idx    = UINT16_MAX;
    sr->gi_probe_irr.idx     = UINT16_MAX;
    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
        sr->csm_tex[i].idx = UINT16_MAX;
        sr->csm_fbo[i].idx = UINT16_MAX;
        sr->u_csm_samplers[i].idx = UINT16_MAX;
    }

    const bgfx_caps_t *caps = bgfx_get_caps();
    sr->homogeneous_depth = caps ? caps->homogeneousDepth : false;

    /* Pick best depth format. */
    bgfx_texture_format_t depth_fmt = BGFX_TEXTURE_FORMAT_D16;
    if (caps) {
        uint16_t d32f = caps->formats[BGFX_TEXTURE_FORMAT_D32F];
        uint16_t d24  = caps->formats[BGFX_TEXTURE_FORMAT_D24S8];
        if ((d32f & BGFX_CAPS_FORMAT_TEXTURE_2D)
            && (d32f & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER))
            depth_fmt = BGFX_TEXTURE_FORMAT_D32F;
        else if ((d24 & BGFX_CAPS_FORMAT_TEXTURE_2D)
                 && (d24 & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER))
            depth_fmt = BGFX_TEXTURE_FORMAT_D24S8;
    }
    sr->shadow_depth_fmt = depth_fmt;

    /* GPU-tier-driven shadow defaults. */
    {
        JceRenderRecommendation rec = jce_renderer_get_recommendation();
        uint32_t sz = rec.shadow_map_size;
        if (sz < 512)  sz = 512;   /* honor the LOW-tier 512 recommendation
                                      (was floored to 1024 → 4x shadow VRAM on
                                      the 512MB / no-GPU baseline) */
        if (sz > 4096) sz = 4096;
        sr->shadow_map_size = (uint16_t)sz;

        switch (rec.tier) {
        case JCE_GPU_TIER_HIGH:
            sr->csm_blend_ratio   = 0.22f;
            sr->csm_normal_bias   = 0.015f;
            sr->csm_filter_radius = 1.6f;
            break;
        case JCE_GPU_TIER_MEDIUM:
            sr->csm_blend_ratio   = 0.20f;
            sr->csm_normal_bias   = 0.012f;
            sr->csm_filter_radius = 1.4f;
            break;
        case JCE_GPU_TIER_LOW:
        default:
            sr->csm_blend_ratio   = 0.18f;
            sr->csm_normal_bias   = 0.010f;
            sr->csm_filter_radius = 1.2f;
            break;
        }
    }

    LOG_INFO(LOG_TAG, "[init] sky shader + uniforms");
    /* Sky vertex layout & shader. */
    bgfx_vertex_layout_begin(&sr->sky_layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&sr->sky_layout, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&sr->sky_layout);

    {
        JceShaderHandle sky_sh = shader_load_program(pak, "sky");
        sr->prog_sky.idx = sky_sh.idx;
        if (sky_sh.idx == UINT16_MAX)
            LOG_WARN(LOG_TAG, "sky shader not found in PAK");
    }

    sr->u_sky_colors   = bgfx_create_uniform("u_sky_colors",
                                             BGFX_UNIFORM_TYPE_VEC4, 3);
    sr->u_sky_params   = bgfx_create_uniform("u_sky_params",
                                             BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->u_sky_equirect = bgfx_create_uniform("s_equirect",
                                             BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->u_light_dir   = bgfx_create_uniform("u_lightDir",
                                            BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->u_light_color = bgfx_create_uniform("u_lightColor",
                                            BGFX_UNIFORM_TYPE_VEC4, 1);

    LOG_INFO(LOG_TAG, "[init] procedural meshes");
    /* Procedural meshes. */
    sr->cube_mesh     = jce_mesh_create_cube(1.0f);
    sr->plane_mesh    = jce_mesh_create_plane(1.0f, 1.0f, 0);
    sr->sphere_mesh   = jce_mesh_create_sphere(0.5f);
    sr->capsule_mesh  = jce_mesh_create_capsule(0.25f, 1.0f);
    sr->cylinder_mesh = jce_mesh_create_cylinder(0.5f, 1.0f);

    LOG_INFO(LOG_TAG, "[init] fallback textures");
    /* 1×1 white fallback texture. */
    {
        uint32_t white = 0xFFFFFFFFu;
        const bgfx_memory_t *mem = bgfx_copy(&white, 4);
        sr->white_tex = bgfx_create_texture_2d(1, 1, false, 1,
                                               BGFX_TEXTURE_FORMAT_RGBA8, 0, mem);
    }

    /* 8×8 magenta/yellow "missing texture" checkerboard (TEXTURED-mode
     * fallback when an entity has no albedo). ABGR pixel layout. */
    {
        const uint32_t MAG = 0xFFFF00FFu; /* alpha=FF, r=FF, g=00, b=FF → magenta */
        const uint32_t YEL = 0xFF00FFFFu; /* alpha=FF, r=FF, g=FF, b=00 → yellow */
        uint32_t pix[8 * 8];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                pix[y * 8 + x] = (((x ^ y) >> 1) & 1) ? MAG : YEL;
        const bgfx_memory_t *mem = bgfx_copy(pix, sizeof(pix));
        sr->checker_tex = bgfx_create_texture_2d(8, 8, false, 1,
                                                 BGFX_TEXTURE_FORMAT_RGBA8, 0, mem);
    }

    LOG_INFO(LOG_TAG, "[init] shadow map (depth_fmt=%d)", (int)depth_fmt);
    /* Shadow map (legacy single-cascade). */
    {
        const uint16_t sz = sr->shadow_map_size;
        sr->shadow_tex = bgfx_create_texture_2d(sz, sz, false, 1, depth_fmt,
            BGFX_TEXTURE_RT
            | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
            | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
            NULL);
        bgfx_attachment_t at;
        memset(&at, 0, sizeof(at));
        bgfx_attachment_init(&at, sr->shadow_tex, BGFX_ACCESS_WRITE,
                             0, 1, 0, BGFX_RESOLVE_NONE);
        sr->shadow_fbo = bgfx_create_frame_buffer_from_attachment(1, &at, false);
        sr->u_shadowMap = bgfx_create_uniform("s_shadowMap",
                                              BGFX_UNIFORM_TYPE_SAMPLER, 1);
        sr->u_shadowVP  = bgfx_create_uniform("u_shadowVP",
                                              BGFX_UNIFORM_TYPE_MAT4, 1);
        sr->shadow_valid = BGFX_HANDLE_IS_VALID(sr->shadow_fbo);
        LOG_INFO(LOG_TAG, "[init] shadow_valid=%d", (int)sr->shadow_valid);
    }

    LOG_INFO(LOG_TAG, "[init] CSM cascades");
    /* CSM cascades. */
    {
        const uint16_t sz = sr->shadow_map_size;
        const char *names[JCE_CSM_MAX_CASCADES] = {
            "s_csmShadow0", "s_csmShadow1", "s_csmShadow2", "s_csmShadow3"
        };
        /* Tier-scale the default cascade count: each cascade is a full-scene
           depth pass, so old / integrated GPUs render fewer. A per-scene or
           per-config override (jce_scene_renderer_render, below) still wins. */
        switch (jce_renderer_get_tier()) {
        case JCE_GPU_TIER_LOW:    sr->csm_cascade_count = 1; break;
        case JCE_GPU_TIER_MEDIUM: sr->csm_cascade_count = 2; break;
        default:                  sr->csm_cascade_count = JCE_CSM_MAX_CASCADES; break;
        }
        sr->csm_valid = true;
        for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
            sr->csm_tex[i] = bgfx_create_texture_2d(sz, sz, false, 1, depth_fmt,
                BGFX_TEXTURE_RT
                | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
                NULL);
            bgfx_attachment_t at;
            memset(&at, 0, sizeof(at));
            bgfx_attachment_init(&at, sr->csm_tex[i], BGFX_ACCESS_WRITE,
                                 0, 1, 0, BGFX_RESOLVE_NONE);
            sr->csm_fbo[i] = bgfx_create_frame_buffer_from_attachment(1, &at, false);
            sr->u_csm_samplers[i] = bgfx_create_uniform(names[i],
                BGFX_UNIFORM_TYPE_SAMPLER, 1);
            if (!BGFX_HANDLE_IS_VALID(sr->csm_fbo[i])) sr->csm_valid = false;
        }
        sr->u_csm_vp = bgfx_create_uniform("u_csmVP",
            BGFX_UNIFORM_TYPE_MAT4, JCE_CSM_MAX_CASCADES);
        sr->u_csm_splits      = bgfx_create_uniform("u_csmSplits",
            BGFX_UNIFORM_TYPE_VEC4, 1);
        sr->u_csm_params      = bgfx_create_uniform("u_csmParams",
            BGFX_UNIFORM_TYPE_VEC4, 1);
        sr->u_csm_bias_scales = bgfx_create_uniform("u_csmBiasScales",
            BGFX_UNIFORM_TYPE_VEC4, 1);
        LOG_INFO(LOG_TAG, "[init] csm_valid=%d", (int)sr->csm_valid);
    }

    /* Local (spot/point) shadow atlas uniforms — P1. The atlas texture +
       FBO are (re)created in sr_create_shadow_targets so they track the
       shadow resolution; the uniforms live for the renderer's lifetime. */
    sr->u_local_shadow_map = bgfx_create_uniform("s_localShadowMap",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->u_local_shadow_vp = bgfx_create_uniform("u_localShadowVP",
        BGFX_UNIFORM_TYPE_MAT4, JCE_MAX_LOCAL_SHADOWS);
    sr->u_local_shadow_params = bgfx_create_uniform("u_localShadowParams",
        BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->u_spot_shadow_slot = bgfx_create_uniform("u_spotShadowSlot",
        BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->u_point_shadow_slot = bgfx_create_uniform("u_pointShadowSlot",
        BGFX_UNIFORM_TYPE_VEC4, 2);   /* 8 point lanes */

    LOG_INFO(LOG_TAG, "[init] light env");
    /* Multi-light env. */
    sr->light_env = jce_light_env_create();

    LOG_INFO(LOG_TAG, "[init] IBL uniforms + BRDF LUT");
    /* IBL uniforms + BRDF LUT. */
    sr->u_ibl_irradiance = bgfx_create_uniform("s_irradiance",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->u_ibl_prefilter  = bgfx_create_uniform("s_prefilter",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->u_ibl_brdf_lut   = bgfx_create_uniform("s_brdfLUT",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->u_ibl_params     = bgfx_create_uniform("u_iblParams",
        BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Baked GI: SH9 ambient (9 vec4 RGB coeffs) + GI params. */
    sr->u_sh9       = bgfx_create_uniform("u_sh9", BGFX_UNIFORM_TYPE_VEC4, 9);
    sr->u_gi_params = bgfx_create_uniform("u_giParams", BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Terrain shader bindings (lazy: created here so submit-time has
     * valid handles even when no terrain is bound). */
    sr->u_terrain_params = bgfx_create_uniform("u_terrainParams",
        BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->s_terrain_splat  = bgfx_create_uniform("s_splatMap",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->s_terrain_layer1 = bgfx_create_uniform("s_layer1",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->s_terrain_layer2 = bgfx_create_uniform("s_layer2",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->s_terrain_layer0 = bgfx_create_uniform("s_albedo",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sr->s_terrain_layer3 = bgfx_create_uniform("s_emissive",
        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    for (int ti = 0; ti < 16; ti++)
        sr->terrain_cache[ti].splat_tex.idx = UINT16_MAX;

    LOG_INFO(LOG_TAG, "[init] BRDF LUT");
    {
        JceTexture brdf = jce_ibl_create_brdf_lut(256);
        sr->brdf_lut.idx = brdf.idx;
        LOG_INFO(LOG_TAG, "[init] BRDF LUT done: idx=%u", (unsigned)brdf.idx);
    }
    sr->skybox = NULL;
    sr->ibl_data = NULL;
    sr->skybox_active = false;
    sr->skybox_hdr_path[0] = '\0';

    LOG_INFO(LOG_TAG, "[init] sprite batch + postfx");
    /* Sprite batch. */
    sr->sprite_batch = jce_sprite_batch_create(256);

    /* PostFX pipeline. */
    sr->postfx_pipeline = jce_postfx_create(jce_allocator_default(), 1, 1);
    if (sr->postfx_pipeline) {
        if (!jce_postfx_load_shaders(sr->postfx_pipeline, pak))
            LOG_WARN(LOG_TAG, "postfx shaders failed to load");
    }

    LOG_INFO(LOG_TAG, "[init] render queue");
    LOG_INFO(LOG_TAG, "scene renderer created");
    /* Phase 2: per-frame material registry for queue-based instancing.
     * Queue itself is created lazily on first use to keep the create
     * path lean; render path wires the binder in Phase 3. */
    sr->render_queue = jce_rq_create(4096);
    /* Transparent queue: smaller (transparency is the minority) and never
     * auto-instances so its back-to-front order is preserved exactly. */
    sr->transparent_queue = jce_rq_create(1024);
    if (sr->transparent_queue)
        jce_rq_set_no_batch(sr->transparent_queue, true);
    sr->mat_count = 0;
    sr->frame_view_id = 0;
    sr->frame_shadow_vp_valid = false; sr->frame_shadow_active = false;
    return sr;
}

/* Public accessors so editors/tools can read the live animation state
 * without keeping a second cache of their own. */
struct JceAnimPlayer *jce_scene_renderer_get_anim_player(
    JceSceneRenderer *sr, const char *skeleton_path)
{
    if (!sr || !skeleton_path || !skeleton_path[0]) return NULL;
    /* Players are per-entity now; return the first live instance using this
       path's model (enough for the editor's single progress-bar/timeline view). */
    JceModel *model = NULL;
    for (int i = 0; i < SR_MODEL_CACHE_MAX; i++) {
        SrModelCache *e = &sr->model_cache[i];
        if (e->used && strcmp(e->path, skeleton_path) == 0) { model = e->model; break; }
    }
    if (!model) return NULL;
    for (int i = 0; i < SR_ANIM_INSTANCE_MAX; i++) {
        SrAnimInstance *a = &sr->anim_inst[i];
        if (a->used && a->model == model && a->player)
            return (struct JceAnimPlayer *)a->player;
    }
    return NULL;
}

struct JceAnimSmBinding *jce_scene_renderer_get_anim_sm(
    JceSceneRenderer *sr, uint32_t entity)
{
    if (!sr) return NULL;
    for (int i = 0; i < SR_ANIM_INSTANCE_MAX; i++) {
        SrAnimInstance *a = &sr->anim_inst[i];
        if (a->used && a->entity == entity)
            return a->sm_binding;
    }
    return NULL;
}

struct JceModel *jce_scene_renderer_get_model(
    JceSceneRenderer *sr, const char *skeleton_path)
{
    if (!sr || !skeleton_path || !skeleton_path[0]) return NULL;
    for (int i = 0; i < SR_MODEL_CACHE_MAX; i++) {
        SrModelCache *e = &sr->model_cache[i];
        if (e->used && strcmp(e->path, skeleton_path) == 0)
            return (struct JceModel *)e->model;
    }
    return NULL;
}

void jce_scene_renderer_destroy(JceSceneRenderer *sr)
{
    if (!sr) return;

    /* Per-entity animation instances (players reference, but don't own, the
       shared skeletons in model_cache — destroy them before the models). */
    for (int i = 0; i < SR_ANIM_INSTANCE_MAX; i++) {
        SrAnimInstance *a = &sr->anim_inst[i];
        if (a->used && a->player) jce_anim_player_destroy(a->player);
        if (a->sm_binding) { jce_anim_sm_binding_destroy(a->sm_binding); a->sm_binding = NULL; }
        if (a->blend_tree) { jce_anim_blend_tree_destroy(a->blend_tree); a->blend_tree = NULL; }
        if (a->ev_pool)    { JCE_FREE(a->ev_pool); a->ev_pool = NULL; }
        a->used = false;
    }

    /* 2D sprite-animator instances (own their sheet + player). */
    for (int i = 0; i < SR_SPRITE_ANIM_MAX; i++) {
        SrSpriteAnim *s = &sr->sprite_anim[i];
        if (s->player) { jce_sprite_player_destroy(s->player); s->player = NULL; }
        if (s->sheet)  { jce_sprite_sheet_destroy(s->sheet);   s->sheet  = NULL; }
        s->used = false;
    }

    /* Model cache. */
    for (int i = 0; i < SR_MODEL_CACHE_MAX; i++) {
        SrModelCache *e = &sr->model_cache[i];
        if (!e->used) continue;
        if (e->model)  jce_model_destroy(e->model);
        e->used = false;
    }

    /* Shader Graph custom-program cache: lookup-only.  The programs are
     * owned by the process-wide cache in jce_pbr_material.c (freed by
     * jce_pbr_material_shutdown during renderer teardown), so we only drop
     * our references here — no destroy, to avoid a double-free. */
    sr->prog_cache_count = 0;

    if (sr->cube_mesh)     jce_mesh_destroy(sr->cube_mesh);
    if (sr->plane_mesh)    jce_mesh_destroy(sr->plane_mesh);
    if (sr->sphere_mesh)   jce_mesh_destroy(sr->sphere_mesh);
    if (sr->capsule_mesh)  jce_mesh_destroy(sr->capsule_mesh);
    if (sr->cylinder_mesh) jce_mesh_destroy(sr->cylinder_mesh);

    /* Terrain cache. */
    for (int i = 0; i < 16; i++) {
        if (!sr->terrain_cache[i].used) continue;
        sr_terrain_free_chunks(sr, i);
        if (sr->terrain_cache[i].terrain) jce_terrain_free(sr->terrain_cache[i].terrain);
        if (BGFX_HANDLE_IS_VALID(sr->terrain_cache[i].splat_tex))
            bgfx_destroy_texture(sr->terrain_cache[i].splat_tex);
        sr->terrain_cache[i].used = false;
    }
    if (BGFX_HANDLE_IS_VALID(sr->u_terrain_params)) bgfx_destroy_uniform(sr->u_terrain_params);
    if (BGFX_HANDLE_IS_VALID(sr->s_terrain_splat))  bgfx_destroy_uniform(sr->s_terrain_splat);
    if (BGFX_HANDLE_IS_VALID(sr->s_terrain_layer1)) bgfx_destroy_uniform(sr->s_terrain_layer1);
    if (BGFX_HANDLE_IS_VALID(sr->s_terrain_layer2)) bgfx_destroy_uniform(sr->s_terrain_layer2);
    if (BGFX_HANDLE_IS_VALID(sr->s_terrain_layer0)) bgfx_destroy_uniform(sr->s_terrain_layer0);
    if (BGFX_HANDLE_IS_VALID(sr->s_terrain_layer3)) bgfx_destroy_uniform(sr->s_terrain_layer3);

    if (BGFX_HANDLE_IS_VALID(sr->white_tex))      bgfx_destroy_texture(sr->white_tex);
    if (BGFX_HANDLE_IS_VALID(sr->checker_tex))    bgfx_destroy_texture(sr->checker_tex);
    if (BGFX_HANDLE_IS_VALID(sr->prog_sky))       bgfx_destroy_program(sr->prog_sky);
    if (BGFX_HANDLE_IS_VALID(sr->u_sky_colors))   bgfx_destroy_uniform(sr->u_sky_colors);
    if (BGFX_HANDLE_IS_VALID(sr->u_sky_params))   bgfx_destroy_uniform(sr->u_sky_params);
    if (BGFX_HANDLE_IS_VALID(sr->u_sky_equirect)) bgfx_destroy_uniform(sr->u_sky_equirect);
    if (BGFX_HANDLE_IS_VALID(sr->u_light_dir))    bgfx_destroy_uniform(sr->u_light_dir);
    if (BGFX_HANDLE_IS_VALID(sr->u_light_color))  bgfx_destroy_uniform(sr->u_light_color);

    sr_destroy_shadow_targets(sr);
    if (BGFX_HANDLE_IS_VALID(sr->u_shadowMap)) bgfx_destroy_uniform(sr->u_shadowMap);
    if (BGFX_HANDLE_IS_VALID(sr->u_shadowVP))  bgfx_destroy_uniform(sr->u_shadowVP);
    if (BGFX_HANDLE_IS_VALID(sr->u_local_shadow_map))    bgfx_destroy_uniform(sr->u_local_shadow_map);
    if (BGFX_HANDLE_IS_VALID(sr->u_local_shadow_vp))     bgfx_destroy_uniform(sr->u_local_shadow_vp);
    if (BGFX_HANDLE_IS_VALID(sr->u_local_shadow_params)) bgfx_destroy_uniform(sr->u_local_shadow_params);
    if (BGFX_HANDLE_IS_VALID(sr->u_spot_shadow_slot))    bgfx_destroy_uniform(sr->u_spot_shadow_slot);
    if (BGFX_HANDLE_IS_VALID(sr->u_point_shadow_slot))   bgfx_destroy_uniform(sr->u_point_shadow_slot);

    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
        if (BGFX_HANDLE_IS_VALID(sr->u_csm_samplers[i]))
            bgfx_destroy_uniform(sr->u_csm_samplers[i]);
    }
    if (BGFX_HANDLE_IS_VALID(sr->u_csm_vp))         bgfx_destroy_uniform(sr->u_csm_vp);
    if (BGFX_HANDLE_IS_VALID(sr->u_csm_splits))     bgfx_destroy_uniform(sr->u_csm_splits);
    if (BGFX_HANDLE_IS_VALID(sr->u_csm_params))     bgfx_destroy_uniform(sr->u_csm_params);
    if (BGFX_HANDLE_IS_VALID(sr->u_csm_bias_scales))bgfx_destroy_uniform(sr->u_csm_bias_scales);

    if (sr->light_env) jce_light_env_destroy(sr->light_env);
    if (sr->ibl_data)  jce_ibl_destroy(sr->ibl_data);
    if (sr->skybox)    jce_skybox_destroy(sr->skybox);
    if (sr->vfog)      jce_volumetric_fog_destroy(sr->vfog);

    /* Weather / decals (P2-weather-decals-tod). */
    if (sr->weather)         jce_weather_destroy(sr->weather);
    if (sr->decals)          jce_decals_destroy(sr->decals);
    if (sr->decals_authored) jce_decals_destroy(sr->decals_authored);

    if (BGFX_HANDLE_IS_VALID(sr->brdf_lut))         bgfx_destroy_texture(sr->brdf_lut);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_irradiance)) bgfx_destroy_uniform(sr->u_ibl_irradiance);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_prefilter))  bgfx_destroy_uniform(sr->u_ibl_prefilter);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_brdf_lut))   bgfx_destroy_uniform(sr->u_ibl_brdf_lut);
    if (BGFX_HANDLE_IS_VALID(sr->u_ibl_params))     bgfx_destroy_uniform(sr->u_ibl_params);
    if (BGFX_HANDLE_IS_VALID(sr->u_sh9))            bgfx_destroy_uniform(sr->u_sh9);
    if (BGFX_HANDLE_IS_VALID(sr->u_gi_params))      bgfx_destroy_uniform(sr->u_gi_params);

    /* Release baked reflection-probe cubemaps loaded this session. */
    for (int i = 0; i < sr->rprobe_cache_count; i++) {
        if (BGFX_HANDLE_IS_VALID(sr->rprobe_cache[i].spec))
            bgfx_destroy_texture(sr->rprobe_cache[i].spec);
        if (BGFX_HANDLE_IS_VALID(sr->rprobe_cache[i].irr))
            bgfx_destroy_texture(sr->rprobe_cache[i].irr);
    }

    if (sr->sprite_batch)    jce_sprite_batch_destroy(sr->sprite_batch);
    if (sr->postfx_pipeline) jce_postfx_destroy(sr->postfx_pipeline);

    if (sr->cull_space) jce_space_destroy(sr->cull_space);
    if (sr->cull_aabbs) JCE_FREE(sr->cull_aabbs);
    if (sr->render_queue) jce_rq_destroy(sr->render_queue);
    if (sr->transparent_queue) jce_rq_destroy(sr->transparent_queue);

    JCE_FREE(sr);
}

/* ── Particle visualisation (P2-particle-vfx-runtime) ─────────────────
 *
 * The CPU particle backend (jce_particles.c) has no dedicated GPU
 * billboard pass yet, so alive particles from the scene-owned
 * JceParticleSystem are submitted through the debug-line pipeline as small
 * axis crosses (camera-agnostic, depth-tested) and flushed with the
 * renderer's color program.  This is the single render path used by both
 * the editor and the shipping runtime, so it owns the flush — no reliance
 * on an external debug-draw flush, and the buffer is always cleared. */
static void sr_particle_visit(const JceParticleView *p, void *ud)
{
    (void)ud;
    float r = p->size * 0.5f;
    if (r < 0.02f) r = 0.02f;

    int rr = (int)(p->color.x * 255.0f); rr = rr < 0 ? 0 : (rr > 255 ? 255 : rr);
    int gg = (int)(p->color.y * 255.0f); gg = gg < 0 ? 0 : (gg > 255 ? 255 : gg);
    int bb = (int)(p->color.z * 255.0f); bb = bb < 0 ? 0 : (bb > 255 ? 255 : bb);
    int aa = (int)(p->color.w * 255.0f); aa = aa < 0 ? 0 : (aa > 255 ? 255 : aa);
    uint32_t abgr = ((uint32_t)aa << 24) | ((uint32_t)bb << 16) |
                    ((uint32_t)gg << 8)  |  (uint32_t)rr;

    jce_vec3 c = p->position;
    jce_debug_draw_line(jce_v3(c.x - r, c.y, c.z), jce_v3(c.x + r, c.y, c.z), abgr);
    jce_debug_draw_line(jce_v3(c.x, c.y - r, c.z), jce_v3(c.x, c.y + r, c.z), abgr);
    jce_debug_draw_line(jce_v3(c.x, c.y, c.z - r), jce_v3(c.x, c.y, c.z + r), abgr);
}

typedef struct {
    const JceParticleSystem *sys;
    JceScene                *scene;
} SrParticleEachCtx;

static void sr_particle_each_entity(JceScene *s, JceEntity e, void *ud)
{
    SrParticleEachCtx *ctx = (SrParticleEachCtx *)ud;
    JceParticleEmitterComponent *c = jce_scene_get_particle_emitter(s, e);
    if (!c || !c->loaded || c->emitter_handle_idx == UINT32_MAX) return;
    if (!jce_scene_component_enabled(s, e, JCE_COMP_FLAG_PARTICLE_EMITTER)) return;
    JceEmitterHandle h = { c->emitter_handle_idx };
    jce_particles_emitter_for_each(ctx->sys, h, sr_particle_visit, NULL);
}

static void sr_draw_particles(JceSceneRenderer *sr, JceScene *scene,
                              uint16_t view_id)
{
    const JceParticleSystem *sys =
        (const JceParticleSystem *)jce_scene_internal_particles_get(scene);
    if (!sys || jce_particles_alive_count(sys) == 0) return;

    SrParticleEachCtx ctx = { sys, scene };
    jce_scene_each_entity(scene, sr_particle_each_entity, &ctx);
    jce_debug_draw_flush(view_id, sr->renderer);
}

/* ── Time-of-day / weather / decals (P2-weather-decals-tod) ───────────
 *
 * These three authored environment systems were built (jce_time_of_day.c,
 * jce_weather.c, jce_decals.c) but never created or driven outside smoke
 * tests.  They are wired here, off the scene's serialized rendering
 * settings + JceDecalComponent, so a scene that authors them sees them
 * update and render every frame in both the editor preview and runtime.
 */

/* Advance the internal day clock and push the evaluated lighting snapshot
 * into the renderer's ToD override (consumed by the sky + lighting passes
 * already wired to sr->tod_state). */
static void sr_drive_time_of_day(JceSceneRenderer *sr,
                                 const JceSceneRenderingSettings *rs,
                                 float dt_sec)
{
    if (!sr || !rs) return;

    if (!rs->tod_enabled) {
        /* Disabled this frame: release the override the first frame after a
         * toggle-off so the renderer's hardcoded sky returns. */
        if (sr->tod_driven) {
            jce_scene_renderer_set_time_of_day(sr, NULL);
            sr->tod_driven    = false;
            sr->tod_clock_valid = false;
        }
        return;
    }

    /* (Re)seed the clock from the authored start hour whenever it is not yet
     * running, so editing tod_hour while paused (speed==0) takes effect. */
    if (!sr->tod_clock_valid || rs->tod_speed <= 0.0f) {
        sr->tod_clock_hour  = rs->tod_hour;
        sr->tod_clock_valid = true;
    }
    if (rs->tod_speed > 0.0f && dt_sec > 0.0f) {
        sr->tod_clock_hour += rs->tod_speed * dt_sec;
        while (sr->tod_clock_hour >= 24.0f) sr->tod_clock_hour -= 24.0f;
        while (sr->tod_clock_hour < 0.0f)   sr->tod_clock_hour += 24.0f;
    }

    JceTimeOfDayConfig cfg = jce_time_of_day_default_config();
    cfg.latitude_degrees = rs->tod_latitude;
    cfg.dawn_hour        = rs->tod_dawn_hour;
    cfg.dusk_hour        = rs->tod_dusk_hour;

    JceTimeOfDayState state;
    jce_time_of_day_evaluate(&cfg, sr->tod_clock_hour, &state);
    jce_scene_renderer_set_time_of_day(sr, &state);
    sr->tod_driven = true;
}

/* Lazily create the screen-space weather overlay, sync its state from the
 * authored settings, advance its animation clock and render it on top of the
 * scene color view. */
static void sr_drive_weather(JceSceneRenderer *sr,
                             const JceSceneRenderingSettings *rs,
                             uint16_t view_id, float dt_sec)
{
    if (!sr || !rs || !sr->pak) return;

    JceWeatherType type = (JceWeatherType)rs->weather_type;
    if (type == JCE_WEATHER_CLEAR || rs->weather_intensity <= 0.0f) {
        /* Nothing to draw; leave any existing system idle (cheap). */
        if (sr->weather) {
            JceWeatherState clear = jce_weather_default(JCE_WEATHER_CLEAR, 0.0f);
            jce_weather_set_state(sr->weather, &clear);
        }
        return;
    }

    if (!sr->weather) {
        JceWeatherDesc d = { sr->pak };
        sr->weather = jce_weather_create(&d);
        if (!sr->weather) return;   /* shader missing — fail soft */
    }

    JceWeatherState st = jce_weather_default(type, rs->weather_intensity);
    jce_weather_set_state(sr->weather, &st);
    jce_weather_update(sr->weather, dt_sec);
    jce_weather_render(sr->weather, view_id);
}

/* Rebuild the authored-decal pool each frame from JceDecalComponent
 * projectors so transform / colour edits update live, then render both the
 * authored and the runtime-stamped pools into the color view. */
typedef struct { JceSceneRenderer *sr; JceScene *scene; uint32_t spawned; } SrDecalEachCtx;

static void sr_decal_each_entity(JceScene *scene, JceEntity e, void *ud)
{
    SrDecalEachCtx *ctx = (SrDecalEachCtx *)ud;
    JceSceneRenderer *sr = ctx->sr;
    JceDecalComponent *dc = jce_scene_get_decal(scene, e);
    if (!dc) return;
    if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_DECAL)) return;
    if (dc->opacity <= 0.0f) return;

    /* World transform of the projector entity. */
    jce_mat4 w = jce_scene_get_world_matrix(scene, e);
    jce_vec3 pos = jce_v3(w.raw[3][0], w.raw[3][1], w.raw[3][2]);

    /* Projector points DOWN its local -Y by convention (Unity/HDRP decal);
     * the surface normal we stamp is that projection axis.  Columns of the
     * world matrix are the rotated local axes. */
    jce_vec3 down = jce_v3(-w.raw[1][0], -w.raw[1][1], -w.raw[1][2]);
    jce_vec3 right = jce_v3(w.raw[0][0], w.raw[0][1], w.raw[0][2]);

    /* Offset the stamp to the projector's far face so it sits on the surface
     * below the pivot rather than floating at the box centre. */
    float depth = dc->size[2] > 0.0f ? dc->size[2] : 1.0f;
    jce_vec3 hit = jce_v3(pos.x + down.x * depth * 0.5f + dc->pivot[0],
                          pos.y + down.y * depth * 0.5f + dc->pivot[1],
                          pos.z + down.z * depth * 0.5f + dc->pivot[2]);

    JceDecalSpawn s;
    memset(&s, 0, sizeof s);
    s.position     = hit;
    s.normal       = jce_v3(-down.x, -down.y, -down.z);  /* face toward projector */
    s.tangent_hint = right;
    float sx = dc->size[0] > 0.0f ? dc->size[0] : 1.0f;
    float sy = dc->size[1] > 0.0f ? dc->size[1] : sx;
    s.size      = (sx > sy ? sx : sy);
    s.thickness = 0.01f;
    s.texture   = sr_resolve_texture(sr, dc->material_path);
    s.tint      = jce_v4(dc->color[0], dc->color[1], dc->color[2],
                         dc->color[3] * dc->opacity);
    s.lifetime_seconds = 0.0f;   /* authored projectors are persistent */

    if (jce_decals_spawn(sr->decals_authored, &s))
        ctx->spawned++;
}

static void sr_drive_decals(JceSceneRenderer *sr, JceScene *scene,
                            uint16_t view_id, float dt_sec)
{
    if (!sr || !scene || !sr->pak) return;

    /* Rebuild the authored projector pool from scratch this frame. */
    if (!sr->decals_authored) {
        JceDecalPoolDesc d = { 256u, sr->pak };
        sr->decals_authored = jce_decals_create(&d);
        if (!sr->decals_authored) return;   /* shader missing — fail soft */
    }
    jce_decals_clear(sr->decals_authored);

    SrDecalEachCtx ctx = { sr, scene, 0 };
    jce_scene_each_entity(scene, sr_decal_each_entity, &ctx);
    if (ctx.spawned > 0)
        jce_decals_render(sr->decals_authored, view_id);

    /* Runtime-stamped decals (created on demand by the public spawn API). */
    if (sr->decals) {
        jce_decals_update(sr->decals, dt_sec);
        jce_decals_render(sr->decals, view_id);
    }
}

uint16_t jce_scene_renderer_render(JceSceneRenderer *sr, JceScene *scene,
                                   const JceCamera *camera,
                                   uint16_t view_id_base, float dt_sec,
                                   const JceSceneRenderConfig *config)
{
    if (!sr || !scene) return view_id_base;
    JCE_PROFILE_ZONE_N("SceneRenderer::Render");

    /* Start a fresh world-matrix cache generation for this render. The
       multiple sub-passes below (shadow producers + color + terrain) each
       read jce_scene_get_world_matrix; memoizing per render means a parent
       shared by K drawables is composed once instead of K times. Bumping
       here (not only in jce_scene_update) keeps edit-mode renders — which
       never call jce_scene_update — reading transforms as edited this frame
       rather than a stale cached pose. */
    jce_scene_invalidate_world_cache(scene);

    /* Label views for GPU profilers (RenderDoc / bgfx debug overlay).
     * bgfx accepts repeat sets; names persist for the lifetime of the
     * view ID, so this is effectively cheap. */
    bgfx_set_view_name(view_id_base,                           "Scene/Color",        INT32_MAX);
    bgfx_set_view_name((uint16_t)(view_id_base + 10),          "Scene/ShadowSimple", INT32_MAX);
    for (uint16_t c = 0; c < JCE_CSM_MAX_CASCADES; ++c) {
        char nm[32];
        snprintf(nm, sizeof(nm), "Scene/ShadowCSM%u", (unsigned)c);
        bgfx_set_view_name((uint16_t)(view_id_base + 11 + c), nm, INT32_MAX);
    }

    JceSceneRenderConfig defcfg = jce_scene_render_config_default();
    const JceSceneRenderConfig *cfg = config ? config : &defcfg;
    const JceSceneRenderingSettings *scene_rendering =
        jce_scene_get_rendering_settings(scene);

    sr->frame_shadow_active = false; sr->frame_shadow_vp_valid = false;
    sr->shadow_use_csm = false; sr->last_csm_valid = false;

    /* Apply optional config overrides. */
    if (cfg->shadow_map_size != 0) {
        sr_ensure_shadow_map_size(sr, cfg->shadow_map_size);
    } else if (scene_rendering && scene_rendering->shadow_resolution > 0) {
        sr_ensure_shadow_map_size(sr,
                                  (uint16_t)scene_rendering->shadow_resolution);
    }
    if (cfg->csm_cascades != 0) {
        sr->csm_cascade_count =
            cfg->csm_cascades < JCE_CSM_MAX_CASCADES
            ? cfg->csm_cascades : JCE_CSM_MAX_CASCADES;
    } else if (scene_rendering && scene_rendering->cascade_count > 0) {
        uint8_t cascades = (uint8_t)scene_rendering->cascade_count;
        sr->csm_cascade_count =
            cascades < JCE_CSM_MAX_CASCADES ? cascades : JCE_CSM_MAX_CASCADES;
    }

    sr_apply_view_order(view_id_base, cfg, sr->csm_cascade_count);

    /* Ensure wireframe is OFF before sky draws (sky's fullscreen quad must
     * render solid). The previous frame may have left it ON. Editor mode
     * only — runtime games own their own wireframe state. */
    if (sr->has_cbs && sr->cbs.load_texture)
        jce_renderer_set_wireframe(sr->renderer, false);

    /* Forward the editor's selected view mode to the PBR shader.
     *   SHADED              → 0 (lit + textured)
     *   WIREFRAME            → 1 (host fills the wireframe pass; shader
     *                            still treated as shaded for the few
     *                            entities that hit the PBR path)
     *   TEXTURED             → 2 (unlit albedo only — raw base color)
     *   WIREFRAME_TEXTURED   → 3 (unlit albedo + host wireframe overlay)
     * Previously this was hard-coded to 0, which made TEXTURED look
     * identical to SHADED. */
    int sm = (int)cfg->view_mode;
    if (sm < 0) sm = 0;
    if (sm > 3) sm = 0;
    jce_pbr_material_set_view_mode(sm);

    /* Scene rendering settings own PostFX defaults; render config remains
     * the fallback for callers that render scenes without authored settings. */
    JcePostFXParams active_postfx = cfg->postfx;
    if (scene_rendering) {
        active_postfx.exposure =
            scene_rendering->exposure;
        active_postfx.gamma =
            scene_rendering->gamma;
        active_postfx.bloom_threshold =
            scene_rendering->bloom_threshold;
        active_postfx.bloom_intensity =
            scene_rendering->bloom_intensity;
        active_postfx.fxaa_span_max =
            scene_rendering->fxaa_span_max;
        active_postfx.vignette_intensity =
            scene_rendering->vignette_intensity;
        active_postfx.vignette_smoothness =
            scene_rendering->vignette_smoothness;
        active_postfx.chromatic_strength =
            scene_rendering->chromatic_strength;

        if (sr->postfx_pipeline) {
            for (int i = 0; i < JCE_POSTFX_COUNT &&
                 i < JCE_SCENE_RENDERING_POSTFX_COUNT; i++) {
                jce_postfx_enable(sr->postfx_pipeline,
                                  (JcePostFXType)i,
                                  scene_rendering->postfx_enabled[i]);
            }
            /* Data-driven custom post pass (engine stays style-agnostic). */
            jce_postfx_set_custom_shader(sr->postfx_pipeline,
                                         scene_rendering->custom_post_shader,
                                         scene_rendering->custom_post_needs_depth);
            jce_postfx_set_custom_params(sr->postfx_pipeline,
                                         scene_rendering->custom_post_params,
                                         scene_rendering->custom_post_param_count);
        }
    }

    /* Blend active Volume components into postfx params before pushing. */
    if (camera) {
        jce_vec3 cp = jce_camera_get_position(camera);
        jce_volume_system_tick(scene, cp, &active_postfx);
    }

    /* Push postfx params. */
    if (sr->postfx_pipeline)
        jce_postfx_set_params(sr->postfx_pipeline, &active_postfx);

    sr->postfx_tonemap_active = false;
    if (sr->postfx_pipeline)
        sr->postfx_tonemap_active =
            jce_postfx_is_enabled(sr->postfx_pipeline, JCE_POSTFX_TONEMAP);

    /* Time-of-day driver (P2-weather-decals-tod): advance the day clock and
     * push the lighting snapshot into sr->tod_state BEFORE the sky + lighting
     * passes consume it.  No-op (and releases any override) when disabled. */
    if (scene_rendering)
        sr_drive_time_of_day(sr, scene_rendering, dt_sec);

    /* Collect entities. */
    EntityList list;
    list.count = 0;
    jce_scene_each_entity(scene, collect_entity_cb, &list);

    /* Skybox scan + IBL refresh. */
    sr_scan_skybox(sr, scene, &list);

    /* Sky pass into the main view.
     * Mirrors 0.5.7 gating: in plain WIREFRAME mode skip sky entirely;
     * in WIREFRAME_TEXTURED skip sky unless an HDR skybox is active. */
    bool sky_drawn = false;
    if (cfg->draw_skybox) {
        bool gate_ok = true;
        if (cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME)
            gate_ok = false;
        else if (cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME_TEXTURED
                 && !sr->skybox_active)
            gate_ok = false;
        if (gate_ok) {
            sr_draw_sky_gradient(sr, view_id_base);
            sky_drawn = true;
        }
    }

    /* Editor overlay hook: invoked between sky and entities. Used by
     * the editor to draw the world grid behind scene geometry, matching
     * 0.5.7 ordering. Runtime games leave on_after_sky == NULL. */
    if (cfg->on_after_sky)
        cfg->on_after_sky(view_id_base, sky_drawn, cfg->on_after_sky_ud);

    /* Skinned animation: advance every skeletal pose ONCE here, before the
       shadow pass records draws, caching each world-bone palette so the
       shadow and color passes share the identical pose (no double-advance). */
    sr_update_skinned_anims(sr, scene, &list, dt_sec);

    /* 2D sprite animation: advance every SpriteAnimator frame time ONCE here;
       the entity draw loop consumes the cached player's current frame. */
    sr_update_sprite_anims(sr, scene, &list, dt_sec);

    /* Shadow passes (do them BEFORE entity pass so PBR can sample). */
    if (cfg->draw_shadows) {
        /* Determine viewport from camera bounds — if absent, assume 16:9. */
        uint32_t vp_w = cfg->viewport_width ? cfg->viewport_width : 1920, vp_h = cfg->viewport_height ? cfg->viewport_height : 1080;
        float shadow_distance = cfg->shadow_distance;
        float split_lambda = cfg->csm_split_lambda;
        if (shadow_distance <= 0.0f && scene_rendering)
            shadow_distance = scene_rendering->shadow_distance;
        if (split_lambda < 0.0f)
            split_lambda = scene_rendering ? scene_rendering->split_lambda : 0.5f;
        sr_draw_shadow_pass(sr, scene, camera, &list, view_id_base,
                            vp_w, vp_h, shadow_distance, split_lambda);
        /* P1 — local (spot) shadow atlas producer, after the directional/CSM
           pass and before the entity/color pass that samples it. */
        sr_draw_local_shadow_pass(sr, scene, &list, view_id_base);
    } else {
        sr->shadow_use_csm = false; sr->last_csm_valid = false;
        sr->frame_local_active = false;
    }

    /* Apply view-mode wireframe via the renderer. CRITICAL: this MUST come
     * AFTER the sky and shadow passes, otherwise the sky's fullscreen quad
     * would be rendered as wireframe lines (looks like the sky is broken).
     * Mirrors 0.5.7 ordering exactly. */
    bool editor_wf_set = false;
    if (sr->has_cbs && sr->cbs.load_texture) {
        bool want_wf = (cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME ||
                        cfg->view_mode == JCE_SCENE_VIEW_WIREFRAME_TEXTURED);
        jce_renderer_set_wireframe(sr->renderer, want_wf);
        editor_wf_set = want_wf;
    }

    /* Entity rendering. */
    sr_draw_entities(sr, scene, camera, &list, view_id_base, dt_sec, cfg);

    /* Cloth/soft-body grids (after entities so lighting uniforms are live). */
    sr_draw_cloth(sr, scene, view_id_base);

    /* Reset wireframe so subsequent overlay passes (grid, selection) draw
     * solid. Mirrors 0.5.7 line 914 exactly. bgfx_set_debug() takes effect
     * per-submission, so submits between set(true) and set(false) draw as
     * wireframe; submits after set(false) draw solid. */
    if (editor_wf_set)
        jce_renderer_set_wireframe(sr->renderer, false);

    /* Particle emitters: draw alive particles from the scene-owned
     * JceParticleSystem (simulated each runtime step by
     * jce_scene_particles_update) as depth-tested debug billboards. */
    sr_draw_particles(sr, scene, view_id_base);

    /* Decals (P2-weather-decals-tod): depth-tested projected quads from
     * authored JceDecalComponent projectors + runtime-stamped decals.  Drawn
     * after opaque geometry so they composite over the surfaces they hug. */
    sr_drive_decals(sr, scene, view_id_base, dt_sec);

    /* Weather overlay (P2-weather-decals-tod): screen-space rain/snow driven
     * by the scene's weather settings.  Drawn last (after the scene + decals)
     * so the precipitation layers over everything in the color view. */
    if (scene_rendering)
        sr_drive_weather(sr, scene_rendering, view_id_base, dt_sec);

    /* PostFX pass — engine-level postfx is currently a no-op since we
       don't own an output FBO here. The caller drives final composition.
       We expose tonemap state through the IBL params binding so PBR
       output remains consistent. */
    (void)cfg->apply_postfx;

    /* ── Volumetric fog (Stage 1 wiring) ──────────────────────────────
     * Renders fog into a private RT.  Composite into the caller's color
     * RT is a deferred Stage 2 (composite shader pending).  The result
     * texture handle is exposed via jce_scene_renderer_get_fog_result_texture()
     * so callers can consume it once the composite pass is in place. */
    sr->vfog_last_rendered = false;
    if (cfg->fog_enabled
        && jce_render_pipeline_is_feature_enabled("volumetric_fog")
        && cfg->fog_depth_tex_handle != UINT16_MAX
        && cfg->fog_rt_width > 0 && cfg->fog_rt_height > 0
        && sr->pak)
    {
        if (!sr->vfog) {
            JceVolumetricFogDesc d = { sr->pak, cfg->fog_rt_width, cfg->fog_rt_height };
            sr->vfog   = jce_volumetric_fog_create(&d);
            sr->vfog_w = cfg->fog_rt_width;
            sr->vfog_h = cfg->fog_rt_height;
        } else if (sr->vfog_w != cfg->fog_rt_width
                || sr->vfog_h != cfg->fog_rt_height) {
            jce_volumetric_fog_resize(sr->vfog, cfg->fog_rt_width, cfg->fog_rt_height);
            sr->vfog_w = cfg->fog_rt_width;
            sr->vfog_h = cfg->fog_rt_height;
        }
        if (sr->vfog) {
            JceVolumetricFogParams p = cfg->fog;
            p.near_plane = camera ? jce_camera_get_near(camera) : 0.1f;
            p.far_plane  = camera ? jce_camera_get_far(camera)  : 200.0f;
            jce_volumetric_fog_set_params(sr->vfog, &p);

            const jce_mat4 vmat = camera ? jce_camera_view(camera) : jce_m4_identity();
            const float aspect  = (float)cfg->fog_rt_width / (float)cfg->fog_rt_height;
            const jce_mat4 pmat = camera
                ? jce_camera_proj(camera, aspect, sr->homogeneous_depth)
                : jce_m4_identity();

            jce_volumetric_fog_render(sr->vfog,
                                      cfg->fog_depth_tex_handle,
                                      &vmat, &pmat,
                                      (uint16_t)(view_id_base + 15));
            sr->vfog_last_rendered = true;
        }
    }

    JCE_PROFILE_ZONE_END;
    return view_id_base;
}

const JceCsmData *jce_scene_renderer_get_csm(const JceSceneRenderer *sr)
{
    if (!sr || !sr->last_csm_valid) return NULL;
    return &sr->last_csm;
}

JcePostFXPipeline *jce_scene_renderer_get_postfx(JceSceneRenderer *sr)
{
    return sr ? sr->postfx_pipeline : NULL;
}

uint16_t jce_scene_renderer_get_fog_result_texture(const JceSceneRenderer *sr)
{
    if (!sr || !sr->vfog || !sr->vfog_last_rendered) return UINT16_MAX;
    return jce_volumetric_fog_get_result_texture(sr->vfog);
}

void jce_scene_renderer_composite_fog(JceSceneRenderer *sr, uint16_t view_id,
                                      uint16_t dst_fb_idx)
{
    if (!sr || !sr->vfog || !sr->vfog_last_rendered) return;
    jce_volumetric_fog_composite(sr->vfog, view_id, dst_fb_idx);
}

bool jce_scene_renderer_is_skybox_active(const JceSceneRenderer *sr)
{
    return sr ? sr->skybox_active : false;
}

void jce_scene_renderer_get_cull_stats(const JceSceneRenderer *sr,
                                        JceSceneCullStats *out)
{
    if (!out) return;
    if (!sr) {
        out->total = out->visible = out->culled = 0;
        out->enabled = false;
        return;
    }
    out->total   = sr->stat_total_entities;
    out->visible = sr->stat_visible_entities;
    out->culled  = sr->stat_culled_entities;
    out->enabled = sr->stat_culling_enabled;
}

void jce_scene_renderer_set_global_lod(JceSceneRenderer *sr,
                                        const JceLodGroup *group)
{
    if (!sr) return;
    if (sr->global_lod != group)
        memset(sr->lod_prev, 0, sizeof(sr->lod_prev));
    sr->global_lod = group;
}

void jce_scene_renderer_set_anim_sm_active(JceSceneRenderer *sr, bool active)
{
    if (sr) sr->anim_sm_active = active;
}

JceMesh *jce_scene_renderer_get_builtin_mesh(JceSceneRenderer *sr, int shape)
{
    if (!sr) return NULL;
    switch (shape) {
    case 0: return sr->cube_mesh;
    case 1: return sr->sphere_mesh;
    case 2: return sr->plane_mesh;
    case 3: return sr->capsule_mesh;
    case 4: return sr->cylinder_mesh;
    default: return NULL;
    }
}

void jce_scene_renderer_invalidate_terrain(JceSceneRenderer *sr,
                                            const char *path)
{
    if (!sr) return;
    for (int i = 0; i < 16; i++) {
        if (!sr->terrain_cache[i].used) continue;
        if (path && *path &&
            strncmp(sr->terrain_cache[i].path, path,
                    sizeof sr->terrain_cache[i].path) != 0)
            continue;
        sr_terrain_free_chunks(sr, i);
        if (sr->terrain_cache[i].terrain) jce_terrain_free(sr->terrain_cache[i].terrain);
        if (BGFX_HANDLE_IS_VALID(sr->terrain_cache[i].splat_tex))
            bgfx_destroy_texture(sr->terrain_cache[i].splat_tex);
        memset(&sr->terrain_cache[i], 0, sizeof sr->terrain_cache[i]);
    }
}

void jce_scene_renderer_get_lod_stats(const JceSceneRenderer *sr,
                                       JceSceneLodStats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!sr) return;
    for (int i = 0; i < JCE_LOD_MAX_LEVELS; i++)
        out->picks[i] = sr->stat_lod_picks[i];
    out->culled      = sr->stat_lod_culled;
    out->enabled     = sr->stat_lod_enabled;
    out->level_count = sr->global_lod ? sr->global_lod->count : 0;
}

void jce_scene_renderer_get_rq_stats(const JceSceneRenderer *sr,
                                      JceSceneRqStats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!sr) return;
    out->commands_in     = sr->stat_rq.commands_in;
    out->submits_out     = sr->stat_rq.submits_out;
    out->batches_merged  = sr->stat_rq.batches_merged;
    out->instances_total = sr->stat_rq.instances_total;
    out->enabled         = sr->stat_rq_active;
}

void jce_scene_renderer_get_occlusion_stats(const JceSceneRenderer *sr,
                                             JceSceneOcclusionStats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!sr) return;
    out->total    = sr->stat_occlusion.total_entities;
    out->visible  = sr->stat_occlusion.visible;
    out->occluded = sr->stat_occlusion.occluded;
    out->warm_up  = sr->stat_occlusion.warm_up;
    out->enabled  = sr->stat_occlusion_enabled;
}

void jce_scene_renderer_set_time_of_day(JceSceneRenderer        *sr,
                                         const JceTimeOfDayState *state)
{
    if (!sr) return;
    if (state) {
        sr->tod_state  = *state;
        sr->tod_active = true;
    } else {
        sr->tod_active = false;
    }
}

const JceTimeOfDayState *jce_scene_renderer_get_time_of_day(
    const JceSceneRenderer *sr)
{
    if (!sr || !sr->tod_active) return NULL;
    return &sr->tod_state;
}

void jce_scene_renderer_set_ambient_override(JceSceneRenderer *sr,
                                              const float       color_rgb[3],
                                              float             intensity)
{
    if (!sr) return;
    if (color_rgb) {
        sr->ambient_override_color     = jce_v3(color_rgb[0], color_rgb[1], color_rgb[2]);
        sr->ambient_override_intensity = intensity;
        sr->ambient_override_active    = true;
    } else {
        sr->ambient_override_active    = false;
    }
}

bool jce_scene_renderer_spawn_decal(JceSceneRenderer    *sr,
                                    const JceDecalSpawn *spawn)
{
    if (!sr || !spawn || !sr->pak) return false;
    if (!sr->decals) {
        JceDecalPoolDesc d = { 1024u, sr->pak };
        sr->decals = jce_decals_create(&d);
        if (!sr->decals) return false;   /* shader missing — fail soft */
    }
    return jce_decals_spawn(sr->decals, spawn);
}
