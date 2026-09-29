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
#include <jce/renderer/jce_fullscreen_effect.h>
#include <jce/renderer/jce_occlusion_culler.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_texture_types.h>
#include <jce/renderer/jce_decals.h>
/* Both types below are used by POINTER only, so a forward declaration would
 * normally do — but each is declared as an UNTAGGED `typedef struct {…} X;`,
 * which C gives no way to forward declare.  Removing these two middleware
 * includes therefore requires tagging the definitions at their source
 * (jce_anim_ik.h / jce_time_of_day.h); until then the includes stay. */
#include <jce/middleware/animation/jce_anim_ik.h>   /* JceAnimEvent (POD) */
#include <jce/middleware/world/jce_time_of_day.h>   /* JceTimeOfDayState */
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

#define JCE_SCENE_FULLSCREEN_EFFECT_MAX_PASSES 8u

typedef struct JceSceneFullscreenEffectStageStatus {
    uint32_t struct_size;
    uint32_t active_count;
    uint32_t applied_count;
    uint32_t dropped_count;
    uint64_t failed_entity;
    bool required_failure;
    JceFullscreenEffectStatus pass;
} JceSceneFullscreenEffectStageStatus;

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
    /* Shadow/depth debug views.  APPENDED, never inserted: the editor stores
     * the selected mode in its config, so renumbering the existing values
     * would silently reinterpret a saved setting as a different view.
     *
     * These three exist because "the picture changes with distance" is not
     * answerable from a shaded frame.  Together they separate the three
     * candidate causes: SCENE_DEPTH says how far a surface actually is,
     * SHADOW_CASCADES says which cascade shadowed it (and grey says none did,
     * i.e. it is past the shadow range), and SHADOW_MASK says how much it was
     * shadowed.  A band that lines up with a cascade colour change is a
     * cascade boundary; one that lines up with grey is the shadow-range fade;
     * one that lines up with neither is not a shadow problem at all. */
    JCE_SCENE_VIEW_SCENE_DEPTH,          /* 8: view depth, ramped + banded  */
    JCE_SCENE_VIEW_SHADOW_CASCADES,      /* 9: cascade index as R/G/B/Y     */
    JCE_SCENE_VIEW_SHADOW_MASK,          /* 10: shadow factor, 0=occluded   */

    /* Sentinel, always last.  The renderer clamps out-of-range modes to
     * SHADED, and that bound used to name JCE_SCENE_VIEW_AO directly -- so
     * adding the three views above left them accepted by the editor, plumbed
     * through the config, and then silently turned back into SHADED one line
     * before the uniform was written.  Nothing failed; the views simply
     * rendered a normal frame, which is the hardest kind of wrong to notice.
     * Bounding against the sentinel means the next view added here is in
     * range by construction. */
    JCE_SCENE_VIEW_COUNT
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

    /* This frame's scene depth texture (matching this view+proj), or
     * UINT16_MAX for none.
     *
     * Set it whenever you have one, INDEPENDENTLY of any feature that
     * consumes it.  Several passes need scene depth -- volumetric fog and
     * underwater absorption today, more later -- and they are unrelated
     * features that users switch on and off separately.
     *
     * It was called fog_depth_tex_handle, and the editor set it only when
     * fog was enabled, which read as correct because the name says it
     * belongs to fog.  The consequence was that turning fog off also
     * silently disabled underwater absorption: an unrelated switch, no
     * error, and nothing in either feature's code to suggest the other
     * was involved. */
    uint16_t                 scene_depth_tex_handle;  /* UINT16_MAX = none */

    /* Volumetric fog (analytic + raymarched).  When fog_enabled is true
     * AND scene_depth_tex_handle is valid, the renderer will allocate /
     * resize a JceVolumetricFog instance, push fog params, and execute
     * the fog raymarch into a private RT.  The result texture handle is
     * exposed via jce_scene_renderer_get_fog_result_texture() so callers
     * can composite it (multiply scene by alpha, add rgb) themselves.
     * The renderer does NOT composite into the caller's color RT yet —
     * that is a separate phase (composite shader pending). */
    bool                     fog_enabled;
    JceVolumetricFogParams   fog;
    int                      fog_rt_width;          /* 0 = skip fog */
    int                      fog_rt_height;         /* 0 = skip fog */

    /* Screen-space reflections: the lit color RT the SSR pass reflects.  SSR's
     * ray-march view renders after the color pass, so this is the current
     * frame's color.  UINT16_MAX = no SSR this frame. */
    uint16_t                 ssr_color_tex_handle;

    /* GI L1 (dynamic irradiance probes): the PREVIOUS frame's lit color RT
     * the probe gather samples (same texture as SSR's).  The gather runs on
     * the pre-color compute view, so by then the texture still holds LAST
     * frame's color — paired with last frame's depth + VP like the Hi-Z
     * cull.  UINT16_MAX = no dynamic GI this frame. */
    uint16_t                 gi_color_tex_handle;

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

    /* Will this caller call jce_scene_renderer_composite_fog() this frame?
     *
     * It has to say, because the renderer cannot know. The volumetric march
     * runs inside jce_scene_renderer_render, but the COMPOSITE is a separate
     * public entry point the application invokes -- so whether the marched fog
     * ever reaches the screen is decided outside this call, after it returns.
     *
     * It matters because the analytic aerial fog in fog_apply.sh and the
     * volumetric march apply the SAME authored extinction to the SAME pixel.
     * Composed, the surface's transmittance is exp(-sigma*d) * exp(-sigma*d)
     * = exp(-2*sigma*d): every scene with fog on was twice as foggy as it
     * asked to be, and the two halves disagree about how -- one is an
     * analytic closed form, the other a 32-step march with a phase function.
     *
     * When this is true and the volumetric pass actually runs, the analytic
     * path is suppressed for that frame: one extinction, applied by the
     * solver that models it better. When it is false -- an embedder that
     * marches fog and never composites it, or one that does not use the
     * volumetric pass at all -- the analytic path stays, because otherwise
     * they would get no fog at all.
     *
     * DEFAULT false preserves existing behaviour exactly, including the
     * doubling: a caller that does composite must opt in by saying so. That
     * is the safe direction for a field appended to a public struct, where
     * every existing caller zero-initialises and cannot be asked. */
    bool                     composites_volumetric_fog;
    /* Camera culling mask, resolved by the HOST from the camera component it
     * is rendering (JceCameraComponent.culling_mask).  Bit N set = draw
     * entities whose JceLayerComponent.layer is N.  ZERO = no filtering, so a
     * zero-initialised config behaves exactly as every build did before this
     * field existed.
     *
     * Applied to the colour pass AND the depth/velocity prepass, which must
     * agree: the prepass feeds SSAO, SSR and TAA motion vectors, and a
     * mismatch there is what made the streamed set shimmer every frame (see
     * the cull_aspect comment in jce_sr_draw.c).  NOT applied to shadow
     * passes -- an object a camera does not render can still cast into the
     * scene -- and NOT to the editor pick pass, so a masked object stays
     * selectable in the Scene View, which is what Unity does. */
    uint32_t             camera_culling_mask;

    /* A SECOND, CHEAP RENDER OF THE SAME SCENE.  Set by a caller that wants
     * the colour pass and nothing else -- a planar reflection, a thumbnail, a
     * capture.  It suppresses every screen-space effect: SSAO, SSR and SSGI,
     * and with them the depth/normal pre-pass.
     *
     * NOT because they would look wrong in a reflection, though they would.
     * Because they write into state the MAIN view owns: sr->ssao_depth_tex
     * and sr->ssao_normal_tex are one pair for the whole renderer, so a
     * second render that fills them hands the main view someone else's depth
     * on the next frame.  And because their view offsets (base+2/3, +18/19,
     * +26/27) are added to whatever base the second render is given -- from a
     * high base that is arithmetic past 255, which bgfx does not have.
     *
     * Default false: every existing caller renders exactly as before.
     * APPENDED. */
    bool                 reduced_pass;

    /* CAMERA STACKING -- how many OVERLAY cameras will draw on top of this
     * render, 0..JCE_VIEW_SR_CAMERA_OVERLAY_MAX.
     *
     * The renderer needs it BEFORE it draws anything, because it decides the
     * view ORDER: the overlay views (base+64..66) have to be named and placed
     * right after the colour view, ahead of SSR, the fullscreen chain, SSGI's
     * composite and the PostFX chain -- all of which READ the colour the
     * overlays write into.  Reserved from this count rather than from whether
     * an overlay actually drew, the same way the underwater view is reserved:
     * what draws is decided after the order is set.
     *
     * The host sets it from jce_scene_camera_resolve_stack and then calls
     * jce_scene_renderer_render_camera_overlay once per overlay, after
     * jce_scene_renderer_render.  Zero -- every scene with one camera, and
     * every scene authored before stacking existed -- reserves nothing and
     * produces a byte-identical order.
     *
     * APPENDED. */
    uint8_t              camera_overlay_count;

    /* JceCameraComponent.clear_mode, verbatim, for the camera this config
     * renders.  Read ONLY by jce_scene_renderer_render_camera_overlay, which
     * turns it into what the overlay view clears; the main render still
     * decides the sky through draw_skybox, which the host sets from
     * jce_scene_camera_clear_draws_skybox -- one authority for the sky
     * question, one for the clear question, and neither derived from the
     * other.  Zero is JCE_CAMERA_CLEAR_SKYBOX, which an overlay refuses, so a
     * zero-initialised config that never authored a stack is unaffected.
     * APPENDED. */
    uint8_t              camera_clear_mode;
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
    /* OPTIONAL, and the same lookup as load_texture with ONE difference: the
     * texture is sRGB-ENCODED COLOUR, so it must be created with a hardware
     * sRGB view -- the sampler then decodes each texel BEFORE filtering it,
     * which is where the shader's pow(2.2) could never run.  The engine asks
     * for this only where the consuming shader treats the sampled value as
     * linear: s_albedo, s_emissive, and fs_terrain's four layer albedos.
     *
     * A NULL here is not fatal: the engine falls back to load_texture and
     * warns ONCE that albedo will render over-bright, because the shader no
     * longer decodes.  APPENDED (ABI) -- an embedder compiled against the
     * older struct has a zeroed tail and takes exactly that fallback. */
    JceTexture  (*load_texture_srgb)(const char *material_path,
                                     const char *mesh_path,
                                     void       *ud);
} JceSceneRendererCallbacks;

/* ── Lifecycle ────────────────────────────────────────────────────── */

/*
 * Create a scene renderer.
 *   renderer : the engine renderer (for shader programs, texture binding).
 *   pak      : PAK archive for loading shaders and fallback assets.
 *   cbs      : optional asset callbacks (NULL = use PAK only).
 */
JCE_API JceSceneRenderer *jce_scene_renderer_create(
    JceRenderer           *renderer,
    const JcePakArchive   *pak,
    const JceSceneRendererCallbacks *cbs);

JCE_API void jce_scene_renderer_destroy(JceSceneRenderer *sr);

/* Drop every entity-keyed environment cache (vegetation scatter, grass,
 * water, foliage-cluster canopies).  Call after any operation that
 * renumbers entity ids while this renderer stays alive — e.g. an editor
 * undo/redo or scene switch that destroys + recreates the ECS world, which
 * hands recycled ids back with bumped generation bits and strands every
 * cached slot.  Content rebuilds lazily on the next draw. */
JCE_API void jce_scene_renderer_reset_entity_caches(JceSceneRenderer *sr);

/* Return true when the scene contains at least one full-screen effect that is
 * enabled at the entity, component-registry, and component-value layers.
 * This query is CPU-only and never allocates renderer resources. */
JCE_API bool jce_scene_renderer_has_fullscreen_effect(JceScene *scene);

/* Apply one insertion stage in deterministic (insertion, order, entity) order.
 * Up to JCE_SCENE_FULLSCREEN_EFFECT_MAX_PASSES consecutive bgfx views starting
 * at view_id_base may be consumed.  No active effect returns scene_color and
 * performs no allocation.  Optional failures preserve the previous color;
 * required failures are reported by get_fullscreen_effect_status(). */
JCE_API JceTextureHandle jce_scene_renderer_apply_fullscreen_effects(
    JceSceneRenderer *sr, JceScene *scene, const JceCamera *camera,
    JceTextureHandle scene_color, JceTextureHandle scene_depth,
    uint32_t width, uint32_t height, uint16_t view_id_base,
    int viewport_id, uint32_t insertion, float dt_sec);

JCE_API void jce_scene_renderer_get_fullscreen_effect_status(
    const JceSceneRenderer *sr, int viewport_id,
    JceSceneFullscreenEffectStageStatus *out_status);

/* Optional host directory containing a `shaders/` overlay.  Project PAK/VFS
 * lookup remains authoritative in packaged builds. */
JCE_API void jce_scene_renderer_set_project_shader_dir(
    JceSceneRenderer *sr, const char *directory);

/* Return true when the scene contains at least one full-screen effect that is
 * enabled at the entity, component-registry, and component-value layers.
 * This query is CPU-only and never allocates renderer resources. */
JCE_API bool jce_scene_renderer_has_fullscreen_effect(JceScene *scene);

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
JCE_API uint16_t jce_scene_renderer_render(
    JceSceneRenderer           *sr,
    JceScene                   *scene,
    const JceCamera            *camera,
    uint16_t                    view_id_base,
    float                       dt_sec,
    const JceSceneRenderConfig *config);

/* Draw ONE overlay camera on top of a render that has already happened.
 *
 * Unity's camera stacking: a Base camera renders the scene, and each Overlay
 * camera draws its own slice of the same scene into the same colour target
 * without clearing it -- a first-person weapon that never intersects the
 * level, a 3D inventory model, a portal.  What makes it an overlay is the
 * clear mode, not a separate target.
 *
 * CALL IT AFTER jce_scene_renderer_render, with:
 *   view_id_base   the SAME base the main render used;
 *   overlay_index  0..JCE_VIEW_SR_CAMERA_OVERLAY_MAX-1, in stack order;
 *   config         the OVERLAY camera's own culling mask and clear mode, and
 *                  the SAME scene_color_fb / viewport as the main render.
 * The view it draws into is view_id_base + JCE_VIEW_SR_CAMERA_OVERLAY_OFFSET
 * + overlay_index, which the main render's view-order pass has already placed
 * immediately after the colour view.  Calling it with a base whose main render
 * did not declare config->camera_overlay_count leaves the overlay in the
 * filler tail, where it would draw after tone mapping -- so the count is not
 * optional, and this function says so rather than guessing.
 *
 * CLEAR MODE decides what survives underneath:
 *   JCE_CAMERA_CLEAR_DEPTH_ONLY  clears depth, keeps colour -- the usual
 *                                choice: the overlay never intersects what is
 *                                already drawn.
 *   JCE_CAMERA_CLEAR_NOTHING     clears nothing -- the overlay shares the
 *                                base camera's depth and can be occluded by it.
 *   JCE_CAMERA_CLEAR_COLOR       clears colour and depth, which erases the base
 *                                camera.  Legal, and almost never what is
 *                                wanted; it is not refused because a full-frame
 *                                second view is a real use.
 *   JCE_CAMERA_CLEAR_SKYBOX      REFUSED and logged once, treated as
 *                                DEPTH_ONLY: a sky drawn by an overlay covers
 *                                everything under it, which is not a stack.
 *                                Unity refuses the same thing in the same way.
 *
 * A no-op when scene, camera or config is NULL, or when overlay_index is past
 * the cap. */
JCE_API void jce_scene_renderer_render_camera_overlay(
    JceSceneRenderer           *sr,
    JceScene                   *scene,
    const JceCamera            *camera,
    uint16_t                    view_id_base,
    uint8_t                     overlay_index,
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
 *   about to use.  It ALWAYS pushes the un-jittered inverse-view-proj +
 *   previous view*proj to the engine PostFX pipeline -- those are camera
 *   state, and TAA is only one of the things that reprojects with them; the
 *   post-fx motion blur wants the same pair, and behind the TAA gate it
 *   silently required TAA to be on.  THEN, when r.taa is ON, it advances the
 *   jitter sequence, writes the JITTERED projection into *out_jittered_proj
 *   (use THAT for the colour pass), and enables TAA on the pipeline.
 *   Returns true iff TAA is active this frame (caller uses
 *   *out_jittered_proj); returns false and leaves *out_jittered_proj ==
 *   clean_proj when r.taa is OFF, so the OFF path is byte-identical to a
 *   build without TAA.  Drive the SAME pipeline you then call
 *   jce_postfx_apply() on (the engine-owned one from get_postfx()).
 *
 * end_frame: call it AFTER the scene colour pass (end of frame) with the
 *   SAME clean view + proj.  ALWAYS records them as next frame's reproject
 *   source -- gated on TAA, "previous" meant "whenever TAA was last on" --
 *   and DISABLES TAA on the engine PostFX pipeline so it never leaks into
 *   the editor's pick / preview / thumbnail postfx invocations.  Safe to
 *   call unconditionally. */
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
/* Render the planar reflection for this frame, if any water asks for one.
 *
 * Call it AFTER the main render, once per frame per host.  It is a SECOND,
 * reduced scene render (jce_planar_reflection.h) into its own absolute view
 * band, and the water pass consumes it on the NEXT frame -- which is forced,
 * not chosen: bgfx runs views in ascending id order and the water draws in
 * the colour view at base+0, so nothing base-relative can precede it.
 *
 * No-op when no Water component has planar_reflection set, which is every
 * scene that has not asked: a project that never opts in pays nothing and
 * renders byte-identically. */
JCE_API void jce_scene_renderer_render_planar_reflection(
    JceSceneRenderer *sr, JceScene *scene, const JceCamera *camera,
    float dt_sec);

/* Add the SSGI bounce onto the destination colour buffer.  `view_id` must
 * be greater than the march view (base + JCE_VIEW_SR_SSGI_OFFSET); no-op when
 * SSGI produced nothing this frame. */
JCE_API void jce_scene_renderer_composite_ssgi(JceSceneRenderer *sr,
                                               uint16_t view_id,
                                               JceFrameBufferHandle dst);

JCE_API void jce_scene_renderer_composite_ssr(JceSceneRenderer *sr, uint16_t view_id,
                                              uint16_t dst_fb_idx);

/* Blend the PLANAR REFLECTION PROBE's mirror over the destination colour.
 *
 * Pass the SAME view id as the SSR composite and call this SECOND: the two
 * are one stage with two producers (jce_views.h's
 * JCE_VIEW_SR_REFLECTION_COMPOSITE_OFFSET says why), and ordering the planar
 * one last lets the accurate reflection win where it applies over the
 * screen-space guess.
 *
 * Reads the depth and G-buffer normal this frame already produced, so it
 * reaches ANY surface that lies on the probe's plane rather than only the
 * materials with a free sampler slot -- which was one, water.  No-op when no
 * planar probe asked, when the mirror could not render (camera behind the
 * plane) or when there is no G-buffer this frame. */
JCE_API void jce_scene_renderer_composite_planar(JceSceneRenderer *sr,
                                                 uint16_t view_id,
                                                 JceFrameBufferHandle dst);

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
    /* Collected entities the COLOUR pass skipped because they are disabled
     * (JceEditorMeta.enabled false, or the cached SR_RK_DISABLED kind).
     *
     * Counted because nothing else could answer it.  Static batching disables
     * the renderers it merged, and with only `total` and `visible` available a
     * bake that worked was indistinguishable from one that did not: a lossless
     * merge draws the same pixels the originals did, so the picture is the same
     * either way and the entity count moves by +1 in both cases.  APPENDED. */
    uint32_t disabled_skipped;
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

typedef enum JceTerrainEditFlags {
    JCE_TERRAIN_EDIT_HEIGHTS = 1u << 0,
    JCE_TERRAIN_EDIT_SPLAT   = 1u << 1,
    JCE_TERRAIN_EDIT_HOLES   = 1u << 2,
    JCE_TERRAIN_EDIT_ALL     = (1u << 3) - 1u
} JceTerrainEditFlags;

/* Invalidate only the terrain-derived GPU data touched by an authoring brush.
 * Bounds are in the terrain asset's LOCAL XZ coordinates. Height/hole edits
 * rebuild intersecting chunks plus a one-chunk normal halo; splat edits refresh
 * the monolithic splat texture or only the intersecting tiled textures. Must be
 * called on the render/main thread between frames. `scene` lets the renderer
 * acknowledge a committed shared-terrain revision and avoid a redundant full
 * slot rebuild on the next frame. */
JCE_API void jce_scene_renderer_invalidate_terrain_region(
    JceSceneRenderer *sr, JceScene *scene, const char *path,
    float min_x, float min_z, float max_x, float max_z,
    uint32_t edit_flags);

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

/* Forget the cached graph PROGRAM for one .mat.json (NULL = all of them).
 *
 * sr_resolve_custom_program caches by path, resolves once and never evicts --
 * correct for a running game, wrong the moment a material is re-authored.
 * Recompiling a Shader Graph swapped the program on the panel's preview sphere
 * while every entity already placed in the scene kept drawing with the old
 * one, so the editor showed two different answers for the same material and
 * neither of them was labelled.
 *
 * Recompiling only forgets the association; the next draw re-reads the
 * .mat.json and links whatever it now names.  Safe to call between frames. */
JCE_API void jce_scene_renderer_invalidate_custom_program(JceSceneRenderer *sr,
                                                          const char *material_path);

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

/* GPU-driven model-batch diagnostics from the most recent color pass. */
typedef struct JceSceneGpuDrivenStats {
    bool     enabled;
    bool     active;
    bool     adaptive_bypass;
    bool     forced;
    bool     supported;
    bool     indirect_supported;
    bool     indirect_ready;
    bool     dispatch_succeeded;
    bool     hiz_enabled;
    uint32_t candidate_groups;
    uint32_t candidate_records;
    uint32_t candidate_draws;
    uint32_t min_records;
    uint32_t min_group_records;
    uint32_t min_records_per_draw;
    uint32_t total_runs;
    uint32_t eligible_runs;
    uint32_t fallback_runs;
    uint32_t records;
    uint32_t gpu_groups;
    uint32_t gpu_runs;
    uint32_t indirect_submits;
    uint32_t fixed_count_submits;
    uint32_t cpu_fallback_submits;
    uint32_t compute_dispatches;
    uint32_t upload_calls;
    uint32_t buffer_growths;
    uint64_t uploaded_bytes;
    double   sort_ms;
    double   prepare_ms;
    double   flush_ms;
} JceSceneGpuDrivenStats;

JCE_API void jce_scene_renderer_get_gpu_driven_stats(
    const JceSceneRenderer *sr, JceSceneGpuDrivenStats *out);

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

/* Map a scene's authored fog settings onto JceVolumetricFogParams.
 *
 * Returns true when the scene wants volumetric fog at all.  This lives in the
 * ENGINE precisely because it used to live in an editor panel: a shipped game
 * therefore had no way to produce these params, so the fog pass never ran
 * outside the editor -- the same shape of bug SSR had and already fixed.  One
 * derivation, both consumers. */
/* ── Intermediate render targets, for an inspector ────────────────────
 *
 * WHAT THIS IS FOR.  Unity's Frame Debugger, UE's `viewmode` buffer
 * visualisation and Godot's debug draw modes all answer one question: what
 * does the shadow map / depth / normals / AO actually contain right now.  In
 * this engine those textures existed and nothing could look at one, so a
 * wrong G-buffer was only ever visible as a wrong final image.
 *
 * A PULL, NOT A REGISTRY.  The scene renderer owns every one of these
 * handles, so it answers directly; there is no second table to drift and no
 * per-frame cost when nobody is looking.
 *
 * ONLY WHAT IS REAL THIS FRAME.  Each target has a validity bit beside it in
 * the renderer (the depth pre-pass may not have run, TAA may be off so the
 * velocity attachment holds nothing, shadows may be in CSM mode so the single
 * map is unused), and the enumeration honours all of them.  A listed-but-
 * stale target would display LAST frame's pixels, or an uninitialised
 * allocation -- and an image that is not blank is the hardest wrong answer
 * there is to notice.  The list therefore CHANGES LENGTH from frame to frame,
 * by design: an entry appearing is the feature turning on.
 *
 * Handles are borrowed and valid only until the next jce_scene_renderer_render
 * on this renderer.  Copy the pixels (jce_render_readback_*) if you need them
 * to outlive the frame. */

typedef enum JceRenderTargetKind {
    JCE_RT_KIND_COLOR = 0,   /* sample and show as-is                    */
    JCE_RT_KIND_DEPTH,       /* near-white over most of its range; a raw
                              * sample is legible only after a remap      */
    JCE_RT_KIND_NORMAL,      /* rgb = normal*0.5+0.5, a = roughness       */
    JCE_RT_KIND_VELOCITY,    /* rg = (curNDC-prevNDC)*0.5+0.5             */
    JCE_RT_KIND_SHADOW,      /* depth from a light                        */
    JCE_RT_KIND_LUT          /* a lookup table, not a picture of a scene  */
} JceRenderTargetKind;

typedef struct JceRenderTargetInfo {
    /* Stable dotted id, e.g. "gbuffer.normal".  An inspector persists the
     * user's selection by NAME: an index would move the moment a target
     * became invalid for a frame, which is precisely when it is being
     * watched. */
    const char      *name;
    /* One sentence on how to READ it, because most of these are not pictures
     * and a viewer that shows a velocity buffer with no note has shown the
     * user a flat grey rectangle and told them nothing. */
    const char      *note;
    JceTextureHandle texture;
    uint16_t         width;      /* 0 when the renderer does not record it */
    uint16_t         height;
    uint32_t         kind;       /* JceRenderTargetKind */
} JceRenderTargetInfo;

/* How many targets are inspectable RIGHT NOW.  Varies by frame and by which
 * features are enabled; 0 is a legitimate answer (a renderer that has not
 * drawn a frame yet has nothing real to show). */
JCE_API int jce_scene_renderer_debug_target_count(const JceSceneRenderer *sr);

/* Fill `out` for target `index` in [0, count).  False for an out-of-range
 * index or a NULL argument, leaving `out` untouched. */
JCE_API bool jce_scene_renderer_debug_target_get(const JceSceneRenderer *sr,
                                                 int index,
                                                 JceRenderTargetInfo *out);

JCE_API bool jce_scene_fog_params_from_scene(const JceScene *scene,
                                             JceVolumetricFogParams *out);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_RENDERER_H */
