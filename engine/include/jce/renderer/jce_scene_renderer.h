/*
 * jce_scene_renderer.h  Engine-owned scene rendering.
 *
 * Single source of truth for rendering a JceScene. Both editor and
 * game consume this; editor adds overlay passes on top.
 *
 * Layer: Render (Layer 4).
 */

#ifndef JCE_SCENE_RENDERER_H
#define JCE_SCENE_RENDERER_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_csm.h>
#include <jce/renderer/jce_occlusion_culler.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_texture_types.h>
#include <jce/renderer/jce_time_of_day.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceRenderer       JceRenderer;
typedef struct JceCamera         JceCamera;
typedef struct JceScene          JceScene;
typedef struct JcePakArchive     JcePakArchive;
typedef struct JceSceneRenderer  JceSceneRenderer;
typedef struct JceLodGroup       JceLodGroup;

/* Mirror of middleware's JCE_LOD_MAX_LEVELS — kept local so the
 * renderer's public header doesn't have to pull middleware/scene/.
 * The implementation file _Static_assert()s these stay in sync. */
#define JCE_SCENE_LOD_MAX_LEVELS 8

/* ── Render configuration (passed per-frame) ──────────────────────── */

typedef enum {
    JCE_SCENE_VIEW_SHADED = 0,
    JCE_SCENE_VIEW_WIREFRAME,
    JCE_SCENE_VIEW_TEXTURED,            /* unlit albedo-only (TODO) */
    JCE_SCENE_VIEW_WIREFRAME_TEXTURED,
} JceSceneViewModeKind;

/* Callback invoked between sky pass and entity pass. Editor uses this
 * to inject overlay passes (e.g. grid) that must render BEHIND scene
 * entities. Mirrors 0.5.7 ordering: sky → grid → entities → outlines.
 *   view_id   : the bgfx view ID where sky was submitted (== view_id_base)
 *   sky_drawn : true if sky was actually drawn (gated by view mode)
 *   ud        : user data passed in config */
typedef void (*JceSceneOnAfterSkyFn)(uint16_t view_id, bool sky_drawn, void *ud);

typedef struct {
    bool                 draw_skybox;
    bool                 draw_shadows;
    bool                 draw_opaque;
    bool                 draw_sprites;
    bool                 draw_transparent;
    bool                 apply_postfx;
    JcePostFXParams      postfx;
    uint16_t             shadow_map_size;  /* 0 = use default (2048) */
    uint8_t              csm_cascades;     /* 0 = use default (4)   */
    JceSceneViewModeKind view_mode;        /* default 0 = shaded   */

    /* Broadphase frustum culling using the spatial grid. Approximate AABBs
     * are derived from each entity's transform position + scale (a precise
     * mesh AABB hookup is a future-work item). Default OFF — opt in when
     * scenes grow large enough to benefit from the index. */
    bool                 frustum_culling;

    /* GPU-query occlusion culling (two-pass coherence-based).
     * Set to a valid JceOcclusionCuller instance to enable; NULL disables.
     * When active, entities occluded in the previous frame are skipped
     * and a depth-only proxy is submitted for the current frame instead.
     * Falls back to always-visible if hardware queries are unsupported.
     * The caller owns the culler lifetime. */
    JceOcclusionCuller  *occlusion_culler;

    /* Overlay hook (editor-only). NULL in runtime games. */
    JceSceneOnAfterSkyFn on_after_sky;
    void                *on_after_sky_ud;
} JceSceneRenderConfig;

/* Returns true if a skybox component is currently active in the scene
 * (i.e. an HDR environment is loaded). Updated each render call by
 * the internal scan. Used by editor to mirror 0.5.7 grid-gating logic. */
JCE_API bool jce_scene_renderer_is_skybox_active(const JceSceneRenderer *sr);

/* Return a sensible default configuration (all features on). */
JCE_API JceSceneRenderConfig jce_scene_render_config_default(void);

/* ── Asset resolution callbacks (optional) ────────────────────────── */

typedef struct JceMesh  JceMesh;
typedef struct JceModel JceModel;

typedef struct {
    JceMesh    *(*load_mesh)(const char *path, void *ud);
    JceModel   *(*load_model)(const char *path, void *ud);
    /* Returns a texture with idx==UINT16_MAX on failure. `material_path`
     * is the primary lookup key (component's albedoTex or .mat.json). If
     * both paths are NULL/empty the call fails. `mesh_path` is an
     * OPTIONAL fallback hint used by the asset cache to derive the
     * texture from a mesh sidecar (e.g. .obj's MTL map_Kd) when the
     * material lookup misses; it must be merged into the SAME cache
     * entry as `material_path` so we don't get duplicate async resolves
     * for the same logical texture (0.5.5 semantics). */
    JceTexture  (*load_texture)(const char *material_path,
                                const char *mesh_path,
                                void       *ud);
    void        *userdata;
} JceSceneRendererCallbacks;

/* ── Lifecycle ────────────────────────────────────────────────────── */

/*
 * Create a scene renderer.
 *   renderer : the engine renderer (for shader programs, texture binding).
 *   pak      : PAK archive for loading shaders and fallback assets.
 *   cbs      : optional asset callbacks (NULL = use PAK only).
 */
JceSceneRenderer *jce_scene_renderer_create(
    JceRenderer           *renderer,
    const JcePakArchive   *pak,
    const JceSceneRendererCallbacks *cbs);

JCE_API void jce_scene_renderer_destroy(JceSceneRenderer *sr);

/* ── Per-frame rendering ──────────────────────────────────────────── */

/*
 * Render the scene from the given camera into bgfx views starting at
 * view_id_base.  Shadow cascades use view_id_base+10..+13.
 *
 * dt_sec : frame delta time in seconds (drives skeletal animation).
 *
 * Returns the view ID into which the final colour was written, so the
 * caller can render overlays into the NEXT view.
 *
 * Decision: animation dt is caller-provided (not internal timer) so
 * editor and runtime can each provide their own frame timing.
 */
uint16_t jce_scene_renderer_render(
    JceSceneRenderer           *sr,
    JceScene                   *scene,
    const JceCamera            *camera,
    uint16_t                    view_id_base,
    float                       dt_sec,
    const JceSceneRenderConfig *config);

/* ── Accessors ────────────────────────────────────────────────────── */

/* Returns the CSM data from the most recent render (for overlay shadow
   queries).  Returns NULL if shadows were not rendered. */
JCE_API const JceCsmData *jce_scene_renderer_get_csm(const JceSceneRenderer *sr);

/* Returns the engine-owned PostFX pipeline. Editor and runtime use this
   single instance — no separate global. Returns NULL before create(). */
JCE_API JcePostFXPipeline *jce_scene_renderer_get_postfx(JceSceneRenderer *sr);

/* Per-frame culling stats from the most recent render call. */
typedef struct {
    uint32_t total;     /* entities collected this frame */
    uint32_t visible;   /* entities that passed frustum culling */
    uint32_t culled;    /* entities removed by culling (== total - visible) */
    bool     enabled;   /* whether culling was active this frame */
} JceSceneCullStats;

JCE_API void jce_scene_renderer_get_cull_stats(const JceSceneRenderer *sr,
                                                JceSceneCullStats *out);

/* Optional global LOD group: when set (non-NULL) every entity that has
 * a resolved mesh has its mesh substituted by jce_lod_pick() based on
 * camera distance. Pass NULL to disable. The pointer is borrowed; the
 * caller must keep the group alive for as long as it is set. */
JCE_API void jce_scene_renderer_set_global_lod(JceSceneRenderer *sr,
                                                const JceLodGroup *group);

/* Access built-in primitive meshes the renderer creates internally:
 * 0=cube, 1=sphere, 2=plane, 3=capsule, 4=cylinder.
 * Returns NULL for invalid shape or if the renderer is not initialised.
 * Useful for building demo LOD groups without loading external assets. */
JCE_API JceMesh *jce_scene_renderer_get_builtin_mesh(JceSceneRenderer *sr,
                                                     int shape);

/* Per-frame LOD pick stats. picks[i] = number of entities drawn at level i. */
typedef struct {
    uint32_t picks[JCE_SCENE_LOD_MAX_LEVELS];
    uint32_t culled;     /* entities the LOD pick reported as past last threshold */
    int      level_count;/* group->count or 0 if no group bound */
    bool     enabled;    /* whether a global LOD group was bound this frame */
} JceSceneLodStats;

JCE_API void jce_scene_renderer_get_lod_stats(const JceSceneRenderer *sr,
                                               JceSceneLodStats *out);

/* Per-frame render-queue stats (sum of shadow + main mesh flushes).
 * `enabled` indicates whether the queue path was taken this frame. */
typedef struct {
    uint32_t commands_in;     /* draw commands fed into flush(es) */
    uint32_t submits_out;     /* actual bgfx_submit calls */
    uint32_t batches_merged;  /* number of auto-merged instance batches */
    uint32_t instances_total; /* total instances across merged batches */
    bool     enabled;
} JceSceneRqStats;

JCE_API void jce_scene_renderer_get_rq_stats(const JceSceneRenderer *sr,
                                              JceSceneRqStats *out);

/* Per-frame occlusion culling stats.
 * Only meaningful when an occlusion culler was bound in config. */
typedef struct {
    uint32_t total;     /* entities tested against the culler */
    uint32_t visible;   /* passed (query returned > min_pixels) */
    uint32_t occluded;  /* skipped (fully occluded last frame) */
    uint32_t warm_up;   /* first frame for entity, always drawn */
    bool     enabled;
} JceSceneOcclusionStats;

JCE_API void jce_scene_renderer_get_occlusion_stats(const JceSceneRenderer *sr,
                                                     JceSceneOcclusionStats *out);

/* ── Time-of-day override ─────────────────────────────────────────── */
/*
 * When set, the renderer uses the snapshot's sky_top/horizon/ground for
 * the procedural-gradient sky pass and `sun_direction` as the implicit
 * directional light when no JceDirLightComponent is present in the scene
 * (also used as the shadow-cascade light vector).
 *
 * Pass NULL to clear the override and restore the renderer's hardcoded
 * defaults.  The struct is copied; caller need not keep it alive.
 */
JCE_API void jce_scene_renderer_set_time_of_day(JceSceneRenderer        *sr,
                                                 const JceTimeOfDayState *state);

/* Returns the active ToD snapshot (may be NULL if no override). */
JCE_API const JceTimeOfDayState *jce_scene_renderer_get_time_of_day(
    const JceSceneRenderer *sr);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_RENDERER_H */
