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
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_csm.h>
#include <jce/renderer/jce_occlusion_culler.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_texture_types.h>
#include <jce/renderer/jce_decals.h>
#include <jce/middleware/animation/jce_anim_ik.h>   /* JceAnimEvent (POD) */
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
    /* Debug channel views (unlit; fs_pbr emits the raw material channel). */
    JCE_SCENE_VIEW_NORMALS,              /* 4: world normal as RGB (N*0.5+0.5) */
    JCE_SCENE_VIEW_ROUGHNESS,            /* 5: roughness grayscale */
    JCE_SCENE_VIEW_METALLIC,             /* 6: metallic grayscale */
    JCE_SCENE_VIEW_AO,                   /* 7: ambient-occlusion grayscale */
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
    /* Distinct viewport identity (default 0).  The editor passes a different id
     * per viewport (Game vs Scene) so each keeps its own previous-frame camera
     * for correct, order-independent TAA motion vectors through the one shared
     * renderer.  Single-viewport callers leave it 0. */
    int                  viewport_id;

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

    /* The bgfx framebuffer the SCENE COLOR + DEPTH pass renders into, as a raw
     * bgfx_frame_buffer_handle_t .idx.  The engine binds the occlusion culler's
     * depth-only proxy view to THIS framebuffer each frame so the proxy boxes'
     * depth test runs against the depth the color pass actually wrote.  Set it
     * to the SAME FBO you bound the color view to:
     *   - editor scene/game view : jce_offscreen_target_get_frame_buffer(bridge)
     *   - runtime with postfx     : jce_offscreen_target_get_frame_buffer(target)
     *   - runtime no postfx       : UINT16_MAX (backbuffer)
     * Default UINT16_MAX (set by jce_scene_render_config_default) = backbuffer,
     * which is correct for the direct-to-backbuffer path and harmless when no
     * occlusion culler is present.  Without this, offscreen paths' proxies test
     * a stale/empty backbuffer depth → occlusion inert or false-culling. */
    uint16_t             scene_frame_buffer;

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

    /* Screen-space reflections: the lit color RT the SSR pass reflects.  SSR's
     * ray-march view renders after the color pass, so this is the current
     * frame's color.  UINT16_MAX = no SSR this frame. */
    uint16_t                 ssr_color_tex_handle;

    /* ── Focus-bounded entity collection ("draw distance") ────────────
     * When cull_focus_enabled is true AND cull_radius > 0, the per-frame
     * entity collect skips any entity whose transform origin is farther
     * than cull_radius (HORIZONTAL / XZ distance) from the focus point
     * (cull_focus_x/y/z).  Because EVERY downstream pass (cull cache,
     * shadow caster loops, depth/velocity prepass, color pass) iterates
     * the collected list, bounding the collect makes them all O(near)
     * instead of O(all entities) — the fix that makes a full-loaded big
     * world playable.
     *
     * The collect adds a ~70m margin to the radius before squaring so a
     * large building whose authored origin sits just outside the radius
     * but whose footprint is inside is not dropped.  Entities with NO
     * transform are always collected (never culled by distance).
     *
     * Default (false / 0) → collect ALL entities exactly as before, so
     * memset'd configs and existing callers are byte-identical (NO
     * regression).  Shipped games are unaffected unless they opt in. */
    bool                     cull_focus_enabled;
    float                    cull_focus_x;
    float                    cull_focus_y;   /* unused for XZ cull; carried for completeness */
    float                    cull_focus_z;
    float                    cull_radius;    /* <= 0 treated as disabled */

    /* ── GPU-driven rendering (roadmap #18, Phase 0+1) ────────────────
     * When true AND the GPU exposes BGFX_CAPS_COMPUTE AND the cull compute
     * program loaded, the color-pass instanced batch is routed through a
     * persistent GPU "GPUScene" buffer + a compute frustum-cull dispatch
     * (cs_cull_frustum) that compacts the surviving per-instance world
     * matrices on the GPU, instead of the per-frame transient instance-data
     * buffer + CPU spatial-grid cull.  Only the OPAQUE COLOR pass is GPU-
     * driven; shadow / depth-prepass / velocity stay on the CPU instancing
     * path.  Any of those preconditions failing falls back to the exact CPU
     * path.
     *
     * Default (false) → byte-identical to the existing CPU instancing path
     * (sr_inst_flush → transient IDB).  The renderer also OR's in the
     * `r.gpu_driven` console cvar (default off), so a build/runtime can flip
     * it live without touching this field; either source turning it on
     * engages the GPU path. */
    bool                     gpu_driven;
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

/* ── Temporal Anti-Aliasing (TAA) driver ──────────────────────────────
 *
 * TAA needs the caller's help at two precise sites because the renderer
 * does NOT own the main colour-pass view transform (the caller sets it,
 * e.g. via jce_offscreen_target_prepare) nor the jce_postfx_apply() call.
 *
 * begin_frame: call it RIGHT BEFORE setting the main scene colour pass's
 *   view transform.  Pass the CLEAN (un-jittered) view + proj you were
 *   about to use.  When r.taa is ON it advances the jitter sequence, writes
 *   the JITTERED projection into *out_jittered_proj (use THAT for the colour
 *   pass), pushes the un-jittered inverse-view-proj + previous view*proj to
 *   the engine PostFX pipeline, and enables TAA on it.  Returns true iff TAA
 *   is active this frame (caller uses *out_jittered_proj); returns false and
 *   leaves *out_jittered_proj == clean_proj when r.taa is OFF, so the OFF
 *   path is byte-identical.  Drive the SAME pipeline you then call
 *   jce_postfx_apply() on (the engine-owned one from get_postfx()).
 *
 * end_frame: call it AFTER the scene colour pass (end of frame) with the
 *   SAME clean view + proj.  Records them as next frame's reproject source
 *   and DISABLES TAA on the engine PostFX pipeline so it never leaks into
 *   the editor's pick / preview / thumbnail postfx invocations.  Safe to
 *   call unconditionally; it self-no-ops when r.taa is OFF. */
JCE_API bool jce_scene_renderer_taa_begin_frame(JceSceneRenderer *sr,
                                                uint32_t target_w,
                                                uint32_t target_h,
                                                const jce_mat4 *clean_view,
                                                const jce_mat4 *clean_proj,
                                                jce_mat4 *out_jittered_proj);

JCE_API void jce_scene_renderer_taa_end_frame(JceSceneRenderer *sr,
                                              const jce_mat4 *clean_view,
                                              const jce_mat4 *clean_proj);

/* STANDARD per-object motion vectors for TAA: request that the next
 * jce_scene_renderer_render() write a per-object (and per-bone, for skinned)
 * velocity buffer in its depth/G-buffer pre-pass.  The caller then binds
 * jce_scene_renderer_get_velocity_texture() into its PostFX pipeline via
 * jce_postfx_set_taa_motion_tex() before applying TAA, so animated geometry
 * stops ghosting.  Set false (default) for zero cost when TAA is off.  Must be
 * set every frame TAA is on (reset to false implicitly is the caller's job). */
JCE_API void jce_scene_renderer_set_taa_velocity_enabled(JceSceneRenderer *sr,
                                                         bool enabled);

/* Call EXACTLY ONCE per displayed frame, BEFORE any viewport renders.  The
 * editor renders multiple viewports (Scene + Game) through one shared renderer
 * each frame; this advances a per-frame generation so the skinned-animation
 * sample + previous-frame TAA state are produced once (on the first viewport)
 * and reused by the rest, instead of being double-advanced and clobbered to
 * zero motion.  A no-op for single-viewport callers that still call it once. */
JCE_API void jce_scene_renderer_begin_velocity_frame(JceSceneRenderer *sr);

/* The per-object motion-vector (velocity) texture produced by the most recent
 * render when velocity was enabled AND the pre-pass ran AND the velocity shaders
 * loaded; UINT16_MAX otherwise.  Encoded identically to fs_motion_vec.sc, so it
 * feeds jce_postfx_set_taa_motion_tex() directly. */
JCE_API uint16_t jce_scene_renderer_get_velocity_texture(const JceSceneRenderer *sr);

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

/* Composite the SSR reflection RT over the destination color framebuffer.
 * No-op when SSR was not active this frame.  view_id must be > the scene's
 * SSR ray-march view (base+2) and the color pass; pass e.g. base+3. */
JCE_API void jce_scene_renderer_composite_ssr(JceSceneRenderer *sr, uint16_t view_id,
                                              uint16_t dst_fb_idx);

/* Per-frame culling stats from the most recent render call. */
typedef struct {
    uint32_t total;     /* entities collected this frame */
    uint32_t visible;   /* entities that passed frustum culling */
    uint32_t culled;    /* entities removed by culling (== total - visible) */
    bool     enabled;   /* whether culling was active this frame */
    /* Persistent extent-sized broad-phase (large-world-opt P1 #4). */
    uint32_t grid_res[3];     /* cells per axis of the cull grid */
    uint32_t grid_cells;      /* total cells (res.x*res.y*res.z) */
    uint32_t grid_occupied;   /* non-empty cells (what the full scan iterates) */
    uint32_t grid_objects;    /* objects resident in the persistent grid */
    uint32_t inserted;        /* entities inserted into the grid this frame */
    uint32_t updated;         /* entities re-bucketed (moved) this frame */
    uint32_t removed;         /* entities removed (despawned) this frame */
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

/* ── Animation frame-event sink (P1 anim-events → gameplay) ───────────
 *
 * The renderer samples skeletal animation AND advances each instance's
 * per-clip frame-event track every render (events authored into the
 * <skeleton>.anim.json sidecar, fired over the (prev,cur] clip-time window
 * by jce_anim_events_advance).  By default a fired event is only logged.
 *
 * Set a hook here to ROUTE fired events to gameplay: the runtime points it
 * at its entity→script dispatch so an authored footstep / hitbox-on / etc.
 * event reaches the entity's `on_anim_event(id, name, f0, f1, i0)` script
 * method.  `entity` is the firing entity id; `ev` is the fired event (valid
 * only for the duration of the call — copy out anything you keep); `user` is
 * the pointer passed here verbatim.  Pass fn=NULL to clear (back to log-only).
 *
 * Default NULL → byte-identical to the prior behaviour (the in-engine
 * LOG_DEBUG still fires), so this is purely additive. */
typedef void (*JceSceneRendererAnimEventFn)(uint64_t entity,
                                            const JceAnimEvent *ev,
                                            void *user);
JCE_API void jce_scene_renderer_set_anim_event_fn(JceSceneRenderer *sr,
                                                  JceSceneRendererAnimEventFn fn,
                                                  void *user);

/* ── Animation state-change sink (state-enter/exit → gameplay) ────────
 *
 * The renderer drives each skeletal animator's bound .anim_sm.json state
 * machine in Play and tracks its active state per instance.  When a state
 * machine's active state CHANGES (e.g. Walk → Attack), this hook (when set)
 * is called once with the transition's endpoints so gameplay can react:
 * the runtime points it at its entity→script dispatch so the change reaches
 * the entity's `on_state_exit(from_state)` / `on_state_enter(to_state)`
 * script methods.
 *
 * `entity` is the animator's entity id; `from_state` is the previous state
 * name (NULL/"" when entering the initial state — there is no prior state),
 * `to_state` is the newly-entered state name.  Both strings point into the
 * live SM and are valid only for the duration of the call (copy out anything
 * you keep).  `user` is the pointer passed here verbatim.  Pass fn=NULL to
 * clear.
 *
 * Default NULL → byte-identical to the prior behaviour (the SM drives the
 * pose, no events fire), so this is purely additive — the per-instance state
 * is only polled when a hook is installed. */
typedef void (*JceSceneRendererAnimStateFn)(uint64_t entity,
                                            const char *from_state,
                                            const char *to_state,
                                            void *user);
JCE_API void jce_scene_renderer_set_anim_state_fn(JceSceneRenderer *sr,
                                                  JceSceneRendererAnimStateFn fn,
                                                  void *user);

/* ── Ground-query hook (Foot IK ground adaptation) ────────────────────
 *
 * Foot IK (the JceFootIkComponent pass, sr_apply_foot_ik) needs to know the
 * ground height + normal under each foot, but the L4 scene renderer does NOT
 * hold the physics world.  The runtime installs this hook to a physics
 * raycast (mirrors the anim-event hook): the renderer calls it with a ray
 * origin (above the foot), a direction (typically straight down), and a max
 * distance; the hook writes the hit Y into *out_hit_y and the surface normal
 * into out_normal[3] and returns true on a hit, false on a miss.
 *
 *   entity    : the rigged entity being solved (lets the hook ignore the
 *               character's own collider if it wants)
 *   origin    : ray start (world space)
 *   dir       : ray direction (world space; need not be unit length)
 *   max_dist  : ray length
 *   out_hit_y : ground height at the hit (world Y); written only on a hit
 *   out_normal: ground normal at the hit; written only on a hit (may be NULL)
 *   user      : the pointer passed to set_ground_query_fn verbatim
 *
 * Default NULL → sr_apply_foot_ik is a complete NO-OP (no ground info → the
 * pose is byte-identical), so authoring a FootIk component changes nothing in
 * tools / before Play.  Pass fn=NULL to clear. */
typedef bool (*JceSceneRendererGroundQueryFn)(uint64_t entity,
                                             const float origin[3],
                                             const float dir[3],
                                             float max_dist,
                                             float *out_hit_y,
                                             float out_normal[3],
                                             void *user);
JCE_API void jce_scene_renderer_set_ground_query_fn(JceSceneRenderer *sr,
                                                    JceSceneRendererGroundQueryFn fn,
                                                    void *user);

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

/* Drop EVERY cached model (the model cache never evicts and caches load
 * FAILURES) so the next frame reloads all models from disk. Editors call this
 * on scene-switch so a stale failed/path flag from the previous scene cannot
 * block a model that exists in the new scene. Joins any in-flight async decode
 * and destroys each GPU model; safe to call between frames. */
JCE_API void jce_scene_renderer_invalidate_model_cache(JceSceneRenderer *sr);

/* ── VRAM ceiling (large-world-opt: free GPU resources on cell unload) ──────
 *
 * The path-keyed model cache is shared across entities/chunks and historically
 * never evicted, so a streaming world's VRAM only climbed.  These calls bound
 * it.  The renderer treats "resolved this frame via sr_get_model" as a per-frame
 * reference count; a model not resolved for a grace window of frames is provably
 * referenced by NO live entity (a streamed chunk's entities are destroyed on
 * unload and stop resolving their model) and can be freed.
 *
 * SAFETY: a model is freed ONLY when (a) it was not used for the grace window,
 * AND (b) no live skeletal-animation instance still references it.  Eviction is
 * driven at the START of jce_scene_renderer_render (after the async upload poll,
 * before any draw resolves a model), so no in-flight draw on this frame's view
 * list can reference a destroyed handle.  RUNTIME MODE ONLY: in editor mode the
 * asset-cache callback owns the models, so the renderer never frees them and
 * these calls are no-ops for eviction (the budget setter still records intent).
 */

/* Set the model-VRAM ceiling in bytes (0 = unlimited / no eviction, the
 * default).  When the resident model VRAM exceeds this, the next render frees
 * the least-recently-used unreferenced models down toward the budget. */
JCE_API void jce_scene_renderer_set_model_vram_budget(JceSceneRenderer *sr,
                                                      uint64_t budget_bytes);

/* Free unreferenced models until resident model VRAM <= budget_bytes (or no
 * eligible victim remains).  Normally driven automatically by the render loop;
 * exposed for tools / tests.  No-op in editor mode.  Returns the number of
 * models freed. */
JCE_API uint32_t jce_scene_renderer_evict_models(JceSceneRenderer *sr,
                                                 uint64_t budget_bytes);

/* Total resident model VRAM (sum of jce_model_gpu_bytes over every loaded model
 * cache slot) — the real GPU bytes the streaming budget should account for.
 * 0 in editor mode (models are owned/sized by the asset cache, not here). */
JCE_API uint64_t jce_scene_renderer_model_vram_bytes(const JceSceneRenderer *sr);

/* Resident GPU bytes of ONE model path (0 if not resident / still decoding /
 * editor mode).  The world streamer sums this across a chunk's entities to
 * report real per-chunk residency to the streaming budget. */
JCE_API uint64_t jce_scene_renderer_model_path_vram_bytes(
    const JceSceneRenderer *sr, const char *path);

/* Sum the resident GPU bytes of the models referenced by a set of entities (a
 * chunk's roster): for each entity, resolve its MeshRenderer / skeletal model
 * path and add jce_scene_renderer_model_path_vram_bytes.  A model shared by
 * several of the entities is counted once.  This is exactly the
 * JceWorldStreamerResidencyFn shape the world streamer wants — wire it as the
 * residency query so per-chunk residency reflects real VRAM.  Returns 0 in
 * editor mode (the renderer doesn't own/size models there). */
JCE_API uint64_t jce_scene_renderer_entities_vram_bytes(
    JceSceneRenderer *sr, JceScene *scene,
    const uint64_t *entity_ids, uint32_t count);

/* Lifetime count of models freed by VRAM-ceiling eviction (editor surfacing). */
JCE_API uint32_t jce_scene_renderer_model_evicted_count(const JceSceneRenderer *sr);

/* Number of LOADED models currently in the path-keyed cache (editor surfacing).
 * Counts only resident models (not pending decodes or cached failures). */
JCE_API uint32_t jce_scene_renderer_model_cache_count(const JceSceneRenderer *sr);

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
    uint32_t no_result; /* query still in flight (GPU latency), drawn */
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

/* Project-level grass rendering gate (Stage 1b.6).
 * Mirrors JceRenderSettings.grass_enabled; default false (off).
 * Call once after loading the project render settings.  Only takes effect
 * when hardware instancing is available AND GPU tier >= HIGH. */
JCE_API void jce_scene_renderer_set_grass_enabled(JceSceneRenderer *sr, bool on);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_RENDERER_H */
