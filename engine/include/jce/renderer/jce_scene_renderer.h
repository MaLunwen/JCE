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
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_texture_types.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceRenderer       JceRenderer;
typedef struct JceCamera         JceCamera;
typedef struct JceScene          JceScene;
typedef struct JcePakArchive     JcePakArchive;
typedef struct JceSceneRenderer  JceSceneRenderer;

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

JCE_EXTERN_C_END

#endif /* JCE_SCENE_RENDERER_H */
