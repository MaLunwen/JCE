/* jce_sr_internal.h  Internal shared state for the scene-renderer modules.
 *
 * Holds the JceSceneRenderer struct, so the (formerly monolithic) renderer can
 * be split across jce_sr_*.c translation units that all share ONE definition
 * of the renderer state.  The Sr* helper types it is built from, and the
 * engine includes and capacity constants they need, are in jce_sr_types.h,
 * which this includes -- so a module includes THIS header and gets both.  Internal to the renderer implementation — NOT a public
 * header and never installed.  (Extracted from jce_scene_renderer.c.)
 */
#ifndef JCE_SR_INTERNAL_H
#define JCE_SR_INTERNAL_H

#include "jce_sr_types.h"   /* engine includes, capacity constants, Sr* records */


/* Ratio of cloud-base wind to the surface wind the environment carries.
 * Surface wind is slowed by friction with the ground; the free-atmosphere wind
 * above the boundary layer is typically two to four times it. */
#define JCE_CLOUD_WIND_FACTOR 3.0f

/* The scene's cloudCoverage slider maps onto the noise field's weather
 * coverage BIAS, not onto a coverage directly.
 *
 * Shared because the sky atlas and the ground shadow map must be baked from
 * the same field with the same bias -- two copies of this line is how the sky
 * and its own shadow came to disagree about where the clouds were. Measured
 * span of the field: a bias of +0.6 leaves 57.8% of the volume non-zero and it
 * saturates above that, so the useful range is about [-0.2, +0.6], which is
 * what this maps onto. A coverage of 0 returns -1, which clamps the whole
 * field to nothing -- fully clear, not "a little cloud". */
static inline float jce_cloud_bias_for_coverage(float coverage)
{
    if (!(coverage > 0.0f)) return -1.0f;
    if (coverage > 1.0f) coverage = 1.0f;
    return -0.2f + coverage * 0.8f;
}

struct JceSceneRenderer {
    JceRenderer            *renderer;
    const JcePakArchive    *pak;
    JceSceneRendererCallbacks cbs;
    bool                    has_cbs;
    bool                    homogeneous_depth;
    struct SrPortalSet     *portals;  /* CLOSED portals; jce_sr_portal.c */
    /* Play drives active_clip from the bound state machine; editor preview
     * leaves it idle so manual clip selection still previews. */
    bool                    anim_sm_active;

    /* Sky shader + uniforms. */
    bgfx_program_handle_t   prog_sky;
    bgfx_vertex_layout_t    sky_layout;
    bgfx_uniform_handle_t   u_sky_colors;
    bgfx_uniform_handle_t   u_sky_params;
    bgfx_uniform_handle_t   u_sky_equirect;
    /* Baked atmospheric transmittance (fs_sky.sc mode 4 = PHYSICAL).
     *
     * Created and uploaded ONCE -- the table is a function of the atmosphere
     * PARAMETERS, not of the sun, so moving the sun does not invalidate it.
     * That is the whole reason it is worth baking: the expensive part happens
     * when a designer changes the air, not every frame. */
    bgfx_texture_handle_t  sky_transmittance_tex;
    /* Hillaire multiple-scattering table, baked once beside the transmittance
     * one it shares its axes with. Both are pure functions of the atmosphere
     * parameters, so both are built on first use and never rebuilt. */
    bgfx_texture_handle_t  sky_multiscatter_tex;
    bgfx_uniform_handle_t  u_sky_multiscatter;
    bool                   sky_multiscatter_ready;
    bgfx_uniform_handle_t  u_sky_transmittance;
    bool                   sky_transmittance_ready;

    /* Cancellable CPU bake; render-thread publication retains the atlas. */
    bgfx_texture_handle_t  cloud_atlas_tex;
    bgfx_uniform_handle_t  u_sky_clouds;
    bgfx_uniform_handle_t  u_cloud_params;
    bgfx_uniform_handle_t  u_cloud_atlas;
    bgfx_uniform_handle_t  u_cloud_quality;
    bgfx_uniform_handle_t  u_cloud_period;
    /* THE authoritative environment state (jce_environment.h, plan section 2).
     * Sky, cloud, fog, water and vegetation are to READ this; none of them
     * keeps its own weather.  Advanced once per frame, from one place. */
    /* A VIEW onto the scene's environment state, refreshed at the top of every
     * render. The renderer advances it, but it does not own it: physics and
     * gameplay read the same wind, and when this was a renderer-private copy
     * they could not -- the runtime built the buoyancy ocean from an
     * unweathered wind while the screen showed a weathered one, and both were
     * handed to the same water field.
     *
     * env_local is the fallback for a render with no scene (the editor draws
     * empty viewports). Never NULL after sr_bind_environment. */
    JceEnvironmentState       *env;
    JceEnvironmentState        env_local;

    bool                   cloud_atlas_ready;
    /* Coverage bias the resident atlas was baked at, so a coverage
     * change invalidates it.  The atlas encodes coverage now; it is not
     * merely thresholded by it. */
    float                  cloud_atlas_bias;
    struct SrCloudAtlasState *cloud_atlas_state;
    /* Report state: off, wrong mode, active, preparing, unavailable. */
    int                    cloud_report_state;
    int                    cloud_shadow_report;  /* -1 = never reported */
    float                  cloud_atlas_dims[4];   /* dim_x,dim_y,dim_z,tiles */
    /* Captured from the scene's rendering settings each frame, like sky_mode. */
    float                  cloud_coverage;
    float                  cloud_density;
    float                  cloud_bottom_km;
    float                  cloud_top_km;

    /* Preetham analytic sky (fs_sky.sc mode 2). */
    bgfx_uniform_handle_t   u_sky_perez;     /* vec4[4]: Y/x/y A..D + E pack */
    bgfx_uniform_handle_t   u_sky_zenith;    /* vec4: Yz,xz,yz,normalize     */
    bgfx_uniform_handle_t   u_sky_sun_dir;   /* vec4: sun dir (toward sun)   */
    /* Stylized sky dome (fs_sky.sc mode 3). */
    bgfx_uniform_handle_t   u_sky_dome_mid;     /* (mid.rgb, mid_pos)                    */
    bgfx_uniform_handle_t   u_sky_dome_glow;    /* (glow.rgb, glow_falloff)              */
    bgfx_uniform_handle_t   u_sky_dome_sun;     /* (sun_size, softness, halo_pow, halo_s)*/
    bgfx_uniform_handle_t   u_sky_dome_sun_col; /* (sun_color.rgb, pad)                  */
    bgfx_uniform_handle_t   u_sky_dome_ray;     /* (count, length_rad, sharpness, str)   */
    /* Captured each frame from scene_rendering (renderer settings-capture). */
    float                   dome_zenith[3];
    float                   dome_mid[4];        /* rgb + mid_pos    */
    float                   dome_horizon[3];
    float                   dome_ground[3];
    float                   dome_glow[4];       /* rgb + falloff    */
    float                   dome_sun[4];        /* size,soft,hpow,hstr */
    float                   dome_sun_col[3];
    float                   dome_sun_dir[3];    /* authored disk dir; zero = unset */
    float                   dome_ray[4];        /* count,len,sharp,strength (count 0 = off) */
    float                   dome_anchor;        /* >0 = origin-anchored dome radius */

    /* Procedural meshes. */
    JceMesh                *cube_mesh;
    JceMesh                *plane_mesh;
    JceMesh                *sphere_mesh;
    JceMesh                *capsule_mesh;
    JceMesh                *cylinder_mesh;

    /* Fallback textures. */
    bgfx_texture_handle_t   white_tex;
    bgfx_texture_handle_t   checker_tex;     /* magenta/yellow "missing" pattern */
    bgfx_texture_handle_t   dummy_cube;      /* 1x1 black cube: IBL stages 6/7 when
                                              * IBL is off (WebGL2 rejects draws
                                              * whose CUBE samplers dangle on the
                                              * same unit as a 2D sampler) */

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

    /* Sky-derived ambient, as 9 SH coefficients per RGB channel.
     *
     * Replaces the hand-picked ambient literals that used to stand in for the
     * sky whenever no HDR skybox supplied IBL.  Those literals could not
     * agree with the Preetham sky actually being drawn, because they were a
     * different model entirely; these coefficients ARE that sky.
     *
     * Diffuse irradiance is band-limited to l<=2, so it changes smoothly with
     * the sun and does not need recomputing every frame.  The cache key is the
     * sun direction and turbidity; the projection re-runs only once the sun has
     * moved past a small angular threshold. */
    float                   sky_sh9[9][3];
    bool                    sky_sh9_valid;
    jce_vec3                sky_sh9_sun;     /* sun dir the cache was built for */
    float                   sky_sh9_turbidity;

    /* Optional editor-supplied ambient override. When active and ToD is
     * inactive, replaces the renderer's hardcoded ambient before lights
     * are gathered each frame. */
    bool                    ambient_override_active;
    jce_vec3                ambient_override_color;
    float                   ambient_override_intensity;
    /* Last ambient this renderer RESOLVED, for the log-on-change line in
     * jce_sr_draw.c.  Environment lighting has to resolve identically in the
     * editor and in the shipped exe, and nothing else makes the resolved
     * value observable -- the last two divergences here were both found by
     * differencing screenshots, which says a difference exists but not
     * which of the three sources produced it. */
    bool                    amb_logged;
    jce_vec3                amb_logged_color;
    float                   amb_logged_intensity;
    const char             *amb_logged_source;
    /* Set via jce_sr_apply_resolved_ambient() (jce_sr_ambient_log.c), which
     * applies the ambient AND reports it.  One entry point on purpose: a
     * separate log call can drift from the value actually applied, and a
     * diagnostic that disagrees with the frame is worse than none. */

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
    /* The camera depth pre-pass produced VALID depth for THIS frame.
     *
     * Distinct from ssao_valid, which only says the render target exists.  The
     * target is deliberately kept across frames (it is shared by SSAO, SSR,
     * TAA velocity and the Hi-Z culls, and the renderer's policy for effect
     * buffers is hysteresis, not an immediate free), so after SSAO/SSR/TAA are
     * all switched off mid-session the handle stays valid forever.  Any
     * consumer that tested the HANDLE went on sampling a depth buffer frozen
     * at the last frame the pre-pass ran -- water absorbing against geometry
     * that had since moved, with no symptom at the moment of the toggle.
     *
     * Cleared every frame before the pre-pass gate; set by
     * sr_draw_depth_prepass.  Falsified if a frame that skips the pre-pass is
     * ever observed with this flag still true. */
    bool                       depth_prepass_frame;
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
    bgfx_uniform_handle_t      u_gbuffer_mat;
    bgfx_uniform_handle_t      u_gbuffer_albedo;  /* rgb = base colour, for SSGI */    /* x = roughness */

    /* TAA per-object MOTION VECTOR G-buffer: a THIRD color attachment on the
     * depth pre-pass FBO (RGBA16F).  When TAA is on, the prepass writes real
     * per-object (and per-bone, for skinned) screen motion into RG using the
     * SAME encoding as fs_motion_vec.sc ((curNDC-prevNDC)*0.5+0.5), so the TAA
     * resolve can reproject moving/animated geometry without ghosting.  Only
     * allocated when the velocity attachment is requested (taa_want_velocity);
     * UINT16_MAX / unused when TAA is off => byte-identical legacy path. */
    /* Base-colour ALBEDO, attachment 1 of the pre-pass, allocated with it.
     *
     * SSGI's bounce is albedo/PI * SUM(L cos); without this target the
     * RECEIVER'S OWN LIT COLOUR stood in for its albedo and counted the direct
     * lighting twice, brightest exactly where the error shows most.
     *
     * ALWAYS ALLOCATED, not gated on SSGI, and the index is why: making it
     * conditional shifts velocity from 2 to 1, and a fragment shader that
     * writes gl_FragData[1] would then put albedo in the velocity target on
     * the frames SSGI is off -- TAA ghosting with nothing in the TAA code to
     * explain it.  The alternative is a shader variant per combination, which
     * is what a keyword system would generate and this engine does not have
     * one.  Cost stated rather than hidden: one RGBA8 at full res, 8.3 MB at
     * 1080p. */
    bgfx_texture_handle_t      ssao_albedo_tex;
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
    /* GPU crowd instancing (JCE_CROWD_INSTANCE): every visible skinned
     * character's CURRENT world-space bone palette packed into ONE per-frame
     * RGBA32F texture (4 texels per bone), so a whole same-mesh crowd draws in a
     * single instanced submit (per-instance palette base in i_data0.x).  The
     * palette is world-space, so the SAME texture serves color, prepass, and
     * shadow.  bone_tex == invalid / feature off => byte-identical legacy path. */
    bgfx_texture_handle_t      bone_tex;             /* RGBA32F, bone_tex_w x bone_tex_h texels */
    uint16_t                   bone_tex_w;           /* texel width (fixed) */
    uint16_t                   bone_tex_h;           /* texel height (grows to fit) */
    uint32_t                   bone_tex_texel_cap;   /* allocated capacity (w*h texels) */
    uint32_t                   bone_tex_frame;       /* frame counter this texture was packed */
    float                     *bone_pack_buf;        /* CPU staging (texel_cap*4 floats) */
    bgfx_uniform_handle_t      s_bones;              /* sampler stage 4 (aliases s_emissive) */
    bgfx_uniform_handle_t      u_boneTexParams;      /* vec4: x=texW, y=texH */
    bgfx_program_handle_t      prog_pbr_skinned_inst;/* vs_pbr_skinned_inst + fs_pbr */
    bool                       crowd_inst_prog_tried;/* lazy program load attempted */
    bool                       crowd_inst_supported; /* RGBA32F vertex-texture sampling */
    /* ANIMATED crowd velocity: PREVIOUS frame's palettes packed at the SAME
     * per-instance bases into a second texture, so the instanced velocity
     * shader (vs_gbuffer_vel_skinned_inst) dual-skins for per-limb motion. */
    bgfx_texture_handle_t      bone_prev_tex;        /* RGBA32F, same dims as bone_tex */
    bgfx_uniform_handle_t      s_prevBonesTex;       /* sampler stage 5 */
    bgfx_program_handle_t      prog_gbuffer_vel_skinned_inst;
    bool                       crowd_vel_prog_tried;
    SrCrowdGroup               crowd_vel_groups[SR_CROWD_MAX_GROUPS];
    int                        crowd_vel_group_count;
    float                      crowd_vel_rough[SR_CROWD_MAX_GROUPS]; /* per-group roughness (first add) */
    SrCrowdGroup               crowd_groups[SR_CROWD_MAX_GROUPS];
    int                        crowd_group_count;    /* live groups this pass */
    SrCrowdGroup               bp_groups[SR_CROWD_MAX_GROUPS];  /* bind-pose (non-animating) */
    int                        bp_group_count;
    /* Bind-pose SHADOW batch: the depth-pass sibling of bp_groups, batched
     * per shadow view (cascade / local tile) and flushed on view change, so a
     * bind-pose crowd's per-char cascade shadow submits collapse to one
     * instanced depth draw per (model, cascade). */
    SrCrowdGroup               bp_sh_groups[SR_CROWD_MAX_GROUPS];
    int                        bp_sh_group_count;
    uint16_t                   bp_sh_view;           /* current bind-pose shadow batch view */
    /* ANIMATED crowd SHADOW batch (vs_shadow_skinned_inst + bone texture):
     * per-view accumulation of animating casters whose palette is packed this
     * frame — one skinned instanced depth draw per (model, cascade) replaces
     * the per-char skinned shadow submits of the crowd's animated subset. */
    bgfx_program_handle_t      prog_shadow_skinned_inst;
    bool                       crowd_sh_prog_tried;
    SrCrowdGroup               crowd_sh_groups[SR_CROWD_MAX_GROUPS];
    int                        crowd_sh_group_count;
    uint16_t                   crowd_sh_view;        /* current animated-crowd shadow batch view */
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
    /* Editor two-viewport pack skip: sr_pack_bone_palettes ran twice per
     * displayed frame (once per viewport) on IDENTICAL palettes (the pose is
     * advanced only on the first viewport, gated by skin_anim_gen).  When the
     * editor drives velocity frames, the 2nd call returns early WITHOUT
     * bumping bone_tex_frame, so every instance's crowd_palette_frame
     * equality still holds and viewport 2 keeps the instanced path. */
    uint32_t                   bone_pack_gen;
    bool                       velocity_frame_driven; /* editor calls begin_velocity_frame: gate anim once/frame */
    /* Camera frustum planes for the depth/velocity pre-pass.  That pass is
     * screen-space (SSAO/SSR/TAA-velocity), so off-screen entities contribute
     * nothing — culling them keeps the per-frame uniform writes (and bgfx's
     * Vulkan uniform scratch buffer) bounded on large scenes (else the scratch
     * buffer overflows -> crash).  Set once per pre-pass in sr_draw_depth_prepass. */
    jce_vec4                   prepass_cull_planes[6];
    /* The COLOUR pass's frustum, stashed where a per-draw consumer can reach
     * it.  It is extracted locally for the visibility step and was thrown away
     * one line later; static-batch member culling needs the same planes at
     * SUBMIT time, one level down, and re-deriving them there would be a
     * second answer to "what is visible this frame". */
    jce_vec4                   color_cull_planes[6];
    float                      color_cull_absn[6][3];
    bool                       color_cull_valid;
    /* |n| for the above, refreshed with it.  The positive-vertex selection is
     * a pure function of the plane normals, so hoisting it here turns the
     * per-entity test into multiply-add -- see jce_aabb_in_frustum_fast. */
    float                      prepass_cull_absn[6][3];
    SrPrevXform               *prev_xform;       /* per-entity prev world: grown open-
                                                  * addressing table (NULL until TAA on). */
    uint32_t                   prev_xform_cap;   /* slot count; grows with scene size */
    uint32_t                   prev_xform_count; /* live entries (load-factor grow trigger) */
    bool                       kindcache_on;     /* Fix #1: ecull[].render_kind valid this frame */

    /* SSR (screen-space reflections).  Shares the SSAO depth pre-pass; reads
     * the lit color (cfg->ssr_color_tex_handle) + reconstructs normals from
     * depth; the result is composited by the editor via
     * jce_scene_renderer_composite_ssr(). */
    JceSsr                    *ssr;
    JcePlanarReflection       *planar;           /* one mirrored render     */
    /* The water plane the reflection was rendered for.  A scene with two
     * water bodies at different heights gets ONE reflection, for the first
     * that asks: a second render per plane is a second full scene render,
     * and the honest place to say that is here rather than as a silent
     * artefact on the second lake. */
    float                      planar_plane_y;
    bool                       planar_wanted;
    /* A PLANAR REFLECTION PROBE asking instead of water.
     *
     * Same single mirrored render, same reason: a second probe is a second
     * full scene render.  When both a water surface and a probe ask in one
     * frame, WATER WINS -- it samples the texture in its own material and
     * would show a reflection of the wrong plane, where the probe's composite
     * simply does not draw.  A wrong picture beats no picture nowhere.
     *
     * probe_valid is cleared and re-established every frame from the scene,
     * unlike planar_wanted which a draw sets; a probe is not drawn by
     * anything, so there is nothing to register it at submit time. */
    bool                       planar_probe_valid;
    jce_vec3                   planar_probe_point;
    jce_vec3                   planar_probe_normal;
    jce_vec3                   planar_probe_box_c;
    jce_vec3                   planar_probe_box_h;
    float                      planar_probe_thickness;
    float                      planar_probe_angle_deg;
    float                      planar_probe_intensity;
    float                      planar_probe_max_rough;
    float                      planar_probe_edge_fade;
    /* THE MAIN CAMERA'S INVERSE VIEW-PROJECTION, kept here rather than read
     * from cur_view_proj, which is MAINTAINED ONLY WHEN TAA VELOCITY IS ON.
     *
     * Borrowing it cost an afternoon: with r.taa=0 the field holds whatever
     * some earlier frame or the other viewport left in it, so the composite
     * reconstructed every pixel at a warped world position.  The symptom was
     * not a wrong picture -- it was a mirror that appeared on a thin band of
     * distant floor and nowhere else, because only there did the stale matrix
     * happen to land within the on-plane tolerance.  That reads as "the
     * effect is weak", not as "the input is wrong".
     *
     * A field that another feature maintains conditionally is not a fact
     * about the frame. */
    jce_mat4                   planar_probe_inv_vp;
    bool                       planar_probe_inv_vp_valid;
    /* The aspect of the viewport that will SAMPLE the mirror.  The mirrored
     * render runs before the main one, so this is last frame's -- which is
     * exact except for the one frame after a viewport resize, the same
     * one-frame property the whole feature already has.  0 until a
     * non-reduced render has happened, and the mirror then falls back to its
     * target's own shape. */
    float                      last_viewport_aspect;
    JceSsgi                   *ssgi;             /* one diffuse bounce      */
    bool                       ssgi_active_frame;
    uint16_t                   ssgi_result_idx;
    bool                       ssr_active_frame; /* result valid this frame */
    uint16_t                   ssr_result_idx;   /* SSR reflection RT handle idx */
    /* Consecutive frames each effect has been unwanted while still holding
     * its render targets.  Used to release them after a settling period so a
     * flickering toggle cannot thrash the GPU allocator. */
    uint32_t                   ssao_idle_frames;
    uint32_t                   ssr_idle_frames;
    /* This frame's viewport, in pixels, captured at the top of
     * jce_scene_renderer_render from JceSceneRenderConfig.  It exists so
     * passes that need a SCREEN-SPACE quantity can compute one instead of
     * hard-coding a world-space number: the ribbon renderer's minimum width
     * used to be a flat 0.05 world units, which is 5% of the frame in a 1 m
     * shot and invisible in a 500 m one.  Same class of mistake as measuring
     * a mechanism's travel in metres and calling it legible. */
    uint32_t                   last_vp_w;
    uint32_t                   last_vp_h;
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
    bgfx_uniform_handle_t      u_csm_penumbra;   /* see pcss.sh */

    /* Dual shadow maps (JCE_SHADOW_DUAL): a SECOND CSM depth atlas holding ONLY
     * dynamic (mover) casters, a 2x2 tile grid of the 4 cascades in one texture,
     * re-rendered every frame with the SAME cascade VPs.  The static csm_tex[c]
     * then cache aggressively (movers no longer dirty them), and the PBR shader
     * min()s the two — so a moving object no longer forces re-rendering all the
     * static casters in its cascade (Unity Shadowmask / UE static-vs-movable).
     * Sampled via the s_shadowMap stage (dead under CSM); u_csmDynParams carries
     * {tiles=2, 1/atlas_size, enabled, 0}. */
    bgfx_texture_handle_t      dyn_csm_atlas_tex;
    bgfx_frame_buffer_handle_t dyn_csm_atlas_fbo;
    bgfx_uniform_handle_t      u_csm_dyn_params;
    uint16_t                   dyn_csm_atlas_size;   /* = shadow_map_size */
    bool                       dyn_csm_valid;        /* allocated this size */
    bool                       frame_dyn_csm_active; /* atlas rendered this frame */
    /* Shadow FILTER tier (x lane; see JceRenderPipelineDesc
       .shadow_filter_quality). Frame-constant uniform branch in
       fs_pbr/fs_terrain selecting 1-tap / 3x3 / full PCF — coherent for
       every fragment of a draw, so the untaken side's texture fetches are
       genuinely skipped on SM3+ hardware. */
    bgfx_uniform_handle_t      u_shadow_quality;
    float                      shadow_filter_tier; /* cached per frame */
    float                      sun_soft_deg_frame; /* ditto; 0 = off   */
    bool                       toon_allowed_frame; /* quality>=HIGH + valid pbr_toon */
    bool                       csm_valid;
    bool                       contact_shadow_frame;
    /* Underwater: set by sr_draw_water when the eye is under a body with
     * absorption authored.  Cleared every frame BEFORE the water pass, so a
     * body that stops being submerged stops tinting -- a sticky flag would
     * leave the whole screen underwater after the camera surfaced. */
    struct JceUnderwater      *underwater;
    bool                       underwater_frame;
    /* Sticky: this scene contains water with absorption authored, so the
     * base+17 view slot must be reserved even on frames the camera is
     * above the surface. */
    bool                       underwater_possible;

    /* ── Cloud shadows ───────────────────────────────────────────────────
     * A CPU-baked top-down transmittance map, uploaded to an R8 texture and
     * delivered through the SSAO target's blue channel.  Re-baked only when
     * the sun or the map centre have moved enough to change it: the bake is
     * hundreds of thousands of density taps, and it is exactly the kind of
     * low-frequency term that should not be recomputed per frame. */
    bgfx_texture_handle_t      cloud_shadow_tex;
    uint8_t                   *cloud_shadow_bytes;   /* res*res, upload staging */
    float                     *cloud_shadow_map;     /* res*res floats          */
    uint32_t                   cloud_shadow_res;
    float                      cloud_shadow_extent;
    float                      cloud_shadow_center[2];
    JceShadowSunHold           cloud_shadow_sun;     /* rebake deadband         */
    /* How far the cloud field has been carried by the wind, in metres of world
     * displacement, accumulated. double because it grows without bound and a
     * float loses its per-frame increment once the total is large: at 9 m/s
     * and 60 fps the increment is 0.15 m, which a float32 can no longer add to
     * a total past ~1.2e6 m -- about a day and a half, after which the clouds
     * would simply stop. Each consumer wraps this into its own period.
     *
     * Accumulated, not wind * elapsed_time: the direction and speed change
     * with the weather, and a product with absolute time teleports the whole
     * cloud layer the instant either one does. */
    double                     cloud_wind_offset[2];
    float                      cloud_shadow_wind[2]; /* offset the map was baked at */
    /* The pass in flight bakes into `back`; `map` stays bound and unchanged
     * until the pass completes and the two are swapped. A single buffer would
     * mean every sample during a pass reads a map that is part this window and
     * part the last one -- a seam that walks down the screen. */
    float                     *cloud_shadow_back;
    uint32_t                   cloud_bake_row;       /* next row; res = idle    */
    JceCloudShadowDesc         cloud_bake_desc;      /* frozen for the pass     */
    /* Storage for the desc's `noise` pointer: it is borrowed for the whole
     * pass, which spans frames, so it cannot live on a stack. */
    /* THIS frame's viewport aspect, set once at the top of the render.
     *
     * Two cull paths built their test frustum with a hard-coded 16:9 while
     * every other one derived it from the viewport, so in a docked editor
     * viewport wider than 16:9 the test frustum was NARROWER than the real
     * one and geometry still on screen at the left and right edges was culled.
     * jce_sr_draw.c records the same bug being fixed for streamed objects,
     * where the symptom was the whole streamed set shimmering every frame.
     *
     * One value, set once, so the next cull site cannot get it wrong either. */
    float                      frame_aspect;
    JceCloudNoiseParams        cloud_bake_noise;
    float                      dbg_raw_cloud_density;
    float                      cloud_bake_center[2];
    float                      cloud_bake_wind[2];
    float                      cloud_bake_extent;
    float                      cloud_bake_ms;        /* last bake, for the probe */
    bool                       cloud_shadow_valid;
    float                      underwater_sigma[3];
    float                      underwater_tint[3];
    JceCascadeBlendMode        csm_blend_mode;
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

    /* CSM shadow-map caching (UE-style "cached shadow maps"): when a cascade's
     * snapped light VP is bit-identical to the frame its csm_fbo[c] was last
     * rendered AND the shadow-caster set is unchanged, the cascade render
     * (clear+gather+submit) is SKIPPED — csm_fbo[c] retains valid depth. The
     * dominant shadow_gather cost (per-cascade caster gather, ~4.5ms on 10k
     * static casters) collapses to ~0 on static frames; texel-snapping keeps the
     * expensive far cascades' VP stable across many moving-camera frames too.
     * Correctness: caster_key (accumulated over shadow casters' xform_gen) +
     * struct_epoch + the dynamic-caster gate invalidate on ANY caster change;
     * the per-cascade VP compare invalidates on camera/light motion. */
    jce_mat4   shadow_cache_vp[JCE_CSM_MAX_CASCADES]; /* VP each csm_fbo[c] holds */
    bool       shadow_cache_valid[JCE_CSM_MAX_CASCADES];
    /* The sun the shadow maps are anchored to -- held still between moves
     * large enough to change a texel (jce_shadow_sun.h). */
    JceShadowSunHold shadow_sun_hold;
    uint64_t   shadow_cache_caster_key;   /* caster_key at last render */
    uint64_t   shadow_cache_static_caster_key; /* STATIC-only key at last render (dual) */
    uint64_t   shadow_cache_struct_epoch; /* structural_epoch at last render */
    uint32_t   shadow_cache_cascades;     /* csm.cascade_count at last render */
    uint16_t   shadow_cache_map_size;     /* shadow_map_size at last render */
    /* Per-frame caster-state signals, accumulated in the ecull build loop and
     * consumed by sr_draw_shadow_pass. */
    uint64_t   shadow_caster_key;         /* FNV-1a over (entity, xform_gen) of casters */
    /* Dual mode: key folding ONLY the STATIC (non-is_dyn_caster) casters, so a
     * mover's xform_gen churn does not void the static cascade cache (movers go
     * to the dynamic atlas).  The full shadow_caster_key still gates single-map. */
    uint64_t   shadow_static_caster_key;
    bool       shadow_has_dynamic_caster; /* any rigidbody/character/skeletal caster */
    /* Per-cascade dynamic gates: list of ecull indices of dynamic casters
     * (rebuilt with the full ecull build; appended by the L2 repair), and
     * whether each cascade CONTAINED a dynamic caster at its last render (a
     * mover leaving a cascade must dirty it once more to erase its shadow). */
    uint32_t  *shadow_dyn;
    uint32_t   shadow_dyn_count, shadow_dyn_cap;
    bool       shadow_cache_dyn_c[JCE_CSM_MAX_CASCADES];

    /* Local (spot/point) shadow atlas — P1. */
    bgfx_texture_handle_t      local_atlas_tex;
    bgfx_frame_buffer_handle_t local_atlas_fbo;
    bgfx_uniform_handle_t      u_local_shadow_map;    /* sampler stage 15 */
    bgfx_uniform_handle_t      u_local_shadow_vp;     /* MAT4[JCE_MAX_LOCAL_SHADOWS] */
    bgfx_uniform_handle_t      u_local_shadow_params; /* x=tiles/side y=1/atlas z=bias w=texel */
    bgfx_uniform_handle_t      u_local_shadow_bias;   /* VEC4: per-slot depth bias (lane=slot) */
    bgfx_uniform_handle_t      u_spot_shadow_slot;    /* VEC4: lane i = slot for spot i (-1=none) */
    bgfx_uniform_handle_t      u_point_shadow_slot;   /* VEC4[2]: 8 point lanes (-1=none) */
    bgfx_uniform_handle_t      u_point_cube_vp;       /* #7: MAT4[JCE_POINT_SHADOW_MAX*6] cube-face VPs */
    bgfx_uniform_handle_t      u_point_cube_base_slot;/* #7: VEC4[4]: 16 point lanes -> face-0 slot (-1) */
    bool                       local_atlas_valid;
    /* NOT cached, and the reason is worth keeping: a whole-pass skip that
     * returns before sr_draw_local_shadow_pass builds its tiles also skips the
     * per-frame uniform bookkeeping the shader depends on -- frame_local_vp[]
     * and frame_point_cube_base_slot[], the latter reset to -1 at the top of
     * the pass. Measured: the lit frame fell back to the no-shadow picture
     * (0.008% of pixels differing from the broken build, under a 0.025% noise
     * floor) while the depth tiles were, as far as the cache knew, still valid.
     * A correct cache has to skip the SUBMITS and keep the bookkeeping. */
    /* Per-frame local-shadow state, filled by sr_draw_local_shadow_pass.
       Spot + point lights share ONE atlas slot pool (max JCE_MAX_LOCAL_SHADOWS). */
    jce_mat4                   frame_local_vp[JCE_MAX_LOCAL_SHADOWS];
    float                      frame_local_bias_slot[JCE_MAX_LOCAL_SHADOWS]; /* per-slot depth bias */
    float                      frame_spot_slot[JCE_MAX_SPOT_LIGHTS];   /* spot i -> slot or -1 */
    float                      frame_point_slot[JCE_MAX_POINT_LIGHTS]; /* point j -> slot or -1 */
    uint32_t                   frame_local_count;
    float                      frame_local_bias;
    bool                       frame_local_active;
    /* #7 omnidirectional point shadows (opt-in). local_tiles = active atlas grid
       (2 default, 6 when point_cube_shadows on). frame_point_cube_vp = 6 face VPs
       per budgeted point light; frame_point_cube_base_slot[i] = face-0 atlas slot
       for point light i (-1 = no cube shadow -> shader falls back to legacy slot). */
    uint32_t                   local_tiles;
    bool                       point_cube_shadows;
    jce_mat4                   frame_point_cube_vp[JCE_POINT_SHADOW_MAX * JCE_POINT_CUBE_FACES];
    float                      frame_point_cube_base_slot[JCE_MAX_POINT_LIGHTS];

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
    /* Colour-pass skips for `disabled`; see JceSceneCullStats. */
    uint32_t                   stat_disabled_skipped;
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

    /* Parallel ecull-build scratch: the per-frame ecull[] build fans its
     * wcache-hit fast path out across the job system (hits are read-only ECS
     * probes + disjoint ecull[i] writes); misses are recorded per chunk and
     * replayed serially after the join (model/mesh resolve caches are not
     * thread-safe).  ecull_key_parts holds each chunk's commutative (XOR of
     * mixed per-caster hashes) shadow-caster-key part. */
    uint32_t                  *ecull_miss;        /* chunk-segmented miss indices */
    uint32_t                  *ecull_miss_counts; /* per-chunk miss counts        */
    uint64_t                  *ecull_key_parts;   /* per-chunk caster-key parts   */
    uint32_t                   ecull_par_cap;       /* entity cap of ecull_miss   */
    uint32_t                   ecull_par_chunk_cap; /* chunk cap of counts/parts  */

    /* Slice 2 (DOTS floor): trust cached kc_enabled on the wcache-hit path
     * while the scene-wide enable_gen is unchanged (latched per build; a flip
     * re-checks every hit entity that frame).  The latch also keys on the
     * scene pointer — a renderer serving a different scene (editor Play swap)
     * must not compare gens across scenes.  JCE_ENABLE_GEN=0 disables. */
    bool                       ecull_trust_enable;
    uint64_t                   ecull_last_enable_gen;
    const void                *ecull_last_enable_scene;

    /* Slice 4 (DOTS floor): frozen-frame ecull.  While the frame key —
     * (collect list_gen, xform_counter, enable_gen, structural_epoch, scene)
     * — is unchanged, ecull[] from last frame is provably identical, so the
     * whole build (parallel fan-out, caster-key rebuild, focus-AABB stores,
     * both amortized prunes) is skipped and the memoized caster key/dynamic
     * flag are replayed.  touched bookkeeping simply pauses with the build
     * (prunes are skipped too), resuming intact on the next live build.
     * The camera-dependent LOD/fade loop still runs every frame.
     * JCE_ECULL_FREEZE=0 disables. */
    bool                       ecull_frz_valid;
    uint64_t                   ecull_frz_list_gen;
    uint64_t                   ecull_frz_xform;
    uint64_t                   ecull_frz_enable;
    uint64_t                   ecull_frz_struct;
    const void                *ecull_frz_scene;
    uint64_t                   ecull_frz_caster_key;
    uint64_t                   ecull_frz_static_caster_key; /* dual: static-only */
    bool                       ecull_frz_dyn;

    /* Slice 5b (DOTS floor): the LOD/fade loop's constants are valid for this
     * collect list generation when no LODGroup exists and the fade clock is
     * stopped — the 150k walk is skipped until the list changes or a LODGroup
     * appears.  0 = constants not authoritative (live loop last frame). */
    uint64_t                   lod_consts_list_gen;
    /* LOD membership cache: the ecull indices that actually carry a
     * LODGroup, so the per-frame LOD pass is O(members) instead of
     * O(entities). Rebuilt when the entity list generation or the
     * scene's structural epoch changes -- the two events that can add
     * or remove a LODGroup. */
    int32_t   *lod_members;
    uint32_t   lod_member_cap;
    uint32_t   lod_member_count;
    uint64_t   lod_member_list_gen;
    uint64_t   lod_member_epoch;

    /* L2 (DOTS floor): entity→list-index map for incremental ecull repair —
     * open addressing, built lazily the first time a repair needs it and
     * valid while the collect list generation is unchanged. */
    struct SrEntityIdxSlot { uint32_t e; uint32_t eci; } *eidx; /* e==0: empty */
    uint32_t                   eidx_cap;      /* power of two */
    uint64_t                   eidx_list_gen; /* 0 = not built */

    /* Settings S1: change-driven resurrection of render-pipeline knobs that
     * previously had no consumer.  Boot behaviour is untouched; the first
     * RUNTIME change to a knob (panel Apply / set_knob / set_feature toggle)
     * latches its *_live flag and the resolved value participates in the
     * per-frame fold from then on. */
    bool                       rp_seen;            /* last-values valid */
    uint16_t                   rp_last_shadow_res;
    uint8_t                    rp_last_cascades;
    uint8_t                    rp_last_msaa;
    bool                       rp_last_taa;
    bool                       rp_shadow_live;     /* shadow_resolution engaged */
    bool                       rp_cascades_live;   /* csm_cascade_count engaged */

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
    /* Incremental rebuild gate: on a static frame the grid + noaabb[] from
     * last frame are still exact, so the O(casters x cells_per_obj) reset +
     * re-insert (~2.3ms/frame at 150k static casters) is skipped.  Same
     * triple invalidation the CSM cascade cache uses (caster_key over
     * (entity, xform_gen) + structural_epoch + the dynamic-caster gate);
     * grid values are ecull/list INDICES, whose frame-to-frame stability is
     * covered by structural_epoch (flecs table order only changes on
     * add/remove/reparent = structural edits) plus the count double-check. */
    uint64_t                   shadow_space_caster_key;
    uint64_t                   shadow_space_struct_epoch;
    int                        shadow_space_list_count;

    /* Optional global LOD group. When non-NULL every entity with a
     * resolved mesh has its mesh replaced by the pick at draw time.
     * MVP — per-entity LOD attachment lands when the ECS schema gains
     * a LodGroup component. */
    const JceLodGroup         *global_lod;
    /* Per-entity previous-LOD-level memory for hysteresis (large-world-opt
     * P1 #6).  Open-addressed (linear-probe) table keyed by entity id, grown on
     * demand — REPLACES the former fixed int8_t[1024] mask that aliased ~50×
     * over at 53k entities (so distant buildings sharing a bucket thrashed each
     * other's LOD level every frame).  Stored value is (level+1) so 0 means
     * "no record" (first selection takes the nominal, hysteresis-free pick);
     * a value of count+1 parks a culled entity at the last level for a clean
     * return swing.  Pruned to the live set so it can't grow without bound. */
    struct SrLodStateEntry    *lod_state;     /* {entity, value, touched} table */
    uint32_t                   lod_state_cap;  /* power of two; 0 until first use */
    uint32_t                   lod_state_count;
    /* ── Streaming LOD cross-fade (Direction B; dithered detail fade-in) ──
     * Parallel open-addressed table (same hashing/growth/prune as lod_state)
     * recording the first-seen phase-clock time per entity, so a freshly
     * streamed detail entity dithers in over JCE_SR_FADE_DURATION while the
     * resident HLOD proxy stays up.  fade_time is a renderer-owned phase clock
     * advanced by dt_sec (mirrors water_time/grass_time): 0 in still editor
     * previews so the fade is dormant there.  u_lod_fade is the per-draw
     * uniform; DEFAULT-set to inactive {1,0,0,0} each color pass so non-fading
     * draws are byte-identical.  All inert until something streams in. */
    struct SrFadeStateEntry   *fade_state;    /* {entity, first_seen, touched} */
    uint32_t                   fade_state_cap; /* power of two; 0 until first use */
    uint32_t                   fade_state_count;
    float                      fade_time;      /* accumulated phase seconds */
    bgfx_uniform_handle_t      u_lod_fade;     /* vec4 {fade, 0, 0, active} */
    uint32_t                   stat_lod_picks[JCE_LOD_MAX_LEVELS];
    uint32_t                   stat_lod_culled;
    bool                       stat_lod_enabled;
    /* Octahedral impostor terminal-LOD stats (P2 #10), reset each scene render:
     * how many far-cards were drawn and in how many instanced draws. */
    uint32_t                   stat_impostor_cards;
    uint32_t                   stat_impostor_draws;

    /* Terrain per-chunk draw stats (P1-terrain-lod), reset each scene render. */
    uint32_t                   stat_terrain_chunks_total;
    uint32_t                   stat_terrain_chunks_drawn;
    uint32_t                   stat_terrain_chunks_culled;
    uint32_t                   stat_terrain_shadow_calls;
    uint32_t                   stat_shadow_ladder;
    uint32_t                   stat_terrain_shadow_drawn;
    uint32_t                   stat_terrain_shadow_culled;

    /* Render-queue stats — accumulated across all flushes this frame
     * (shadow + main mesh pass).  Reset at start of each scene render. */
    JceRenderQueueStats        stat_rq;
    bool                       stat_rq_active;
    JceSceneGpuDrivenStats     stat_gpu_driven;

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
    /* r.point_shadows — OPT-IN omnidirectional point-light cube shadows (#7).
     * Read each frame so the console toggles it live; OR'd with the
     * JCE_POINT_CUBE_SHADOWS env for headless. NULL if registration failed. */
    JceCvar                  *cv_point_shadows;
    /* True for the current frame when the clustered path is BOTH enabled AND
     * the fs_pbr_fwdplus variant program loaded — gates the per-submit
     * jce_forwardplus_bind in the material bind callbacks. */
    bool                      fp_active_frame;

    /* ── Temporal Anti-Aliasing (r.taa) ──────────────────────────────────
     * Persistent jitter + previous-camera state.  Zero-init valid.  Driven
     * by the CALLER via jce_scene_renderer_taa_begin_frame / _end_frame around
     * the main colour-pass matrix setup + postfx apply.
     *
     * This said "the editor/runtime caller" until 2026-09-22, and the runtime
     * half was never true: taa_begin_frame has exactly one caller in the whole
     * tree, the editor Scene view, and the editor Game View enables TAA on its
     * own pipeline directly.  No shipped build reaches either.  Measured on
     * examples/snake_seven: forcing r.taa=0 on both sides took the same static
     * frame from 12.50%% of pixels differing to 2.98%%.
     * cv_taa is the opt-in console toggle (default OFF → no jitter, no TAA
     * passes → byte-identical to the legacy FXAA path).  NULL if registration
     * failed. */
    JceTaaState               taa_state;
    JceCvar                  *cv_taa;
    /* r.upscaler: dynamic-resolution upscaler for the editor Scene View —
     * 0=Off (bilinear), 1=RCAS spatial sharpen (default), 2=TSR temporal.
     * Read live each frame; JCE_TSR / JCE_RCAS env force the boot value. */
    JceCvar                  *cv_upscaler;

    /* Skybox / IBL state. */
    JceSkybox               *skybox;
    JceIblData              *ibl_data;
    bgfx_texture_handle_t    brdf_lut;
    bgfx_uniform_handle_t    u_ibl_irradiance;
    bgfx_uniform_handle_t    u_ibl_prefilter;
    bgfx_uniform_handle_t    u_ibl_brdf_lut;
    bgfx_uniform_handle_t    u_ibl_params;
    char                     skybox_hdr_path[256];
    bool                     skybox_use_as_ibl;  /* draw sky, don't light from it */
    bool                     skybox_active;
    /* ibl_enabled false => fs_pbr falls back to the FLAT authored ambient. */
    bool                     ibl_enabled;
    float                    skybox_exposure;
    float                    skybox_rotation;
    bool                     postfx_tonemap_active;
    /* Stage-1a.5: track the last resolved LUT path to avoid per-frame
     * texture loads (the 3D texture is re-resolved only on path change). */
    char                     last_lut_path[256];
    /* ── Async IBL bake ───────────────────────────────────────────────
     * The irradiance+prefilter convolution runs as a structured task so a
     * cache-miss does not freeze the main thread for several seconds. The
     * worker produces a CPU buffer; the main thread uploads it to GPU and
     * swaps it in when ready (the scene renders with fallback ambient until
     * then). See sr_scan_skybox. */
    struct SrIblJob         *ibl_job;        /* in-flight job, NULL = none */
    JceAsyncTask            *ibl_task;       /* worker for ibl_job */
    char                     ibl_job_hdr[256]; /* HDR the in-flight bake is for */
    /* The procedural sky's own IBL.
     *
     * A scene with no HDR skybox got NO environment specular at all: the whole
     * IBL path is keyed on an hdr_path changing, so every scene lit by the
     * analytic sky -- which is every scene with a day/night cycle -- had
     * metal, water and wet ground reflecting nothing. The sky is CPU-
     * evaluable (jce_sky_radiance is pure math), so it can feed exactly the
     * same bake the HDR path uses.
     *
     * `sky_ibl_key` identifies what the CURRENT ibl_data was baked for, and
     * doubles as the staleness key for an in-flight sky bake -- ibl_job_hdr
     * cannot serve, because a sky bake has no HDR path to compare. */
    char                     sky_ibl_key[96];
    uint64_t                 sky_ibl_t0;       /* submit stamp, main thread   */
    float                    sky_ibl_bake_ms;  /* submit -> collected, ms     */
    float                    sky_ibl_work_ms;  /* the convolution itself, ms  */
    uint32_t                 sky_ibl_bakes;    /* how many, this process     */
    bool                     ibl_job_is_sky;

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
    bool gi_probe_active, gi_probe_box_proj; jce_vec3 gi_probe_box_center, gi_probe_box_half;

    /* Baked light probes: frame fallback + the set (jce_sr_light_probe.h). */
    float                    gi_sh9[9][3];
    bool                     gi_sh9_active; JceSrProbeSet *lp_set;

    bgfx_uniform_handle_t    u_sh9;       /* 9 vec4: SH9 RGB coeffs */
    bgfx_uniform_handle_t    u_gi_probe_box, u_gi_params; /* x=sh9 on, y=probe intensity */

    /* ── Per-fragment aerial-perspective fog (Stage 1a.3) ──────────────
     * Pure-ALU fog composited in the lit body / terrain shader (fog_apply.sh).
     * Captured once per frame from the scene's existing fog_* settings + the
     * sun direction, then uploaded in BOTH per-submit bind paths (always-set,
     * mirroring u_iblParams) so every sorted draw — not just the first —
     * receives it. NONE mode (fog_mode 0) => params.x=0 => shader no-op. */
    bgfx_uniform_handle_t    u_fog_params;     /* vec4: x=mode,y=density,z=start,w=end */
    bgfx_uniform_handle_t    u_fog_color;      /* vec4: xyz=horizon, w=strength        */
    bgfx_uniform_handle_t    u_fog_color_sun;  /* vec4: xyz=warm,   w=height_falloff   */
    bgfx_uniform_handle_t    u_fog_height;     /* vec4: x=height_origin, yzw reserved  */
    struct {
        float params[4];     /* mode, density, start, end */
        float color[4];      /* horizon.rgb, strength      */
        float color_sun[4];  /* warm.rgb, height_falloff   */
        /* x = fog_height_origin; yzw reserved. Separate from the three above
         * because all twelve of their floats are spoken for -- see the comment
         * on u_fogHeight in engine/shaders/pbr/fog_apply.sh. */
        float height[4];
    } fog_frame;

    /* ── Look Profile GPU snapshot (stylized slice plan 02) ────────────
     * Built once per frame in jce_scene_renderer_render from the scene's
     * JceSceneRenderingSettings, gated by stylized_look.  When the gate is
     * off or values are neutral, look_gpu is the algebraic-identity packing
     * and the shaders no-op.  Uploaded by sr_bind_baked_gi (the single lit
     * submit funnel) so all three driver paths inherit it for free. */
    bgfx_uniform_handle_t    u_look_wrap;        /* {wrap, rimPow, rimInt, toonFlag} */
    bgfx_uniform_handle_t    u_look_rim;         /* {rimColor.rgb, pad} */
    bgfx_uniform_handle_t    u_look_hemi_ground; /* {groundColor.rgb, hemiEnabled} */
    struct {
        float wrap[4];   /* {wrap_factor, rim_power, rim_intensity, toon_flag} */
        float rim[4];    /* {rim_color.rgb, 0} */
        float hemi[4];   /* {ground_color.rgb, hemi_enabled} */
    }                        look_gpu;
    bool                     look_gpu_active;     /* gate latch for this frame */

    /* Sprite batch for 2D sprite entities. */
    JceSpriteBatch          *sprite_batch;

    /* Model / animation cache. */
    SrModelCache             model_cache[SR_MODEL_CACHE_MAX];
    SrModelCache            *model_lookup_hint;
    int                      model_inflight;   /* concurrent async model decodes */
    uint32_t                 model_upload_frame;
    bool                     model_upload_frame_valid;
    SrAnimInstance           anim_inst[SR_ANIM_INSTANCE_MAX];
    /* O(1) entity -> anim_inst slot index (open addressing, linear probe,
     * capacity 2x the slot cap, power of two).  Value is slot+1 (0 = empty).
     * sr_find_anim_instance was a linear scan over SR_ANIM_INSTANCE_MAX and
     * is called from per-entity per-pass hot paths (bind-pose color/shadow/
     * velocity gates + anim update) — at 4000 skinned chars x ~6 calls/frame
     * x 256 slots that was ~6M comparisons per frame, quadrupled by the
     * 64 -> 256 cap raise.  Maintained at the slot-assignment points in
     * jce_sr_anim.c; slots are only released wholesale (renderer destroy). */
    uint16_t                 anim_idx[SR_ANIM_INSTANCE_MAX * 2u];
    uint32_t                 anim_idx_keys[SR_ANIM_INSTANCE_MAX * 2u];
    /* Same-pattern O(1) index for the 2D sprite-animator slots (identical
     * lifecycle: slots release only wholesale at renderer destroy). */
    uint16_t                 sprite_idx[SR_SPRITE_ANIM_MAX * 2u];
    uint32_t                 sprite_idx_keys[SR_SPRITE_ANIM_MAX * 2u];
    bool                     anim_cache_full_warned; /* once-per-renderer cap warning */
    /* VRAM ceiling (large-world-opt): monotonically-increasing frame counter
     * bumped once per render; sr_get_model stamps the resolved slot's
     * last_used_frame with this so eviction can tell a model not referenced
     * for N frames from one in active use this frame.  model_evict_budget is
     * the per-renderer model-VRAM ceiling in bytes (0 = no eviction); when the
     * resident model VRAM exceeds it, jce_scene_renderer_evict_models frees the
     * oldest unreferenced models (runtime mode only). */
    uint64_t                 model_frame;
    uint64_t                 model_evict_budget;
    uint32_t                 model_evicted_count;  /* lifetime model evictions */

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
        uint32_t        path_hash; /* FNV-1a of path (+colour space) → O(1) key */
        /* PART OF THE KEY, not cargo.  The same PNG can legitimately be one
         * material's albedo (sRGB-encoded colour) and another's mask (linear
         * data); they need two GPU textures because the decode happens in the
         * sampler.  Sharing one would give whichever material asked first. */
        bool            srgb;
        JceTexture      tex;
        bool            used;
        bool            failed;  /* tried, but loader returned invalid */
        bool            pending; /* async decode in flight */
        JceAsyncTask   *task;    /* structured decode task */
        struct SrTexJob *job;    /* worker job (owns CPU result) */
    } tex_cache[SR_TEX_CACHE_MAX];
    int tex_cache_count;
    int tex_inflight;            /* concurrent async decodes */

    /* Persistent open-addressing index (path_hash -> tex_cache idx) so the
     * runtime texture resolve is O(1) instead of an O(tex_cache_count) strncmp
     * scan (the model cache beside it is already hashed — see sr_get_model).
     * The cache is insert-only for the renderer's life (no per-frame reset, no
     * deletes), so a slot stores idx+1 (0 = empty, calloc-friendly). */
    int32_t tex_hash[SR_TEX_HASH_SIZE];

    /* What a .mat.json gives this renderer, keyed by material path so a
     * per-frame draw never re-reads the file.  `program` is the Shader Graph
     * shader it declares (UINT16_MAX = none), owned by jce_pbr_material.c —
     * references only, never destroyed here.  `albedo` is the base-colour
     * image jce_sr_ribbon.c samples; "" is an ANSWER, not "not asked". */
    struct {
        char            path[256];
        char            albedo[256];
        JceShaderHandle program;   /* UINT16_MAX = no custom shader */
        bool            used;
        bool            resolved;    /* program load attempted (cache negatives) */
        bool            albedo_done; /* albedo resolution attempted */
    } prog_cache[SR_MAT_PROG_CACHE_MAX];
    int prog_cache_count;

    /* PostFX pipeline (owned). */
    JcePostFXPipeline       *postfx_pipeline;

    /* Project-authored full-screen stages.  Pipelines are lazy and keyed by
     * entity inside each viewport so temporal history never crosses cameras or
     * effects when deterministic sort order changes. */
    SrFullscreenEffectSlot  fullscreen_effects[JCE_SR_VIEWPORT_SLOTS]
                                              [JCE_SR_FULLSCREEN_EFFECT_CAP];
    JceSceneFullscreenEffectStageStatus
                              fullscreen_status[JCE_SR_VIEWPORT_SLOTS];
    jce_mat4                 fullscreen_prev_vp[JCE_SR_VIEWPORT_SLOTS];
    bool                     fullscreen_prev_vp_valid[JCE_SR_VIEWPORT_SLOTS];
    float                    fullscreen_elapsed[JCE_SR_VIEWPORT_SLOTS];
    uint32_t                 fullscreen_last_frame[JCE_SR_VIEWPORT_SLOTS];
    bool                     fullscreen_frame_valid[JCE_SR_VIEWPORT_SLOTS];
    char                     fullscreen_shader_dir[1024];

    /* Terrain mesh cache (path -> terrain + per-chunk meshes).  Terrain draws
     * chunk-by-chunk with frustum culling and distance LOD; meshes rebuild only
     * when their required LOD changes or the terrain is edited. */
    struct {
        char                  path[256];
        JceTerrain           *terrain;
        bgfx_texture_handle_t splat_tex;
        bool                  used;
        bool                  failed;
        bool                  splat_uploaded;
        /* Revision of the BORROWED JceTerrain this slot's chunk meshes and
         * splat texture were derived from.  The grid itself belongs to the
         * scene's terrain cache; when it moves, everything below is stale. */
        uint64_t              revision;

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
        /* large-world #4: per-chunk splat GPU texture for a TILED terrain (the
         * monolithic splat_tex above is invalid then).  Built lazily from the
         * terrain's tile splat block when chunk_size == tile_dim (chunk i ↔ tile
         * i).  idx==UINT16_MAX until built. */
        bgfx_texture_handle_t *chunk_splat_tex; /* [chunk_count] or NULL    */
    } terrain_cache[16];
    struct SrTerrainMaterialCache *terrain_material_cache;

    /* Vegetation scatter cache (P0 foliage): per-entity deterministic scatter
     * (jce_foliage_scatter) of an instanced mesh over the terrain.  Rebuilt only
     * when the component params change (param_hash) so per-frame cost is just N
     * model draws (GPU instancing is a perf follow-up).  Freed on destroy. */
    struct {
        bool                used;
        JceEntity           entity;
        char                mesh_path[256];
        uint32_t            param_hash;     /* seed/density/area/slope/scale */
        /* Authored align_to_normal for this scatter, captured when the
         * instances were built.  Stored on the slot rather than threaded
         * through every draw path because the instance matrix is built
         * from four different call sites (tile residency, LOD cull, and
         * two instance-packing loops) with different things in scope. */
        float               align_to_normal;
        JceFoliageInstance *insts;
        uint32_t            inst_count;
        uint32_t            inst_cap;
        jce_mat4           *roots;          /* per-instance world matrices (built */
        uint32_t            roots_cap;      /* at rebuild) for one instanced submit */
        /* 千万 S1: persistent GPU instance buffer — `roots` uploaded ONCE here
         * (on scatter / param change) so the per-frame draw binds it directly
         * with zero CPU copy. inst_vb_hash tracks the param_hash it holds; a
         * mismatch (or invalid handle) triggers a one-time (re)upload. */
        bgfx_dynamic_vertex_buffer_handle_t inst_vb;
        uint32_t            inst_vb_hash;
        uint32_t            inst_vb_count;
        /* 千万 S2: GPU compute frustum-cull of `inst_vb` → compacted survivors +
         * one indirect draw, so the GPU rasterises only the in-frustum subset
         * (GPU cost ∝ visible, not N).  Persistent, grown with inst_count. */
        bgfx_dynamic_vertex_buffer_handle_t cull_visible;  /* compacted survivor mat4s */
        bgfx_dynamic_index_buffer_handle_t  cull_counter;  /* 1 uint survivor count (IB pool — see ensure) */
        bgfx_indirect_buffer_handle_t       cull_indirect; /* 1 drawIndexedIndirect el */
        uint32_t            cull_cap;          /* cull_visible slot capacity */
        /* 千万 S4/S5 LOD-in-cull: ONE band-PARTITIONED survivor buffer (band b
         * owns slots [b*cap_band, (b+1)*cap_band)), one counter buffer (uint per
         * band) and one indirect buffer (element per band) — a single cull
         * dispatch (per tile) classifies every survivor into its distance band,
         * then one drawIndexedIndirect per band draws that band's reduced LOD
         * with startInstance = partition base.  Kept SEPARATE from the
         * single-band cull_* above so the primitive S2 path is untouched. */
        bgfx_dynamic_vertex_buffer_handle_t cull_lod_visible;  /* bands × cap_band mat4s */
        bgfx_dynamic_vertex_buffer_handle_t cull_lod_counter;  /* uint per band          */
        bgfx_indirect_buffer_handle_t       cull_lod_indirect; /* element per band       */
        uint32_t            cull_lod_cap;      /* per-band partition capacity */
        uint8_t             cull_lod_bands;    /* bands actually created (0 = none) */
        /* 千万 S5 tiled paging (want > JCE_FOLIAGE_MAX_INSTANCES): the field is a
         * grid of TILES, each deterministically re-scatterable on demand
         * (seed ^ tile hash) — only frustum-visible tiles within the far draw
         * distance hold a resident roots VB (LRU beyond the budget), so a
         * 10M-instance AUTHORED field costs only its visible neighbourhood.
         * Non-tiled scatters keep tiles == NULL and the legacy single-VB path. */
        struct SrFoliageTile {
            bgfx_dynamic_vertex_buffer_handle_t vb;   /* resident roots (COMPUTE_READ) */
            uint32_t count;        /* instances (valid when resident)   */
            uint32_t last_used;    /* frame stamp for LRU eviction      */
            jce_vec3 mn, mx;       /* conservative world AABB           */
            bool     resident;
        } *tiles;
        int      tiles_x, tiles_z;   /* tile grid dims (0 = non-tiled)  */
        float    tile_w, tile_h;     /* tile world size                 */
        uint32_t tiles_resident;     /* resident VB count (LRU budget)  */
        JceFoliageScatterParams tile_params; /* snapshot for per-tile regen
                                              * (mask fields nulled)    */
        jce_vec3 tile_origin;        /* scatter entity origin           */
        /* 千万 S4: distance-LOD bands for the MODEL scatter path.  Instances are
         * counting-sorted by camera distance into contiguous LOD bands in
         * `lod_sorted`; each band is one instanced submit at its LOD level, so
         * distant heavy meshes render reduced-LOD (or impostor) geometry — GPU
         * vertex cost ∝ detail × distance.  Re-partitioned only when the camera
         * moves past lod_cam_eps (O(1) steady-state, like S1). */
        jce_mat4           *lod_sorted;        /* roots reordered by LOD band   */
        uint32_t            lod_sorted_cap;
        uint32_t            lod_band_start[JCE_FOLIAGE_LOD_BANDS + 1]; /* offsets */
        uint8_t             lod_bands;         /* bands actually populated       */
        bool                lod_valid;         /* partition matches lod_cam_pos  */
        jce_vec3            lod_cam_pos;       /* camera pos the partition used  */
    } foliage_cache[16];

    /* Terrain shader uniforms (created lazily on first terrain submit). */
    bgfx_uniform_handle_t u_terrain_params;
    bgfx_uniform_handle_t u_terrain_tile_uv;  /* large-world #4: per-tile splat UV remap */
    /* Stage 3 on every lit surface. Terrain has all sixteen occupied and 3
     * held a declared-but-never-bound s_aoMap, so 3 is the only stage it
     * could give up -- which makes 3 the stage the others must match. */
    bgfx_uniform_handle_t u_weather_surface; /* x=wetness y=snow                   */
    bgfx_uniform_handle_t s_terrain_ao;      /* stage 1 -- was a dead s_metalRough */
    bgfx_uniform_handle_t s_cloud_shadow;
    bgfx_uniform_handle_t u_cloud_shadow;
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
        bool      ocean_mesh;   /* which TOPOLOGY the cache holds      */
        float     size_z;
        JceMesh  *mesh;         /* flat grid; displaced in vs_water    */

        /* ── FFT ocean (Tessendorf) per-entity GPU state (water_mode==FFT) ─
         * ONLY the GPU resource lives here.  The simulation itself belongs to
         * the scene's JceWaterField, which the renderer uploads FROM -- see
         * jce_water_field.h.  This cache used to own a JceWaterFft plus a copy
         * of every spectrum parameter, which made the renderer a second,
         * independent simulator of the same water; the drawn surface and the
         * one the physics solver sampled then disagreed by construction.
         *
         * fft_tex is a MUTABLE displacement texture (created once, refreshed
         * each frame with bgfx_update_texture_2d — mirrors the Forward+
         * dynamic-texture lifecycle), torn down on eviction and destroy. */
        bgfx_texture_handle_t  fft_tex;
        /* Second cascade, same lifecycle.  Two textures rather than one wider
         * one: the cascades have DIFFERENT resolutions in general, and packing
         * them together would force the smaller to the larger's size. */
        bgfx_texture_handle_t  fft2_tex;
        int                    fft_res;        /* resolution fft_tex was sized for */
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
    bgfx_uniform_handle_t s_water_disp;
    bgfx_uniform_handle_t s_water_disp2;  /* second FFT cascade */       /* FFT displacement texture (VS fetch) */
    /* The disturbance layer's height grid, uploaded once per frame and shared
     * by every water body -- there is one per scene, not one per body. */
    bgfx_uniform_handle_t s_water_ripple;
    bgfx_uniform_handle_t u_water_ripple;
    bgfx_texture_handle_t water_ripple_tex;
    int                   water_ripple_res;
    uint32_t              water_ripple_frame;
    bool                  water_ripple_frame_valid;
    bgfx_uniform_handle_t s_water_data;       /* STYLIZED shore-distance data map    */
    bgfx_uniform_handle_t s_water_planar;     /* planar reflection colour            */
    bgfx_uniform_handle_t u_water_planar;     /* x=on, y=intensity                   */
    bgfx_uniform_handle_t u_water_planar_vp;  /* the mirror render's view-projection */
    bgfx_uniform_handle_t s_water_depth;      /* opaque scene depth (absorption)     */
    bgfx_uniform_handle_t u_water_absorb;     /* sigma.rgb + underwater flag         */
    bgfx_uniform_handle_t u_water_absorb_tint;
    bgfx_uniform_handle_t u_water_depth_params;
    bgfx_uniform_handle_t u_water_caustics;
    bgfx_uniform_handle_t s_water_caustic_disp;
    bgfx_uniform_handle_t u_water_shore;
    bgfx_uniform_handle_t u_foliage_hashed_alpha;
    /* Sky stylisation grade (jce_sky_stylise.h).  Bound every frame; the
     * enable in .w is what makes an unstylised sky bit-identical. */
    /* Populated from the scene each frame (the sky draw has no scene access).
     * Defaults to identity so a renderer whose scene never set it still
     * renders unstylised rather than black. */
    JceSkyStylise         sky_stylise;
    bgfx_uniform_handle_t u_sky_stylise;
    bgfx_uniform_handle_t u_sky_tint_shadow;
    bgfx_uniform_handle_t u_sky_tint_mid;
    bgfx_uniform_handle_t u_sky_tint_high;
    /* ── Toon character (stylized-slice §5.6) ──────────────────────────
     * Four uniforms + two per-frame scratch arrays for the pre-submit cb.
     * All created eagerly in jce_scene_renderer_create; destroyed in destroy. */
    bgfx_uniform_handle_t u_toon_params;     /* vec4 bands,thresh,rim_power,rim_int */
    bgfx_uniform_handle_t u_toon_rim_color;  /* vec4 rgb,pad                        */
    bgfx_uniform_handle_t u_outline_params;  /* vec4 width,_,_,_                    */
    bgfx_uniform_handle_t u_outline_color;   /* vec4 rgb,1                          */
    /* Per-frame scratch: stashed by the skinned-draw site, read by the
     * pre-submit cb (sr_model_toon_presubmit_cb) for EVERY primitive submit. */
    float                 toon_params_frame[4];     /* bands,thresh,rim_pow,rim_int  */
    float                 toon_rim_color_frame[4];  /* rgb,pad                       */

    /* ── Grass Field (GPU-instanced procedural blades + wind, Stage 1b.6) ──
     * Dedicated module (NOT the 4096-cap foliage loop, NOT fs_pbr_body).  One
     * SHARED blade mesh (grass_blade) is instanced once per field via the
     * stride-80 tinted instance layout; placement reuses jce_foliage_scatter
     * cached per entity.  Program + uniforms load lazily on first grass submit.
     * grass_time is a per-renderer phase clock advanced each render. */
    struct {
        bool                used;
        JceEntity           entity;
        uint32_t            param_hash;     /* scatter-shaping fields */
        JceFoliageInstance *insts;
        uint32_t            inst_count;
        uint32_t            inst_cap;
        /* Pre-packed stride-80 instance bytes (mat4 + tint), built once per
         * param_hash.  Blade TRS + tint are static (wind is shader-side via
         * u_grass_time), so the color pass bulk-copies these instead of
         * rebuilding the per-blade matrix every frame (was ~1.1 ms/frame). */
        uint8_t            *packed;
        uint32_t            packed_cap;     /* bytes */
        /* Spatial grid for per-frame frustum + fade-distance CELL culling.
         * The packed buffer is re-ordered blade-by-cell at rebuild time so a
         * cell's blades occupy the contiguous byte range
         * [cell_start[c], cell_start[c+1]) — the color pass frustum/distance-
         * tests each cell (view-independent grid, works for scene + game views)
         * and submits only the visible cells, so GPU cost ∝ on-screen blades,
         * not the whole (now 2× denser) field.  grid_gx*grid_gz <= cap. */
        uint16_t            grid_gx, grid_gz;
        float               grid_min_x, grid_min_z, cell_size;
        float               field_ymin, field_ymax;   /* whole-field Y span */
        uint32_t            cell_start[JCE_GRASS_CELL_CAP + 1u]; /* blade prefix */
        /* Persistent GPU instance buffer (mirrors the foliage 千万 S1 path):
         * the packed stride-80 bytes are uploaded ONCE per param_hash, and the
         * per-frame cell-run submits bind [start,n) slices with ZERO CPU copy
         * — the transient alloc+memcpy per visible run is the fallback only.
         * inst_vb_count==0 <=> no buffer (a zeroed handle LOOKS valid). */
        bgfx_dynamic_vertex_buffer_handle_t inst_vb;
        uint32_t            inst_vb_hash;
        uint32_t            inst_vb_count;  /* blades resident in inst_vb */
        /* THIS field's blade, built from its authored blade_height /
         * blade_width / cards.
         *
         * There used to be one shared mesh for every field, built once from
         * the literals (0.4, 0.05, 4). The three controls were live in the
         * inspector, parsed, serialized, folded into the scatter param hash
         * and used to size the pick proxy and the selection outline -- and the
         * geometry ignored all three. Dragging blade height re-ran the whole
         * scatter, repack and counting sort every frame of the drag, moved the
         * click box, and changed no pixel.
         *
         * A blade is a handful of cards, so a mesh per field costs nothing
         * next to the instance buffer already on this slot. Rebuilt only when
         * one of the three changes, which is why the built-with values are
         * kept beside it rather than re-read from the component. */
        JceMesh            *blade;
        float               blade_h, blade_w;
        int                 blade_cards;
    } grass_cache[16];
    JceMesh              *grass_blade;       /* fallback blade, shared */
    bgfx_program_handle_t prog_grass;
    bool                  grass_prog_tried;  /* load attempted (success or not) */
    float                 grass_time;        /* accumulated phase seconds */
    bgfx_uniform_handle_t u_grass_time;      /* x=time */
    bgfx_uniform_handle_t u_grass_wind;      /* xy=dir, z=speed, w=amplitude */
    bgfx_uniform_handle_t u_grass_color;     /* root.rgb (vec4[0]) + tip.rgb (vec4[1]) */
    bgfx_uniform_handle_t u_grass_fade;      /* x=fade_start, y=fade_end, z=hue_jitter */
    bool                  grass_enabled;     /* project gate: JceRenderSettings.grass_enabled */

    /* Foliage clusters (stylized billboard canopies/bushes): per-entity
     * sampled shell cache (positions+normals packed as instance data) +
     * the shared quad + program.  Rebuilt on param-hash change only. */
    struct {
        JceEntity  entity;
        bool       used;
        uint32_t   param_hash;
        uint32_t   count;
        float     *inst;        /* count * 8 floats: pos.xyz,scale, nrm.xyz,phase */
        uint32_t   inst_cap;    /* floats */
    } fcluster_cache[48];
    JceMesh              *fcluster_quad;        /* shared 1x1 billboard card */
    JceMesh              *fcluster_shadow_sphere; /* unit sphere: soft blob shadow proxy */
    bgfx_program_handle_t prog_fcluster;
    bgfx_program_handle_t prog_fcluster_shadow;  /* vs_foliage + fs_foliage_shadow:
                                                  * REAL alpha-tested wind-animated
                                                  * leaf shadows (sphere = fallback) */
    bool                  fcluster_prog_tried;
    float                 fcluster_time;
    bgfx_uniform_handle_t u_foliage_time;      /* x=time */
    bgfx_uniform_handle_t u_foliage_colors;    /* vec4[4]: shadow/mid/high/mult */
    bgfx_uniform_handle_t u_foliage_light;     /* xyz = TO-light dir */
    bgfx_uniform_handle_t s_foliage_alpha;     /* leaf alpha mask sampler */

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

    /* Per-frame per-entity cull cache (see SrEntityCull).  Heap-allocated in
     * create (SR_MAX_ENTITIES entries initially) and rebuilt every frame (the
     * scene is editable so entities can move) in one O(n) pass that replaces ~6×
     * redundant per-entity world-AABB + model recomputes across the shadow/
     * prepass cull sites.  GROWN on demand (large-world capacity) so it always
     * has >= list.count entries — every ecull[i] access is bounded by list.count,
     * so growing to list.count keeps all the cull sites in range. */
    SrEntityCull     *ecull;
    /* #8 — composed world matrices cached ONCE per frame, index-parallel with
     * ecull[] (SoA split; same capacity, grown in lockstep).  The shadow
     * cascades / depth prepass / color submit read this instead of re-walking
     * the parent chain; the cull-only loops never touch it, so they stream
     * 40B/entry instead of 104B. */
    jce_mat4         *ecull_world;
    /* Per-shadow-pass memo for the cascade gather.  Which mesh an entity draws
     * does not depend on which cascade is being rendered, but the gather used
     * to re-resolve it per cascade: get_mesh_renderer + component_enabled +
     * resolve_mesh, ~135 cycles, once per candidate PER CASCADE.  A moving
     * camera misses every cascade cache, so at the 200k bench that ran 790k
     * times a frame to answer 200k distinct questions.
     * state: 0 = not yet resolved this pass, 1 = resolved (mesh in sh_res_mesh),
     * 2 = no drawable mesh (skip).  Reset once per pass, not per cascade. */
    JceMesh         **sh_res_mesh;
    uint8_t          *sh_res_state;
    uint32_t          sh_res_cap;
    /* kc_has_light mirror, one byte per entry (index-parallel with ecull[]):
     * the sparse-light gather scan reads ONLY this — 150KB streamed at 150k
     * instead of the whole 40B-entry array (~6MB) per viewport per frame.
     * Bytes (not bits) so the parallel wcache-hit workers can write their
     * disjoint entries without atomics.  Valid only while kindcache_on. */
    uint8_t          *ecull_light_byte;
    /* Second 1-byte mirror, same reason as the light one: SR_RK_IS_FAST for
     * this entry.  The submit loop skips an INVISIBLE fast-path entity on this
     * byte plus visible[i] -- two bytes -- instead of pulling the 44-byte
     * ecull[i] record to read render_kind. The sampler put that read at 14.8%
     * of the frame, the single hottest line, because 173k of 200k entities
     * exist in that loop only to be skipped. */
    uint8_t          *ecull_fast_byte;
    uint32_t          ecull_cap;     /* allocated SrEntityCull entries */

    /* Camera culling mask for THIS frame, copied once at the top of
     * jce_scene_renderer_render; 0 = no filtering.  Here rather than in each
     * pass's signature so the colour pass and the depth/velocity prepass
     * cannot read different values -- a disagreement draws objects with no
     * motion vector, which shimmers every frame. */
    uint32_t          frame_culling_mask;
    /* Steady-state CPU-cull skip: on a frame whose collect list is unchanged
     * (frame_list_gen) and whose movers are known (cull_movers, published by
     * the L2 ecull repair; count<0 => unknown => full path), the per-frame
     * pass-A/B/C + the 150k-entity grid maintenance walk in
     * sr_compute_visible are skipped — aabbs[] and the grid still hold last
     * frame's identical values; only movers re-bucket.  cull_mode0 caches the
     * transform-less always-kept indices for the visible[] re-init. */
    uint64_t          frame_list_gen;    /* published by the ecull build */
    /* Static-frame prepass cache (UE-style retained depth): one slot per
     * viewport (keyed by view_id_base).  When list/xforms/VP/size match and
     * no velocity is written, the prepass FBO already holds byte-identical
     * depth (+normals) — the whole pass is skipped. */
    struct SrPrepassCacheSlot {
        bool        valid;
        uint16_t    view_base, w, h;
        const void *scene;
        uint64_t    list_gen, xform;
        jce_mat4    vp;
        /* Velocity frames: prev-frame VP must match too, else the frame
         * right after the camera stops would keep a stale nonzero motion. */
        float       prev_vp[16];
        bool        had_velocity;
    }                 pre_cache[4];
    int32_t           cull_movers_count; /* -1 = unknown (full rebuild) */
    uint32_t          cull_movers[1024];
    uint64_t          cull_last_list_gen;
    uint64_t          cull_last_xform;
    const void       *cull_last_scene;
    bool              cull_frame_valid;
    bool              cull_mode0_valid;
    uint32_t          cull_mode0_count;
    uint32_t          cull_mode0[256];

    /* Parallel-gather scratch (concurrent-ECS opt-in, JCE_PARALLEL_GATHER).
     * pg_idx[k] = cull-index of the k-th eligible entity; pg_cmds[k] its
     * worker-built draw cmd.  Renderer-owned + grown on demand (no per-frame
     * alloc).  Unused (NULL) when parallel gather is off. */
    int              *pg_idx;
    SrPgCmd          *pg_cmds;
    uint32_t          pg_cap;

    /* Per-frame frustum-visibility scratch (one bool per collected entity).
     * Heap-grown to list.count so a large world does not put a 100KB+ array on
     * the stack (was `bool visible_buf[SR_MAX_ENTITIES]` — a stack overflow risk
     * once the cap is removed).  Grown lazily in the draw; freed on destroy. */
    bool             *visible_buf;
    uint32_t          visible_buf_cap;

    /* Cross-frame persistent static world-matrix + AABB cache (see
     * SrWorldCacheEntry).  Open-addressed table keyed by entity id; grown on
     * demand.  Lets the ecull build loop skip recomposing a static entity's
     * world matrix + re-transforming its AABB when the scene's structural epoch
     * is unchanged.  NULL until first use; sized a power of two. */
    SrWorldCacheEntry *wcache;
    uint32_t           wcache_cap;     /* power of two; 0 until first alloc */
    uint32_t           wcache_count;   /* occupied (used) slots */
    uint64_t           wcache_epoch;   /* scene structural epoch the table was built against */

    /* Lever ③ draw-cmd cache: frame-global content generation folded into each
     * cached cmd's validity. Bumped when the SSAO override or the debug view-mode
     * changes (both rewrite an eligible entity's built material), and by async
     * texture/model pop-in — so a single per-entity {epoch,xform_gen,material_gen,
     * content_gen} compare covers every invalidation without a per-frame rebuild.
     * dc_last_* detect the SSAO/view-mode edges. Inert when the lever is off. */
    uint64_t           dc_content_gen;
    bool               dc_last_ssao_active;
    uint16_t           dc_last_ssao_ao;
    int                dc_last_view_mode;

    /* ── Persistent focus-cull world-AABB cache (off-centre ground fix) ───────
     * The focus-bounded entity COLLECT runs before the per-frame ecull build, so
     * it has no AABB to test the focus disc against on the entity it is deciding.
     * This open-addressed table (keyed by entity id, mirrors wcache) caches every
     * collected entity's world AABB at the END of the ecull build, so the NEXT
     * frame's collect can keep an entity whose world AABB OVERLAPS the focus
     * region instead of culling by transform-ORIGIN distance.  Unlike wcache it is
     * NOT gated on the static predicate — a world-spanning STATIC-but-rigidbody
     * terrain (treated as "dynamic" by the wcache's conservative predicate) is the
     * exact entity that must survive an off-centre focus.  Entries untouched for a
     * frame are pruned so the table tracks the active streamed set. */
    struct SrFocusAabbEntry {
        uint32_t entity;          /* 0 = empty slot */
        jce_vec3 wmin, wmax;
        bool     used;
        bool     touched;         /* set when stored this frame; prune untouched */
    }                 *focus_aabb;
    uint32_t           focus_aabb_cap;     /* power of two; 0 until first alloc */
    uint32_t           focus_aabb_count;   /* occupied (used) slots */

        /* ── Persistent cull broad-phase (large-world-opt P1 #4) ──────────────
     * The cull grid (cull_space) is now PERSISTENT across frames: an entity is
     * inserted ONCE and kept, jce_space_update re-buckets only the entities
     * whose world AABB changed this frame (static city = ~0/frame), and despawned
     * entities are removed.  This map keys a stable per-entity space handle by
     * entity id (open-addressed linear probe, mirrors wcache), plus the AABB the
     * object was last (re)bucketed with (to skip no-op updates) and a per-cull
     * "seen" generation so entities absent this frame are removed.  Rebuilt from
     * scratch (all handles dropped + reinserted) only when the grid's world
     * bounds/resolution must change — rare once the streamed set is stable. */
    struct SrCullSpaceEntry {
        uint32_t entity;     /* 0 = empty slot */
        uint32_t handle;     /* jce_space handle (1-based; 0 = none) */
        jce_vec3 last_min, last_max;  /* AABB last (re)bucketed with */
        uint32_t seen_gen;   /* cull generation this entity was last seen */
        bool     used;
    }                 *cull_map;
    uint32_t           cull_map_cap;     /* power of two; 0 until first alloc */
    uint32_t           cull_map_count;   /* occupied (used) slots */
    uint32_t           cull_gen;         /* bumped each sr_compute_visible call */
    JceAABB            cull_space_world;  /* bounds the persistent grid was built for */
    bool               cull_space_built;  /* false => (re)insert everything */
    uint32_t           stat_cull_updated; /* entities re-bucketed this frame */
    uint32_t           stat_cull_inserted;/* entities inserted this frame */
    uint32_t           stat_cull_removed; /* entities removed this frame */
    /* Heap hit buffer for the frustum query result (was a 128 KB stack array). */
    uint32_t          *cull_hit_buf;
    uint32_t           cull_hit_cap;

    /* Color-pass GPU-instancing batch (see SrInstEntry).  Grown on demand;
     * reset at the start of each color entity walk, flushed at its end. */
    SrInstEntry      *inst_batch;
    uint32_t          inst_batch_count;
    uint32_t          inst_batch_cap;
    JceSrBatchTracker inst_batch_tracker;
    jce_mat4         *inst_gather;       /* contiguous per-model matrices for the instanced submit */
    uint32_t          inst_gather_cap;
    /* Parallel per-instance tint scratch for a tinted run (large-world-opt P1
     * #7): grown in lockstep with inst_gather, filled only for runs that carry
     * a non-white baseColor tint, and passed to jce_model_draw_instanced_tinted
     * as the i_data4 stream.  NULL/zero until the first tinted run. */
    jce_vec4         *inst_tint_gather;
    uint32_t          inst_tint_gather_cap;

    /* Factor-only primitive tint-instancing batch (opt-in JCE_PRIM_INSTANCE;
     * see SrPrimInstEntry).  Collected during the color entity walk (diverting
     * eligible shape primitives out of the solo/queue path), sorted by
     * (mesh, mat_key), and flushed as instanced-tinted submits at the walk end.
     * Grown on demand; reset each color walk. */
    SrPrimInstEntry  *prim_inst;
    uint32_t          prim_inst_count;
    uint32_t          prim_inst_cap;
    JceSrBatchTracker prim_batch_tracker;
    /* Permutation used by the flush's front-to-back sort.  Sorting indices
     * rather than the ~104-byte entries keeps the ordering without the memory
     * traffic; see sr_prim_inst_flush. */
    uint32_t         *prim_order;
    uint32_t          prim_order_cap;
    /* Cached depth permutation (see the gate in sr_prim_inst_flush): valid only
     * when the sort's own input is bit-identical to the frame that produced it,
     * and only for radix-produced orders -- a qsort order among equal keys is
     * not reproducible and so not cacheable. */
    uint64_t                   prim_order_hash;    /* rebuilt every frame */
    uint64_t                   prim_order_key;     /* hash the cache was built for */
    uint32_t                   prim_order_n;
    uint32_t                   prim_order_group;   /* single_group at build time */
    bool                       prim_order_valid;
    uint32_t                  *prim_order_verify;  /* JCE_DBG_VERIFY_SORT only */
    uint32_t                   prim_order_verify_cap;
    /* Scratch for the depth radix sort: 3 x prim_order_cap u32 (key, key_tmp,
     * idx_tmp).  qsort with an indirect comparator over 26,656 instances
     * measured 2.73 ms/frame - 89% of the whole flush phase. */
    uint32_t         *prim_sort_scratch;
    uint32_t          prim_sort_scratch_cap;

    /* Texture-diverse instancing batch + built-array cache (opt-in
     * JCE_TEX_INSTANCE; see SrTexInstEntry / SrTexArrayEntry). */
    SrTexInstEntry   *tex_inst;
    uint32_t          tex_inst_count;
    uint32_t          tex_inst_cap;
    /* Permutation for the flush's two-level sort; see sr_tex_inst_flush. */
    uint32_t         *tex_order;
    uint32_t          tex_order_cap;
    SrTexArrayEntry  *tex_arrays;       /* built albedo 2D-array cache */
    uint32_t          tex_arrays_count;
    uint32_t          tex_arrays_cap;
    uint64_t          tex_arrays_epoch;  /* structural epoch the cache was built at */

    /* ── GPU-driven rendering (roadmap #18, Phase 0+1) ─────────────────
     * gpu_scene owns the persistent GPUScene buffer + compute cull program.
     * cv_gpu_driven is the r.gpu_driven console toggle (default off).
     * gpu_driven_frame is latched at the top of each render() from the config
     * field OR the cvar, AND gpu_scene support — it gates the divergence in
     * sr_inst_flush and the view-order builder.  gpu_rec is the per-flush
     * scratch array of GPUScene records (grown on demand). */
    JceGpuScene      *gpu_scene;
    JceCvar          *cv_gpu_driven;
    JceCvar          *cv_gpu_min_records;
    JceCvar          *cv_gpu_min_group;
    JceCvar          *cv_gpu_min_per_draw;
    JceCvar          *cv_gpu_force;
    bool              gpu_driven_frame;
    bool              gpu_batch_active;
    /* 千万 S2: foliage GPU-cull latched per render() (JCE_FOLIAGE_GPU_CULL +
     * caps + HIGH-tier + first-render-per-frame).  Shares gpu_cull_view /
     * gpu_reset_view + gpu_frame_planes with the model GPU-driven path; when
     * true the foliage scatter draw GPU-culls + indirect-draws instead of the
     * S1 bind-all path.  Default off → S1 path byte-unchanged. */
    bool              foliage_gpu_cull_frame;
    /* Cull ENABLED for this run (env / perf default AND HIGH tier AND compute
     * support) — WITHOUT the per-viewport gpu_first guard, so it is stable across
     * the editor's scene+game viewports.  sr_draw_foliage keys the COMPUTE_READ
     * persistent instance VB off this; foliage_gpu_cull_frame (= this && gpu_first)
     * gates the actual per-frame dispatch. */
    bool              foliage_cull_enabled;
    JceGpuSceneRecord *gpu_rec;
    uint32_t          gpu_rec_cap;
    uint32_t         *gpu_index_counts;
    uint32_t          gpu_index_counts_cap;
    uint16_t          gpu_cull_view;     /* compute view for the cull/compact + build-indirect dispatches */
    uint16_t          gpu_reset_view;    /* SEPARATE earlier compute view for the counter-reset pass
                                          * (D3D12 cross-view barrier so reset's zero precedes compact's atomics) */
    jce_vec4          gpu_frame_planes[6]; /* color-pass frustum planes (this frame) */
    bool              gpu_frame_planes_valid;
    jce_vec3          gpu_frame_cam_pos;   /* color-pass camera position (with planes) */
    float             gpu_frame_err_k;     /* V3 DAG-cut world-error/metre allowance
                                            * (tol_px * 2*tan(fov/2) / vp_h; 0 = leaves) */

    /* Nanite-lite V2/V3.1: per-entity meshlet-cull indirect buffers (hero
     * meshes, opt-in JCE_MESHLET_CULL).  One drawIndexedIndirect element per
     * meshlet; per-ENTITY so several instances of one model never clobber
     * each other's same-frame args.  pass_seq stamps the last color pass a
     * slot drew in: a full table evicts the LRU slot NOT used this pass
     * (same-pass slots are live — >32 heroes in one view fall back to the
     * plain draw instead of thrashing buffer create/destroy each frame). */
    struct {
        bool      used;
        JceEntity entity;
        bgfx_indirect_buffer_handle_t indirect;
        /* V4: per-cascade shadow indirect args (survivor set differs per
         * light view).  Shares `count` (meshlet count) with the color buf. */
        bgfx_indirect_buffer_handle_t shadow_indirect[JCE_CSM_MAX_CASCADES];
        uint32_t  count;
        uint32_t  pass_seq;
    } meshlet_cache[32];
    uint32_t          meshlet_pass_seq;     /* bumped once per color pass    */
    bool              meshlet_hiz_ready;    /* Hi-Z prepared for this pass   */

    /* GI L1: dynamic irradiance probe grid (jce_gi_probes).  Fed from the
     * prev frame's lit color (cfg->gi_color_tex_handle) + depth prepass;
     * sampled at the camera each pass into gi_dyn_sh9, which the lit-submit
     * funnel uploads through the EXISTING u_sh9 baked-GI path whenever no
     * baked LightProbeGroup is active.  intensity 0 = fully dormant. */
    JceGiProbes      *gi_dyn;
    float             gi_dyn_intensity;     /* scene setting x JCE_GI env    */
    bool              gi_dyn_active;        /* this frame's camera SH valid  */
    float             gi_dyn_sh9[9][3];
    uint32_t          gpu_scene_frame;       /* bgfx frame idx that claimed the GPU path */
    bool              gpu_scene_frame_valid;
    /* Per-flush GPU-driven run draw list: each eligible model-run's model, the
     * GPU visible-buffer partition base, the inst_batch source index (for the
     * CPU fallback if the dispatch fails), the instance count, and the indirect
     * element index (UINT32_MAX when the 1:1 fixed-count fallback is in use).
     * Filled in pass 1 (record build + cull batch), drawn in pass 2 after one
     * dispatch. */
    struct { JceModel *model; uint32_t base; uint32_t src; uint32_t count;
             uint32_t indirect_el; uint32_t prim; } *gpu_draw;
    uint32_t          gpu_draw_count;
    uint32_t          gpu_draw_cap;
    /* Shadow-pass GPU-instancing batch (separate from the color batch: the
     * shadow pass runs per cascade/light, so this auto-flushes whenever the
     * target shadow view changes, plus a final flush before the color pass).
     * Reuses inst_gather as scratch (flushes are synchronous, no overlap). */
    SrInstEntry      *sh_batch;
    uint32_t          sh_batch_count;
    uint32_t          sh_batch_cap;
    JceSrBatchTracker sh_batch_tracker;
    uint16_t          sh_batch_view;     /* shadow view the pending batch targets */
    uint32_t          trace_sh_batch_instances;
    uint32_t          trace_sh_batch_flushes;
    uint32_t          trace_sh_sort_bypasses;

    /* ── GPU-driven CSM shadow cascades (roadmap #18, shadow extension) ──
     * One DEDICATED GPUScene instance per cascade.  The color pass dispatches
     * its cull ONCE against the camera frustum into the color gpu_scene; each
     * CSM cascade needs a SEPARATE cull against its own light frustum, and bgfx
     * defers all submits to frame() — so reusing ONE set of cull buffers across
     * cascades would let a later cascade's compute overwrite the visible/indirect
     * args an earlier cascade's already-queued submit_indirect reads at frame()
     * (aliased/garbage shadows).  Giving each cascade its OWN GPUScene = its own
     * visible/counter/indirect buffers means no buffer is overwritten before its
     * draw is consumed.  All instances SHARE the color pass's compute views —
     * base+3 (reset) and base+9 (cull/compact/build) — which is safe because they
     * write disjoint buffers and the view-order builder orders base+3 < base+9 <
     * color(base+0) < cascade draws(base+11+c): the cross-view barrier makes all
     * resets precede all compacts, and each cascade's compute (base+9) precedes
     * its draw (base+11+c).  Lazily created with the color gpu_scene. */
    JceGpuScene      *gpu_shadow_scene[JCE_CSM_MAX_CASCADES];
    jce_vec4          gpu_shadow_planes[JCE_CSM_MAX_CASCADES][6];
    bool              gpu_shadow_planes_valid;  /* per-frame: cascade planes set */
    uint16_t          gpu_shadow_view0;         /* first cascade view (base+11)  */
    uint32_t          gpu_shadow_cascades;      /* valid cascades this frame      */
    /* Per-shadow-flush GPU run draw list (mirrors gpu_draw shape; shadow flushes
     * complete before the color flush, so the two never overlap). */
    struct { JceModel *model; uint32_t base; uint32_t src; uint32_t count;
             uint32_t indirect_el; uint32_t prim; } *gpu_sh_draw;
    uint32_t          gpu_sh_draw_count;
    uint32_t          gpu_sh_draw_cap;

    /* Octahedral impostor terminal LOD (P2 #10).  Per-path atlas cache + the
     * per-frame per-atlas card accumulators (collected during the color entity
     * walk, flushed as one instanced draw each in sr_impostor_flush). */
    SrImpostorCache   impostor_cache[SR_IMPOSTOR_CACHE_MAX];
    int               impostor_cache_count;
    SrImpostorBatch   impostor_batch[SR_IMPOSTOR_CACHE_MAX];
    int               impostor_batch_count;

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
    /* Open-addressing hash index over mat_cache (key -> entry index), so both
     * sr_register_material (per-entity) and sr_bind_material_cb (per-submit)
     * resolve a material key in O(1) instead of an O(mat_count) linear scan.
     * Stamped per frame by a generation token (mat_hash_gen) so reset is O(1)
     * (no per-frame memset of the whole table). SR_MAT_HASH_SIZE is a pow2
     * >= 4x SR_MAT_CACHE_MAX to keep the load factor < 0.25 (few probes). */
    int32_t           mat_hash_idx[SR_MAT_HASH_SIZE];
    uint32_t          mat_hash_stamp[SR_MAT_HASH_SIZE];
    uint32_t          mat_hash_gen;
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
    /* CPU-particle billboard sprite: the SAME vs_particle+fs_particle shaders
     * the GPU pool draws with, fed a CPU-built transient instance buffer.  So
     * CPU-simulated particles render as the identical view-aligned soft-circle
     * on EVERY backend (the GPU compute path is unreliable on Vulkan/D3D12, so
     * CPU is the one consistent path).  Lazily created on first particle draw. */
    bgfx_program_handle_t       prog_particle_sprite;
    bgfx_vertex_buffer_handle_t particle_quad_vb;
    bgfx_index_buffer_handle_t  particle_quad_ib;
    bgfx_uniform_handle_t       u_particle_misc;   /* .x tex .z softfade */
    bgfx_uniform_handle_t       u_particle_soft;   /* 1/w 1/h near far   */
    bgfx_uniform_handle_t       s_particle_scene_depth; /* prepass depth */
    bool                        particle_sprite_tried;
    /* The renderer no longer owns an hour-of-day clock.  It kept a private
     * advancing tod_clock_hour plus the authored value it was seeded from,
     * which made it the second of two clocks (the editor advanced the AUTHORED
     * field in place, from ImGui's frame time) and put the live hour where only
     * something holding a renderer could read it -- so a headless build had no
     * clock and gameplay had no accessor.  The live hour is now in the scene's
     * JceEnvironmentState; sr_drive_time_of_day reads it. */
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
 * receive it across the module boundary with one shared definition.
 *
 * Large-world capacity (Direction C): `entities` is a HEAP buffer that grows on
 * demand to the scene's entity count, so worlds of any size render fully (no
 * arbitrary SR_MAX_ENTITIES cap that silently drops entities beyond it).
 * SR_MAX_ENTITIES is the INITIAL capacity — a scene with <= that many entities
 * never reallocs, so the common case is byte-identical to the fixed-array path.
 * Indexing is by [i] exactly as before (pointer vs array is transparent). */
typedef struct {
    JceEntity *entities;   /* heap; NULL until first sr_entity_list_reserve */
    int        count;
    int        cap;        /* allocated capacity (>= count) */
} EntityList;

enum { SR_TERRAIN_SURFACE_MAX = 16 };

typedef struct {
    JceTerrain *terrain;
    jce_mat4 world;
    jce_mat4 inverse;
    jce_mat4 normal_matrix;
    jce_vec3 world_min;
    jce_vec3 world_max;
    float local_min_y;
    float local_max_y;
    bool upright;
} SrTerrainSurface;

/* World-space projection set shared by foliage and grass.  Terrain assets stay
 * local; this adapter owns Transform, overlap selection and cache identity. */
typedef struct {
    SrTerrainSurface surfaces[SR_TERRAIN_SURFACE_MAX];
    int count;
    float reference_y;
    uint32_t hash;
} SrTerrainSurfaceSet;

/* Ensure `list->entities` has room for at least `need` elements, growing (2×,
 * floor SR_MAX_ENTITIES) on demand.  Returns true on success; on allocation
 * failure leaves the existing buffer/cap intact and returns false (the caller
 * then drops the overflow element gracefully).  Grow-BEFORE-write: callers must
 * call this before writing list->entities[count].  Defined in
 * jce_scene_renderer.c. */
bool sr_entity_list_reserve(EntityList *list, int need);

/* ── Shared internal renderer helpers (cross-module) ──────────────────
 * Helpers that were file-static in the monolithic jce_scene_renderer.c but are
 * now called across the split jce_sr_*.c translation units.  These keep their
 * original definitions (with `static` removed) in whichever module owns them;
 * the prototypes here give the other modules visibility.  Pure move + linkage:
 * no behaviour change. */

/* Frustum / AABB helpers (own: core). */
void  sr_extract_frustum_planes(const jce_mat4 *m, jce_vec4 planes[6]);
/* Instanced draw that fills the instance buffer directly from the caller's
 * records through an index list -- see jce_model.c. Engine-internal: the public
 * jce_mesh_draw_instanced_tinted keeps its two-array shape.
 *
 * `shader_keys` is what the batch's MATERIAL asks for -- normally
 * sr_material_shader_keys(sr, key), since a primitive batch is keyed by a
 * material by construction.  It is a parameter rather than a second entry
 * point because this one is engine-internal: only the primitive flush calls
 * it, and it takes one branch or the other depending on whether the
 * instances were gathered.  Wiring one branch and not the other is the
 * half-wired failure the keyword manifest exists to end. */
void jce_mesh_draw_instanced_gathered(const JceMesh *mesh, const JceRenderer *r,
                                      uint16_t view_id, const void *src,
                                      size_t src_stride, size_t src_offset,
                                      const uint32_t *ord, uint32_t count,
                                      bool has_tint, uint64_t state,
                                      void (*pre_submit)(void *user, uint16_t view_id),
                                      void *pre_submit_user,
                                      uint32_t shader_keys);

bool  sr_aabb_in_frustum(const jce_vec4 planes[6], jce_vec3 mn, jce_vec3 mx);
/* Same predicate with the frustum-only part precomputed (jce_frustum_abs_normals).
 * For loops that test many boxes against one frustum; a one-off test should use
 * the plain form and skip the setup. */
bool  sr_aabb_in_frustum_fast(const jce_vec4 planes[6], const float absn[6][3],
                              jce_vec3 mn, jce_vec3 mx);
void  sr_transform_aabb(const jce_mat4 *m, jce_vec3 lmn, jce_vec3 lmx,
                        jce_vec3 *out_mn, jce_vec3 *out_mx);

/* Material bind helpers (own: core).
 *
 * `receiver_layer` is JceLayerComponent.layer (0..31) for the object about to
 * be submitted; it picks the lights that reach it.  A PARAMETER, not a field
 * on sr: a draw that forgot to update renderer state would inherit the
 * previous draw's layer and be right by accident. */
void  sr_inline_bind_pbr_global(JceSceneRenderer *sr, const JcePbrMaterial *pbr,
                                uint16_t view_id, JceScene *scene, EntityList *list,
                                uint32_t receiver_layer);
void  sr_inline_bind_pbr_global_overrides(
    JceSceneRenderer *sr, const JcePbrMaterial *pbr, uint16_t view_id,
    JceScene *scene, EntityList *list, JceTexture albedo_override,
    JceTexture emissive_override, uint32_t receiver_layer);

/* Terrain (own: jce_sr_terrain.c). */
void      sr_terrain_slot_free(JceSceneRenderer *sr, int slot);
int       sr_terrain_find_or_load_slot(JceSceneRenderer *sr, JceScene *scene,
                                      const char *path);
void      sr_terrain_surfaces_collect(JceSceneRenderer *sr, JceScene *scene,
                                      EntityList *list, float reference_y,
                                      SrTerrainSurfaceSet *out);
bool      sr_terrain_surfaces_sample(void *user, float world_x,
                                     float world_z, bool want_normal,
                                     float *out_world_y,
                                     float out_world_normal[3]);
void      sr_terrain_free_chunks(JceSceneRenderer *sr, int slot);
void      sr_terrain_material_cache_destroy(JceSceneRenderer *sr);
JceMesh  *sr_terrain_chunk_mesh(JceSceneRenderer *sr, int slot,
                                int cx, int cz, int lod);
/* `receiver_layer` is the TERRAIN ENTITY's layer -- every chunk of one
 * terrain is the same object as far as lighting is concerned. */
void      sr_draw_terrain_chunks(JceSceneRenderer *sr, JceScene *scene,
                                 EntityList *list,
                                 const JceCamera *camera, uint16_t view_id,
                                 int slot, JceTerrainComponent *tc,
                                 const jce_mat4 *model,
                                 const JcePbrMaterial *pbr,
                                 const bgfx_texture_handle_t layer_tex[4],
                                 uint32_t receiver_layer);
/* `cull_vp` is the shadow view's view-projection, or NULL for no cull.
 * Only the caller knows which cascade a view id belongs to, and a cull
 * against the wrong frustum deletes shadows that should be there. */
bool      sr_try_submit_terrain_shadow(JceSceneRenderer *sr, JceScene *scene,
                                       JceEntity e, uint16_t view_id,
                                       const jce_mat4 *cull_vp);

/* Particles (own: jce_sr_particles.c). */
void      sr_draw_particles(JceSceneRenderer *sr, JceScene *scene,
                            const JceCamera *camera, uint16_t view_id);
void      sr_drive_gpu_particles(JceSceneRenderer *sr, JceScene *scene,
                                 const JceCamera *camera,
                                 uint16_t view_id_base, float dt_sec);

/* Core caches / lookups (own: core jce_scene_renderer.c) used by every module. */
bool          entity_enabled(JceScene *scene, JceEntity e);
JceTexture    sr_resolve_texture(JceSceneRenderer *sr, const char *path);
/* The same resolve, for a texture whose consuming shader expects a LINEAR
 * value: the texture is created with a hardware sRGB view so each texel is
 * decoded BEFORE filtering.  s_albedo, s_emissive and fs_terrain's four layer
 * albedos are the whole set -- every other sampler still reads the encoded
 * value, so the plain resolver above is the default and stays so. */
JceTexture    sr_resolve_texture_srgb(JceSceneRenderer *sr, const char *path);
JceTexture    sr_resolve_texture2_srgb(JceSceneRenderer *sr,
                                       const char *material_path,
                                       const char *mesh_path);
JceTexture    sr_resolve_texture2_cs(JceSceneRenderer *sr,
                                     const char *material_path,
                                     const char *mesh_path,
                                     bool srgb);
JceTexture    sr_resolve_texture2(JceSceneRenderer *sr,
                                  const char *material_path,
                                  const char *mesh_path);
/* jce_sr_batch.c -- static-batch member culling.  A merged group is one draw
 * and therefore ONE cullable object; the member table takes that back.
 * The probe is asked once per model slot (batch_probed); the run submit is
 * the colour pass's, and a wholly-visible group coalesces to ONE submit. */
void sr_probe_batch_members(JceSceneRenderer *sr, SrModelCache *mc,
                            const char *path);
/* Split one queued draw of a merged group into one command per VISIBLE run.
 * Returns false when there is nothing to split -- every ordinary model --
 * and the caller then pushes the command it built, byte for byte. */
/* Arm the visible index runs for the next model draw of this entity, or
 * disarm when it is not a merged group.  Called where execution is PROVEN
 * to reach; jce_model_draw_program applies them at the bottom. */
/* Keep the colour pass's frustum where the per-draw member cull can read it. */
void sr_stash_color_cull(JceSceneRenderer *sr, const jce_vec4 planes[6]);
/* The pre-pass albedo target SSGI reads as the receiver's albedo, or an
 * invalid handle under JCE_SSGI_NO_ALBEDO (the ablation). */
uint16_t sr_ssgi_albedo_handle(const JceSceneRenderer *sr);
void sr_arm_batch_runs(JceSceneRenderer *sr, JceScene *scene, JceEntity e,
                       const JceMeshRenderer *mr);

SrModelCache *sr_get_model(JceSceneRenderer *sr, const char *path,
                           uint32_t entity_id);
void          sr_fullscreen_effect_destroy_all(JceSceneRenderer *sr);
void          sr_fullscreen_effect_reset_all(JceSceneRenderer *sr);

/* Environment (own: jce_sr_environment.c): vegetation, water, tilemap, sky,
 * cloth, async IBL bake + skybox scan, time-of-day / weather / decals. */
void  sr_draw_foliage(JceSceneRenderer *sr, JceScene *scene,
                      EntityList *list, JceEntity e, uint16_t view_id,
                      const JceCamera *camera);
void  sr_draw_line_renderer(JceSceneRenderer *sr, JceScene *scene,
                            EntityList *list, JceEntity e,
                            const JceCamera *camera, uint16_t view_id);
void  sr_draw_trail_renderer(JceSceneRenderer *sr, JceScene *scene,
                             EntityList *list, JceEntity e,
                             const JceCamera *camera, uint16_t view_id);
void  sr_draw_water(JceSceneRenderer *sr, JceScene *scene,
                    const JceCamera *camera,
                    EntityList *list, JceEntity e, uint16_t view_id);
void  sr_water_slot_free(JceSceneRenderer *sr, int slot);
/* Grass Field (own: jce_sr_environment.c): blade-mesh builder (also used by
 * the unit test), lazy program/uniform lifecycle, per-field scatter cache. */
/* Per-card blade geometry: a segmented, tapered, curved strip (stylized
 * leaf look) — (SEGS+1) vertex rows of 2, SEGS quads of 2 tris. */
#define JCE_GRASS_BLADE_SEGS    4
#define JCE_GRASS_BLADE_VERTS   ((JCE_GRASS_BLADE_SEGS + 1) * 2)
#define JCE_GRASS_BLADE_INDICES (JCE_GRASS_BLADE_SEGS * 6)
/* Pure-CPU fill (no bgfx) — testable headlessly; called by sr_grass_build_blade. */
uint32_t sr_grass_fill_blade(float blade_height, float blade_width, int cards,
                             JceMeshVertex *v, uint32_t *idx);
JceMesh *sr_grass_build_blade(float blade_height, float blade_width, int cards);
void  sr_grass_slot_free(JceSceneRenderer *sr, int slot);
void  sr_foliage_slot_free(JceSceneRenderer *sr, int slot);
void  sr_draw_foliage_cluster(JceSceneRenderer *sr, JceScene *scene,
                              EntityList *list, JceEntity e, uint16_t view_id);
/* Sphere-proxy shadow caster for a FoliageCluster (soft round canopy shadow).
 * Returns true if the entity IS a foliage cluster (handled, submitted or not). */
bool  sr_try_submit_foliage_shadow(JceSceneRenderer *sr, JceScene *scene,
                                   JceEntity e, uint16_t view_id,
                                   uint16_t shadow_inst_idx, uint16_t shadow_idx);
/* 千万 ③: submit every shadow-casting VegetationScatter as one instanced
 * depth draw into cascade view cv at a reduced LOD (default coarsest; env
 * JCE_FOLIAGE_SHADOW_LOD).  Rides the persistent roots inst_vb (zero-copy);
 * called per rendered cascade, outside the per-entity caster walk. */
void  sr_submit_scatter_shadows(JceSceneRenderer *sr, JceScene *scene,
                                uint16_t cv, uint16_t shadow_inst_idx);
void  sr_draw_grass(JceSceneRenderer *sr, JceScene *scene,
                    EntityList *list, JceEntity e, uint16_t view_id,
                    const JceCamera *camera, const JceSceneRenderConfig *cfg);
int   sr_tilemap_find_or_load_slot(JceSceneRenderer *sr,
                                   const JceTilemapComponent *tmc);
void  sr_tilemap_free_slot(JceSceneRenderer *sr, int i);
uint32_t sr_tilemap_color_abgr(const float c[4]);
void  sr_tilemap_build_chunks(JceSceneRenderer *sr, int slot, uint32_t abgr);
void  sr_draw_tilemap_chunks(JceSceneRenderer *sr, int slot,
                             const JceCamera *camera, uint16_t view_id,
                             const jce_mat4 *model, uint32_t sort_order);
void  sr_draw_sky_gradient(JceSceneRenderer *sr, uint16_t view_id);
void  sr_draw_cloth(JceSceneRenderer *sr, JceScene *scene, uint16_t view_id);
void  sr_scan_skybox(JceSceneRenderer *sr, JceScene *scene, EntityList *list);
bool  sr_cloud_atlas_update(JceSceneRenderer *sr, bool enabled);
bool  sr_cloud_atlas_is_pending(const JceSceneRenderer *sr);
void  sr_cloud_atlas_shutdown(JceSceneRenderer *sr);
void  sr_refresh_sky_ibl(JceSceneRenderer *sr,
                         const JceSceneRenderingSettings *rs);
void  sr_bind_cloud_shadow(JceSceneRenderer *sr);
void  sr_advance_environment_state(JceSceneRenderer *sr,
                                   const JceSceneRenderingSettings *rs,
                                   float dt_sec);
void  sr_drive_time_of_day(JceSceneRenderer *sr, JceScene *scene,
                           const JceSceneRenderingSettings *rs);
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
/* Destroy the instance's file-resolved clips (SrAnimInstance.file_clips).
 * Declared beside sr_free_morph_vbs because it has the same obligation: it
 * must run at EVERY instance-lifecycle site -- model swap AND slot reclaim --
 * or the clip leaks on whichever path the author did not think about. */
void      sr_release_file_clips(SrAnimInstance *a);
uint16_t  sr_morph_vb_cb(void *user, uint32_t node, uint32_t prim);
/* jce_sr_morph.c -- the blendshape pass.  jce_sr_anim.c owns WHICH entities
 * are animated this frame (it builds the animator-selection memo); this file
 * owns what shape their meshes end up in.  Both are called from the resolve
 * loop there, once per instance, after the skeleton is posed. */
uint32_t  sr_sample_morph_track(SrAnimInstance *ai, float *out,
                                uint32_t max_out);
void      sr_deform_morph_prims(JceSceneRenderer *sr, SrAnimInstance *ai);
void      sr_update_sprite_anims(JceSceneRenderer *sr, JceScene *scene,
                                 EntityList *list, float dt_sec);
void      sr_update_skinned_anims(JceSceneRenderer *sr, JceScene *scene,
                                  EntityList *list, float dt_sec,
                                  const JceCamera *camera);

/* jce_sr_socket.c -- bone attachments.  Runs immediately after
 * sr_update_skinned_anims and before anything draws: the pose it reads is
 * produced by that call, so a runtime-side pass would necessarily be one
 * frame behind and the weapon would trail the hand while it swings. */
void      sr_update_bone_attachments(JceSceneRenderer *sr, JceScene *scene,
                                     EntityList *list);

/* GPU crowd instancing: pack all resident skinned palettes into the per-frame
 * bone texture (JCE_CROWD_INSTANCE).  Call once per viewport draw right after
 * sr_update_skinned_anims. */
void      sr_pack_bone_palettes(JceSceneRenderer *sr);

/* GPU crowd instancing color batcher.  reset clears groups at pass start; add
 * records an eligible skinned character (returns false on overflow => caller
 * draws it per-character); flush emits one instanced draw per (model,skinned
 * primitive) and clears the groups. */
void      sr_crowd_reset(JceSceneRenderer *sr);
bool      sr_crowd_add(JceSceneRenderer *sr, void *model,
                       const jce_mat4 *world, uint32_t palette_base);
void      sr_crowd_flush(JceSceneRenderer *sr, uint16_t view_id);

/* GPU bind-pose instancing color batcher (non-animating skinned characters). */
void      sr_bindpose_reset(JceSceneRenderer *sr);
bool      sr_bindpose_add(JceSceneRenderer *sr, void *model, const jce_mat4 *world);
void      sr_bindpose_flush(JceSceneRenderer *sr, uint16_t view_id);

/* GPU bind-pose instancing SHADOW batcher (depth-pass sibling of the above).
 * sr_try_submit_bindpose_shadow eats a non-animating skinned caster into the
 * per-view batch (flushing the previous view on change) and returns true;
 * sr_bindpose_shadow_flush emits the instanced depth draws and must be called
 * once more after the shadow pass to flush the final view's batch. */
void      sr_bindpose_shadow_reset(JceSceneRenderer *sr);
bool      sr_bindpose_shadow_add(JceSceneRenderer *sr, void *model, const jce_mat4 *world);
void      sr_bindpose_shadow_flush(JceSceneRenderer *sr, uint16_t view_id);
bool      sr_try_submit_bindpose_shadow(JceSceneRenderer *sr, JceScene *scene,
                                        JceEntity e, int cull_idx, uint16_t view_id);

/* ANIMATED crowd shadow batcher (vs_shadow_skinned_inst + bone texture) —
 * intercepted inside sr_try_submit_skinned_shadow for casters whose palette is
 * packed this frame; flushed on view change + drained at color-pass start. */
void      sr_crowd_shadow_reset(JceSceneRenderer *sr);
void      sr_crowd_shadow_flush(JceSceneRenderer *sr, uint16_t view_id);

/* Cross-module helpers shared with the shadow / draw / cull modules
 * (own: core jce_scene_renderer.c, except where noted). */
void  sr_rq_flush_and_collect(JceSceneRenderer *sr);
const char *sr_mesh_renderer_model_path(JceScene *scene, JceEntity e);
bool  sr_build_entity_model(JceSceneRenderer *sr, JceScene *scene,
                            JceEntity e, int cull_idx, jce_mat4 *out_model,
                            JceMesh **out_mesh,
                            JceMeshRenderer **out_mr);
jce_vec3 sr_light_world_shine_direction(const jce_vec3 *comp_dir,
                                        const JceTransform *xf);
bool  sr_resolve_primary_dir_light(JceSceneRenderer *sr, JceScene *scene,
                                   EntityList *list, bool shadow_only,
                                   jce_vec3 *out_to_light, jce_vec3 *out_color,
                                   float *out_intensity);
/* Shadow-pass GPU-instancing batch (own: core/draw instancing).  `lod` is the
 * in-asset auto-LOD level for the caster (0 = base); the batch is keyed by
 * (model, lod) so LOD'd casters still instance (P1 #6). */
bool  sr_sh_batch_add(JceSceneRenderer *sr, JceModel *model,
                      const jce_mat4 *world, uint16_t lod);
void  sr_sh_flush(JceSceneRenderer *sr, uint16_t view_id);
/* Shadow-submit helpers (own: core; route skinned / model casters). */
bool  sr_try_submit_skinned_shadow(JceSceneRenderer *sr, JceScene *scene,
                                   JceEntity e, int cull_idx, uint16_t view_id);
bool  sr_try_submit_mesh_renderer_model_shadow(JceSceneRenderer *sr,
                                               JceScene *scene, JceEntity e,
                                               int cull_idx, uint16_t view_id);
/* Nanite-lite V4: cluster-cull a hero mesh into shadow cascade `cascade`
 * (view `cv`).  Own: jce_sr_draw.c; called from the cascade loops. */
bool  sr_try_submit_meshlet_shadow(JceSceneRenderer *sr, JceScene *scene,
                                   JceEntity e, int cull_idx, uint16_t cv,
                                   uint32_t cascade);
/* JCE_MESHLET_CULL master switch (own: jce_sr_draw.c). */
bool  sr_mlcull_enabled(void);
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
                          bool gpu_cull_view,
                          bool include_underwater_view);
void  sr_destroy_shadow_targets(JceSceneRenderer *sr);
void  sr_create_shadow_targets(JceSceneRenderer *sr);
/* Lazily-allocated shadow targets: created on the first frame that actually
 * renders into them, so a scene with one directional light and no local
 * casters does not pay for surfaces it never touches.  Return false when the
 * allocation failed -- callers must skip the pass, not bind an invalid FBO. */
bool  sr_ensure_shadow_map(JceSceneRenderer *sr);
bool  sr_ensure_local_atlas(JceSceneRenderer *sr);
/* Cascade targets are allocated only up to the count the pipeline renders, so
 * a LOW-tier run (floor clamps to one cascade) does not pay for four.  Call
 * before the cascade loop: a tier or pipeline change can RAISE the count after
 * the targets were built, and this grows the set to match. */
bool  sr_ensure_csm_count(JceSceneRenderer *sr);
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
/* want_normals: write the G-BUFFER NORMAL TARGET, not just depth.
 *
 * The parameter used to be called want_ssr and the pass read it as "SSR is
 * on", which happened to be the only consumer -- so with SSR off the pass
 * quietly switched to a depth-only batched program and the normal target kept
 * whatever was in it.  That is invisible until a SECOND consumer wants
 * normals: SSGI's first frames gathered a constant normal at every pixel and
 * produced exactly zero changed pixels, with every gate green.  The name now
 * says what the flag decides. */
void  sr_draw_depth_prepass(JceSceneRenderer *sr, JceScene *scene,
                            const JceCamera *camera, EntityList *list,
                            uint16_t view_id_base, uint32_t vp_w, uint32_t vp_h,
                            bool want_normals);
void  sr_select_lights(JceSceneRenderer *sr, JceScene *scene,
                       EntityList *list, const JceCamera *camera);
uint32_t sr_compute_visible(JceSceneRenderer *sr, JceScene *scene,
                            const EntityList *list, const jce_vec4 planes[6],
                            bool *visible);
void  sr_gather_baked_gi(JceSceneRenderer *sr, JceScene *scene,
                         const JceCamera *camera, EntityList *list);
void  sr_bind_baked_gi(JceSceneRenderer *sr, float ibl_params[4]);
void  sr_upload_gi_uniforms(JceSceneRenderer *sr);
void  sr_upload_gi_uniforms_at(JceSceneRenderer *sr, const jce_vec3 *pos);

/* Core helpers consumed by the draw module (own: core jce_scene_renderer.c). */
bool  sr_is_gltf_model_path(const char *path);
JceShaderHandle sr_resolve_custom_program(JceSceneRenderer *sr,
                                          const char *material_path);
void  sr_rq_flush_queue_and_collect(JceSceneRenderer *sr, JceRenderQueue *q);
/* Per-frame material registry (own: core; consumed by sr_draw_entities). */
void     sr_reset_material_cache(JceSceneRenderer *sr);
/* `receiver_layer` is JceLayerComponent.layer for the objects this key will
 * batch -- see the note at the end of sr_compute_material_key. */
uint32_t sr_compute_material_key(const JcePbrMaterial *pbr, bool is_terrain,
                                 int terrain_slot,
                                 const bgfx_texture_handle_t *terrain_layer_tex,
                                 uint32_t receiver_layer);
uint32_t sr_material_shader_keys(const JceSceneRenderer *sr,
                                 uint32_t key);
uint32_t sr_register_material(JceSceneRenderer *sr, uint32_t key,
                              const JcePbrMaterial *pbr, bool is_terrain,
                              int terrain_slot, float terrain_tile_scale,
                              bool terrain_splat_enabled,
                              const bgfx_texture_handle_t *terrain_layer_tex,
                              uint32_t receiver_layer);
void     sr_bind_material_cb(uint32_t material_key, void *user);
void     sr_bind_material_overrides(uint32_t material_key, void *user,
                                    JceTexture albedo_override,
                                    JceTexture emissive_override);

/* Cross-frame static world-matrix + draw-cmd cache lookup (own: core; consumed
 * by sr_draw_entities for lever ③). Returns the entry for `entity` iff it is
 * still valid for {epoch, xform_gen}, else NULL. */
SrWorldCacheEntry *sr_wcache_find(JceSceneRenderer *sr, uint32_t entity,
                                  uint64_t epoch, uint64_t xform_gen);

/* Draw (own: jce_sr_draw.c): the color-pass + shadow-pass GPU-instancing
 * batches, GPU-driven instanced draw, the per-entity model/material draw
 * helpers, and the main entity-rendering pass. */
void  sr_draw_entities(JceSceneRenderer *sr, JceScene *scene,
                       const JceCamera *camera, EntityList *list,
                       uint16_t view_id, float dt_sec,
                       const JceSceneRenderConfig *cfg);

/* In-asset auto-LOD (large-world-opt P1 #6): compute + cache one entity's LOD
 * level for the frame into ecull[cull_idx] (camera-dependent; called from the
 * ecull build loop before both passes), and prune the per-entity LOD state to
 * the live set once per render. */
void  sr_compute_entity_lod(JceSceneRenderer *sr, JceScene *scene,
                            const JceCamera *camera, JceEntity e, int cull_idx);
void  sr_lod_state_prune(JceSceneRenderer *sr);

/* Streaming LOD cross-fade (Direction B): record/return an entity's normalized
 * fade-in factor [0,1] (1 = fully present) keyed off its first-seen time in the
 * color pass, and prune the per-entity fade state to the live set each render.
 * Returns 1.0 (no fade) whenever the phase clock is not advancing (dt<=0 still
 * editor preview) so static previews are byte-identical. */
float sr_fade_factor_for_entity(JceSceneRenderer *sr, uint32_t entity);
void  sr_fade_state_prune(JceSceneRenderer *sr);


/* Apply the resolved ambient to the light env, and log it when it changes.
 * `have_ambient` false means the engine fallback was used (no sky fill).
 * Implemented in jce_sr_ambient_log.c -- out of line because jce_sr_draw.c is
 * already past the 3000-line cap and check_file_size.py refuses to let an
 * over-cap file grow, which is the correct answer for a diagnostic. */
void jce_sr_apply_resolved_ambient(JceSceneRenderer *sr, jce_vec3 amb,
                                   float ai, bool have_ambient);

#endif /* JCE_SR_INTERNAL_H */

/* Pack the cascade far-planes into the vec4 csm_shadow.sh reads. All four
 * lanes are always written: see the definition for why a zero in .y/.z/.w is
 * a hard, camera-swept banding bug rather than a harmless unused slot. */
/* The cascade cross-fade fraction this frame, 0..0.35 -- the single accessor
 * both the surface path (u_csmParams.y) and the volumetric fog march read.
 * Shared rather than copied on purpose: two subsystems softening the SAME
 * cascade boundary by different amounts is how the fog came to band in front
 * of walls that did not. */
float sr_effective_csm_blend(const JceSceneRenderer *sr);

/* THE matrix cascade `c` is addressed with this frame -- shared by the static
 * cascade bind and the dynamic atlas render, because the shader derives both
 * taps from one u_csmVP[cascade]. See the definition for the judder that
 * followed from only one of them using it. */
jce_mat4 sr_cascade_sample_vp(const JceSceneRenderer *sr,
                              const JceCsmData *csm, uint32_t c);

void sr_pack_csm_splits(const float *splits, uint32_t cascade_count,
                        float out[4]);
