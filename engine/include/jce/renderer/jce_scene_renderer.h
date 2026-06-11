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
#include <jce/renderer/jce_decals.h>
#include <jce/middleware/world/jce_time_of_day.h>
#include <jce/renderer/jce_volumetric_fog.h>

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

/* Scene view mode (editor + runtime). The sign of u_normalScale.z in
 * the PBR shader carries this value so a single fragment shader handles
 * all four variants (see engine/shaders/pbr/fs_pbr.sc lines 329-339).
 *
 *   SHADED              — Textured + full PBR lighting (directional /
 *                         point / spot lights, CSM shadows, IBL,
 *                         AO, fog, post-fx). Default editor view; the
 *                         "what the shipping build will look like"
 *                         preview.
 *   WIREFRAME           — Lines only, no fill, no lighting, no
 *                         textures. Topology / mesh-density inspection.
 *   TEXTURED            — Textured but UNLIT: shader emits the gamma-
 *                         corrected raw albedo (`pow(albedo, 1/2.2)`)
 *                         and returns before the lighting pipeline.
 *                         Used to verify base-color textures and UVs
 *                         independent of scene lighting / exposure.
 *   WIREFRAME_TEXTURED  — Unlit textured fill + line overlay. Combined
 *                         UV + topology check.
 *
 * Texture loading: ALL view modes load textures (including SHADED).
 * If the albedo handle is invalid (in-flight async load OR resolution
 * failed), the renderer flags `use_checker_fallback` so the shader
 * substitutes a triplanar pink/black checker — never a flat white
 * "loading" surface. See jce_scene_renderer.c around the
 * `use_checker_fallback` block. */
typedef enum {
    JCE_SCENE_VIEW_SHADED = 0,           /* textured + lit (full PBR) */
    JCE_SCENE_VIEW_WIREFRAME,            /* lines only, unlit */
    JCE_SCENE_VIEW_TEXTURED,             /* textured but UNLIT (raw albedo) */
    JCE_SCENE_VIEW_WIREFRAME_TEXTURED,   /* unlit textured + line overlay */
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
    float                shadow_distance;  /* 0 = scene/default     */
    float                csm_split_lambda; /* <0 = scene/default    */
    JceSceneViewModeKind view_mode;        /* default 0 = shaded   */
    uint32_t             viewport_width;   /* 0 = fallback 16:9    */
    uint32_t             viewport_height;  /* 0 = fallback 16:9    */

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

    /* Volumetric fog (analytic + raymarched).  When fog_enabled is true
     * AND fog_depth_tex_handle is a valid bgfx texture id (the depth tex
     * matching this frame's view+proj), the renderer will allocate /
     * resize a JceVolumetricFog instance, push fog params, and execute
     * the fog raymarch into a private RT.  The result texture handle is
     * exposed via jce_scene_renderer_get_fog_result_texture() so callers
     * can composite it (multiply scene by alpha, add rgb) themselves.
     * The renderer does NOT composite into the caller's color RT yet —
     * that is a separate phase (composite shader pending). */
    bool                     fog_enabled;
    JceVolumetricFogParams   fog;
    uint16_t                 fog_depth_tex_handle;  /* UINT16_MAX = none */
    int                      fog_rt_width;          /* 0 = skip fog */
    int                      fog_rt_height;         /* 0 = skip fog */
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
    /* OPTIONAL. Returns true ONLY if a texture lookup has been attempted
     * and is known to have permanently failed (asset missing, decode
     * error, etc.). Returns false while the asset is still being
     * resolved/loaded asynchronously. The renderer uses this to suppress
     * the magenta/yellow "missing texture" checker during the brief
     * in-flight window after a scene loads — without it, every textured
     * entity briefly flashes white → pink-checker → final texture as
     * each async load completes. */
    bool        (*texture_failed)(const char *material_path,
                                  const char *mesh_path,
                                  void       *ud);
    /* OPTIONAL. Resolve an asset-relative path (as written in scene JSON)
     * to a path that jce_fs_host_read_all can open.  Used for engine-side
     * file loads that don't go through the load_mesh/load_model/load_texture
     * paths (terrain meta JSON, terrain bin, etc).  Return true if the
     * input was successfully resolved into out (NUL-terminated).  Return
     * false to leave the engine using the path as-is. */
    bool        (*resolve_path)(const char *in, char *out, int outsz,
                                void *ud);
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

/* Most-recent volumetric fog result texture (RGBA8: rgb in-scatter,
 * a transmittance).  Returns UINT16_MAX when fog was disabled this
 * frame, params/depth were invalid, or fog has never been rendered.
 * Caller composites:  final = scene.rgb * a + rgb. */
JCE_API uint16_t jce_scene_renderer_get_fog_result_texture(const JceSceneRenderer *sr);

/* Composite the most recent volumetric fog RT into the currently bound
 * frame buffer of `view_id` using blend ONE/SRC_ALPHA.  Caller must
 * configure the view's frame buffer / viewport BEFORE calling — the
 * fog quad is fullscreen NDC so no camera transform is required.
 *
 * No-op when fog was disabled this frame, the composite shader is
 * unavailable on this backend, or the renderer has never run. */
JCE_API void jce_scene_renderer_composite_fog(JceSceneRenderer *sr, uint16_t view_id,
                                              uint16_t dst_fb_idx);

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

/* When active (Play), a bound animation state machine drives active_clip;
 * when inactive (editor preview, the default), the SM stays idle so manual
 * clip selection previews normally. The editor flips this on play start/stop. */
JCE_API void jce_scene_renderer_set_anim_sm_active(JceSceneRenderer *sr,
                                                   bool active);

/* Access built-in primitive meshes the renderer creates internally:
 * 0=cube, 1=sphere, 2=plane, 3=capsule, 4=cylinder.
 * Returns NULL for invalid shape or if the renderer is not initialised.
 * Useful for building demo LOD groups without loading external assets. */
JCE_API JceMesh *jce_scene_renderer_get_builtin_mesh(JceSceneRenderer *sr,
                                                     int shape);

/* Drop the cached terrain mesh for `path` (or all cached terrains if
 * path is NULL/empty) so the next frame re-loads from disk. Tools
 * (Terrain panel) call this after Save so Scene View reflects edits. */
JCE_API void jce_scene_renderer_invalidate_terrain(JceSceneRenderer *sr,
                                                    const char *path);

/* Drop the cached tilemap (chunk meshes + map/tileset assets) for `path`
 * (or all cached tilemaps if path is NULL/empty) so the next frame
 * re-loads from disk. Tools (Tile Palette panel) call this after Save. */
JCE_API void jce_scene_renderer_invalidate_tilemap(JceSceneRenderer *sr,
                                                    const char *path);

/* Forward decls for accessors below — full headers may not be in this TU. */
struct JceAnimPlayer;
struct JceModel;

/* Look up the live JceAnimPlayer the renderer is driving for a given
 * skeleton/model path.  Returns NULL if the model has not yet been seen
 * by the renderer or if it has no skeletal animation.  Editors and tools
 * use this to read the currently-playing time / state without keeping a
 * second cache of their own (which would never advance). */
JCE_API struct JceAnimPlayer *jce_scene_renderer_get_anim_player(
    JceSceneRenderer *sr, const char *skeleton_path);

/* Live state-machine binding for an entity's skeletal animator (created once
 * its sm_path is set). NULL until the SM has been bound. Gameplay/editor code
 * drives transitions through it:
 *   JceAnimSmBinding *b = jce_scene_renderer_get_anim_sm(sr, (uint32_t)e);
 *   if (b) jce_anim_sm_binding_set_float(b, "Speed", v); */
JCE_API struct JceAnimSmBinding *jce_scene_renderer_get_anim_sm(
    JceSceneRenderer *sr, uint32_t entity);

/* Look up the JceModel cached by the renderer for a given path. */
JCE_API struct JceModel *jce_scene_renderer_get_model(
    JceSceneRenderer *sr, const char *skeleton_path);

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

/* Editor-side ambient override.
 * Pass color_rgb=NULL to disable and restore the renderer's hardcoded
 * (1,1,1) * 0.15 ambient. ToD, when active, still wins over this. */
JCE_API void jce_scene_renderer_set_ambient_override(JceSceneRenderer *sr,
                                                      const float       color_rgb[3],
                                                      float             intensity);

/* Runtime decal stamping (P2-weather-decals-tod).
 *
 * Stamp a decal (bullet hole, blood splatter, scorch mark, footprint, …)
 * onto a world surface.  The renderer owns a lazily-created decal pool that
 * is updated + rendered every jce_scene_renderer_render() into the color
 * view; spawned decals respect their JceDecalSpawn.lifetime_seconds (0 =
 * persistent until evicted).  This is independent of authored
 * JceDecalComponent projectors, which are rebuilt from the scene each frame.
 *
 * Returns true if the decal was stamped (false if the pool could not be
 * created — e.g. the decal shaders are missing — or the spawn was invalid). */
JCE_API bool jce_scene_renderer_spawn_decal(JceSceneRenderer    *sr,
                                            const JceDecalSpawn *spawn);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_RENDERER_H */
