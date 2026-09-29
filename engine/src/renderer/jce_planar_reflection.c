/*
 * jce_planar_reflection.c -- see the header for what this is and the two
 * limits measurement forced on it.
 *
 * The mirror itself is four lines: reflect the eye and the look-at point
 * through the plane y = plane_y, and flip the up vector so the handedness
 * survives.  Everything else here is about not disturbing the renderer this
 * borrows.
 */
#include <jce/renderer/jce_planar_reflection.h>

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_renderer.h>

#include <bgfx/c99/bgfx.h>
#include <jce/renderer/jce_scene_renderer.h>

#include "os/core/jce_memory.h"
#include "renderer/jce_fullscreen_pass.h"
#include "renderer/jce_shader_load.h"

#include <math.h>
#include <string.h>

#define LOG_TAG "planar_reflection"

struct JcePlanarReflection {
    JceOffscreenTarget *target;   /* colour + depth: the second render needs both */
    uint16_t            view_id;  /* the bridge's own view; the render's base   */
    int                 w, h;
    jce_mat4            view_proj;
    bool                valid;

    /* The APPLY pass.  All optional: a caller that only wants the texture
     * (the water path) passes no pak and every handle below stays invalid.
     *
     * EVERY ONE OF THESE IS EXPLICITLY SET TO UINT16_MAX, not left at the
     * calloc'd zero, because bgfx handle 0 is a VALID handle -- this tree has
     * paid for that twice, most recently with three uniform handles that
     * passed BGFX_HANDLE_IS_VALID, were never created, and sent every set to
     * whoever owned handle 0. */
    bgfx_program_handle_t prog_apply;
    bgfx_uniform_handle_t s_planar, s_depth, s_normal;
    bgfx_uniform_handle_t u_inv_vp, u_refl_vp, u_plane, u_params, u_box,
                          u_box_half;
    JceFsQuad             quad;
    bool                  apply_ready;
};

JcePlanarReflection *jce_planar_reflection_create(
    const JcePlanarReflectionDesc *desc)
{
    if (!desc || !desc->renderer) return NULL;

    JcePlanarReflection *pr =
        (JcePlanarReflection *)JCE_CALLOC(1, sizeof(*pr));
    if (!pr) return NULL;

    pr->w = desc->width  > 0 ? desc->width  : 640;
    pr->h = desc->height > 0 ? desc->height : 360;
    pr->view_id   = desc->view_id;
    pr->view_proj = jce_m4_identity();

    /* A full offscreen target, not the bare colour RT jce_fullscreen_pass.h
     * hands the screen-space effects: this one is rendered INTO by the scene
     * renderer, so it needs a depth attachment of its own. */
    pr->target = jce_offscreen_target_create(desc->renderer, desc->view_id);
    if (!pr->target) {
        LOG_ERROR(LOG_TAG, "offscreen target creation failed (%dx%d)",
                  pr->w, pr->h);
        JCE_FREE(pr);
        return NULL;
    }

    pr->prog_apply.idx = UINT16_MAX;
    pr->s_planar.idx   = UINT16_MAX;
    pr->s_depth.idx    = UINT16_MAX;
    pr->s_normal.idx   = UINT16_MAX;
    pr->u_inv_vp.idx   = UINT16_MAX;
    pr->u_refl_vp.idx  = UINT16_MAX;
    pr->u_plane.idx    = UINT16_MAX;
    pr->u_params.idx   = UINT16_MAX;
    pr->u_box.idx      = UINT16_MAX;
    pr->u_box_half.idx = UINT16_MAX;

    if (desc->pak) {
        const char *sfx = jce_shader_backend_suffix();
        bgfx_shader_handle_t vsh = { UINT16_MAX }, fsh = { UINT16_MAX };
        if (sfx) {
            vsh = jce_shader_load_from_pak(desc->pak, "vs_ssr", sfx, LOG_TAG);
            fsh = jce_shader_load_from_pak(desc->pak, "fs_planar_apply", sfx,
                                           LOG_TAG);
        }
        if (vsh.idx != UINT16_MAX && fsh.idx != UINT16_MAX) {
            /* vs_ssr, not a vs of this module's own: the apply pass is the
             * same fullscreen triangle the SSR composite draws, and a second
             * copy of six lines would be a second thing to keep in step. */
            pr->prog_apply = bgfx_create_program(vsh, fsh, true);
            jce_fs_quad_init(&pr->quad);
            pr->s_planar   = bgfx_create_uniform("s_planar",  BGFX_UNIFORM_TYPE_SAMPLER, 1);
            pr->s_depth    = bgfx_create_uniform("s_depth",   BGFX_UNIFORM_TYPE_SAMPLER, 1);
            pr->s_normal   = bgfx_create_uniform("s_normal",  BGFX_UNIFORM_TYPE_SAMPLER, 1);
            pr->u_inv_vp   = bgfx_create_uniform("u_planarInvViewProj",  BGFX_UNIFORM_TYPE_MAT4, 1);
            pr->u_refl_vp  = bgfx_create_uniform("u_planarReflViewProj", BGFX_UNIFORM_TYPE_MAT4, 1);
            pr->u_plane    = bgfx_create_uniform("u_planarPlane",   BGFX_UNIFORM_TYPE_VEC4, 1);
            pr->u_params   = bgfx_create_uniform("u_planarParams",  BGFX_UNIFORM_TYPE_VEC4, 1);
            pr->u_box      = bgfx_create_uniform("u_planarBox",     BGFX_UNIFORM_TYPE_VEC4, 1);
            pr->u_box_half = bgfx_create_uniform("u_planarBoxHalf", BGFX_UNIFORM_TYPE_VEC4, 1);
            pr->apply_ready = true;
        } else {
            if (vsh.idx != UINT16_MAX) bgfx_destroy_shader(vsh);
            if (fsh.idx != UINT16_MAX) bgfx_destroy_shader(fsh);
            LOG_WARN(LOG_TAG, "apply pass unavailable: fs_planar_apply not in "
                              "the pak; the mirror texture still renders");
        }
    }
    return pr;
}

void jce_planar_reflection_destroy(JcePlanarReflection *pr)
{
    if (!pr) return;
    if (pr->target) jce_offscreen_target_destroy(pr->target);
    if (pr->apply_ready) {
        if (pr->prog_apply.idx != UINT16_MAX) bgfx_destroy_program(pr->prog_apply);
        if (pr->s_planar.idx   != UINT16_MAX) bgfx_destroy_uniform(pr->s_planar);
        if (pr->s_depth.idx    != UINT16_MAX) bgfx_destroy_uniform(pr->s_depth);
        if (pr->s_normal.idx   != UINT16_MAX) bgfx_destroy_uniform(pr->s_normal);
        if (pr->u_inv_vp.idx   != UINT16_MAX) bgfx_destroy_uniform(pr->u_inv_vp);
        if (pr->u_refl_vp.idx  != UINT16_MAX) bgfx_destroy_uniform(pr->u_refl_vp);
        if (pr->u_plane.idx    != UINT16_MAX) bgfx_destroy_uniform(pr->u_plane);
        if (pr->u_params.idx   != UINT16_MAX) bgfx_destroy_uniform(pr->u_params);
        if (pr->u_box.idx      != UINT16_MAX) bgfx_destroy_uniform(pr->u_box);
        if (pr->u_box_half.idx != UINT16_MAX) bgfx_destroy_uniform(pr->u_box_half);
        jce_fs_quad_destroy(&pr->quad);
    }
    JCE_FREE(pr);
}

/* OBLIQUE NEAR-PLANE CLIPPING -- the mirror must not reflect its own glass.
 *
 * THE DEFECT THIS FIXES, measured rather than anticipated: with an ordinary
 * projection the mirrored camera sits BELOW the plane looking up, and the
 * first thing in front of it is the underside of the very surface it is a
 * mirror for.  On a 0.2 m floor slab that underside filled three quarters of
 * the reflection target, lit by nothing, so the "reflection" composited onto
 * the floor was a flat dark field -- which reads as "the effect is too subtle"
 * rather than as "the mirror is looking at its own back".  A lake got away
 * with it because water is a surface with nothing under it; a floor, a
 * polished table and a wall mirror do not.
 *
 * Lengyel's method: replace the projection's third row with the clip plane,
 * so the near plane IS the mirror plane and everything on the camera's side
 * of it is clipped by hardware rather than by hoping the scene has no
 * geometry there.  `plane_view` is the plane in the MIRROR camera's view
 * space, oriented so the half-space to keep has dot(plane, p) > 0.
 *
 * The depth distribution afterwards is not the usual one -- that is what an
 * oblique near plane means -- and nothing here reads that depth back. */
static jce_mat4 oblique_near_plane(jce_mat4 proj, jce_vec4 plane_view,
                                   bool homogeneous_depth)
{
    const float sx = (plane_view.x < 0.0f) ? -1.0f : 1.0f;
    const float sy = (plane_view.y < 0.0f) ? -1.0f : 1.0f;
    if (fabsf(proj.raw[0][0]) < 1e-9f || fabsf(proj.raw[1][1]) < 1e-9f ||
        fabsf(proj.raw[3][2]) < 1e-9f)
        return proj;

    /* The corner of the frustum furthest from the plane, in clip space, back
     * through the projection: the point the modified row must still map to
     * the far plane. */
    jce_vec4 q;
    q.x = (sx - proj.raw[2][0]) / proj.raw[0][0];
    q.y = (sy - proj.raw[2][1]) / proj.raw[1][1];
    q.z = 1.0f;
    q.w = (1.0f + proj.raw[2][2]) / proj.raw[3][2];

    const float denom = plane_view.x * q.x + plane_view.y * q.y +
                        plane_view.z * q.z + plane_view.w * q.w;
    if (fabsf(denom) < 1e-9f) return proj;

    const float k = (homogeneous_depth ? 2.0f : 1.0f) / denom;
    const jce_vec4 c = jce_v4(plane_view.x * k, plane_view.y * k,
                              plane_view.z * k, plane_view.w * k);

    proj.raw[0][2] = c.x;
    proj.raw[1][2] = c.y;
    /* GL keeps z in [-1,1], so the row is the plane MINUS the w row; every
     * other backend keeps [0,1] and the row is the plane itself.  Getting
     * this backwards does not crash, it clips the wrong half. */
    proj.raw[2][2] = homogeneous_depth ? (c.z - proj.raw[2][3]) : c.z;
    proj.raw[3][2] = homogeneous_depth ? (c.w - proj.raw[3][3]) : c.w;
    return proj;
}

void jce_planar_reflection_render(JcePlanarReflection *pr,
                                  JceSceneRenderer *sr,
                                  JceScene *scene,
                                  const JceCamera *camera,
                                  float plane_y,
                                  uint16_t view_id_base,
                                  float dt_sec)
{
    /* Water is horizontal and the call site says so.  One implementation. */
    /* Water samples the texture in its OWN shader with the main camera's
     * projection, so it needs the consumer's aspect for the same reason a
     * probe does; it passes <= 0 only because this entry point has never had
     * a way to say, and changing that is the water row's to make. */
    jce_planar_reflection_render_plane(pr, sr, scene, camera,
                                       jce_v3(0.0f, plane_y, 0.0f),
                                       jce_v3(0.0f, 1.0f,    0.0f),
                                       -1.0f, view_id_base, dt_sec);
}

/* Mirror a POINT through the plane; the direction version drops the offset. */
static jce_vec3 reflect_point_plane(jce_vec3 p, jce_vec3 p0, jce_vec3 n)
{
    const float d = jce_v3_dot(jce_v3_sub(p, p0), n);
    return jce_v3_sub(p, jce_v3_scale(n, 2.0f * d));
}

static jce_vec3 reflect_dir_plane(jce_vec3 v, jce_vec3 n)
{
    return jce_v3_sub(v, jce_v3_scale(n, 2.0f * jce_v3_dot(v, n)));
}

void jce_planar_reflection_render_plane(JcePlanarReflection *pr,
                                        JceSceneRenderer *sr,
                                        JceScene *scene,
                                        const JceCamera *camera,
                                        jce_vec3 plane_point,
                                        jce_vec3 plane_normal,
                                        float consumer_aspect,
                                        uint16_t view_id_base,
                                        float dt_sec)
{
    if (!pr || !sr || !scene || !camera || !pr->target) return;

    const float nl = jce_v3_len(plane_normal);
    if (nl < 1e-6f) { pr->valid = false; return; }
    const jce_vec3 pn = jce_v3_scale(plane_normal, 1.0f / nl);

    const jce_vec3 eye = jce_camera_get_position(camera);

    /* BEHIND the plane: leave the previous frame's texture alone.  The mirror
     * of a surface you are behind is not what you see through it, and drawing
     * the mirrored world there looks worse than a stale frame -- see the
     * header.  For the horizontal case this is exactly the old eye.y test. */
    if (jce_v3_dot(jce_v3_sub(eye, plane_point), pn) <= 0.001f) {
        pr->valid = false;
        return;
    }

    JCE_PROFILE_ZONE_N("Renderer::PlanarReflection");

    const jce_vec3 fwd = jce_camera_get_forward(camera);
    jce_vec3 look;
    look.x = eye.x + fwd.x;
    look.y = eye.y + fwd.y;
    look.z = eye.z + fwd.z;

    JceCameraDesc d;
    memset(&d, 0, sizeof(d));
    d.position   = reflect_point_plane(eye,  plane_point, pn);
    d.target     = reflect_point_plane(look, plane_point, pn);
    /* Mirroring flips handedness; the up vector has to flip with it or the
     * reflected image comes back upside down AND wound the wrong way, which
     * back-face culling then eats.  Reflecting the camera's OWN up is that
     * flip stated generally -- for a +Y plane and a +Y up it is the (0,-1,0)
     * this used to hard-code, which is how the generalisation was checked. */
    d.up = reflect_dir_plane(jce_camera_get_up(camera), pn);
    d.fov_deg    = jce_camera_get_fov(camera);
    d.near_plane = jce_camera_get_near(camera);
    d.far_plane  = jce_camera_get_far(camera);

    JceCamera *mirror = jce_camera_create(&d);
    if (!mirror) return;

    JceSceneRenderConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.draw_skybox      = true;    /* the sky is most of what a lake shows */
    cfg.draw_shadows     = false;
    cfg.draw_opaque      = true;
    cfg.draw_sprites     = false;
    cfg.draw_transparent = false;   /* no water inside the water's reflection */
    cfg.apply_postfx     = false;
    cfg.viewport_width   = (uint32_t)pr->w;
    cfg.viewport_height  = (uint32_t)pr->h;
    /* Slot 3 of JCE_SR_VIEWPORT_SLOTS: the two editor viewports use 0 and 1
     * and the runtime uses 0, so this render keeps its own previous-frame
     * camera and cannot disturb theirs. */
    cfg.viewport_id      = 3;
    cfg.reduced_pass     = true;    /* see JceSceneRenderConfig::reduced_pass */
    cfg.ssr_color_tex_handle = UINT16_MAX;
    cfg.gi_color_tex_handle  = UINT16_MAX;

    /* THE CONSUMER'S ASPECT, not the target's -- see the header for the
     * measurement.  The target's own shape is only a sampling density; the
     * projection has to match the view that will look the reflection up, or
     * every lookup lands scaled toward the centre. */
    const float rt_aspect = (pr->h > 0) ? ((float)pr->w / (float)pr->h) : 1.0f;
    const float aspect = (consumer_aspect > 0.0f) ? consumer_aspect : rt_aspect;
    jce_mat4 v = jce_camera_view(mirror);
    jce_mat4 p = jce_camera_proj(mirror, aspect, bgfx_get_caps()->homogeneousDepth);

    /* Clip at the mirror plane.  The plane goes into the mirror camera's VIEW
     * space oriented so the kept half-space is the one the mirror looks at --
     * everything on the far side of the glass. */
    {
        const jce_vec4 n4 = jce_v4(pn.x, pn.y, pn.z, 0.0f);
        const jce_vec4 p4 = jce_v4(plane_point.x, plane_point.y,
                                   plane_point.z, 1.0f);
        const jce_vec4 nv = jce_m4_mul_v4(&v, n4);
        const jce_vec4 pv = jce_m4_mul_v4(&v, p4);
        const jce_vec3 nv3 = jce_v3(nv.x, nv.y, nv.z);
        const jce_vec3 pv3 = jce_v3(pv.x, pv.y, pv.z);
        const jce_vec4 plane_view = jce_v4(nv3.x, nv3.y, nv3.z,
                                           -jce_v3_dot(nv3, pv3));
        p = oblique_near_plane(p, plane_view,
                               bgfx_get_caps()->homogeneousDepth);
    }

    /* prepare() both allocates the target at this size and points the base
     * view at it -- one call, and the same one the editor viewport uses, so
     * the reflection cannot drift from how a viewport is set up. */
    if (!jce_offscreen_target_prepare(pr->target, (uint32_t)pr->w,
                                      (uint32_t)pr->h,
                                      JCE_M4_PTR(v), JCE_M4_PTR(p),
                                      0x000000ff, "planar-reflection")) {
        jce_camera_destroy(mirror);
        pr->valid = false;
        return;
    }
    cfg.scene_frame_buffer =
        jce_offscreen_target_get_frame_buffer(pr->target);

    jce_scene_renderer_render(sr, scene, mirror, view_id_base, dt_sec, &cfg);

    pr->view_proj = jce_m4_multiply(&p, &v);
    pr->valid = true;

    jce_camera_destroy(mirror);
    JCE_PROFILE_ZONE_END;
}

void jce_planar_reflection_apply(JcePlanarReflection *pr,
                                 uint16_t view_id,
                                 JceFrameBufferHandle dst_fb,
                                 JceTextureHandle depth_tex,
                                 JceTextureHandle normal_tex,
                                 const jce_mat4 *inv_view_proj,
                                 jce_vec3 plane_point,
                                 jce_vec3 plane_normal,
                                 float thickness_m,
                                 float max_angle_deg,
                                 float intensity,
                                 float max_roughness,
                                 jce_vec3 box_centre,
                                 jce_vec3 box_half,
                                 float edge_fade_m,
                                 uint16_t dst_width,
                                 uint16_t dst_height)
{
    if (!pr || !pr->apply_ready || !pr->valid || !inv_view_proj) return;
    if (pr->prog_apply.idx == UINT16_MAX)  return;
    if (depth_tex.idx  == UINT16_MAX)      return;
    /* NO G-BUFFER, NO PASS.  Without a normal this could still test the plane
     * distance, and it would then paint the mirror onto the bottom row of
     * every wall standing on it.  Refusing is the honest branch. */
    if (normal_tex.idx == UINT16_MAX)      return;

    const float nl = jce_v3_len(plane_normal);
    if (nl < 1e-6f) return;
    const jce_vec3 pn = jce_v3_scale(plane_normal, 1.0f / nl);

    const bgfx_texture_handle_t ptex =
        { jce_offscreen_target_get_color_texture(pr->target) };
    if (ptex.idx == UINT16_MAX) return;

    JCE_PROFILE_ZONE_N("Renderer::PlanarReflection::apply");

    if (dst_width == 0 || dst_height == 0) return;

    bgfx_frame_buffer_handle_t dst = { dst_fb.idx };
    bgfx_set_view_frame_buffer(view_id, dst);
    /* THE RECT IS NOT OPTIONAL.  This pass shares its view id with the SSR
     * composite, and a view whose rect was never set keeps whatever the last
     * user of that id left on it -- so omitting this is correct in every
     * frame SSR also ran and wrong in every frame it did not.  That is the
     * shape of bug that gets reported as "it works in my scene". */
    bgfx_set_view_rect(view_id, 0, 0, dst_width, dst_height);
    bgfx_set_view_mode(view_id, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_touch(view_id);

    const bgfx_texture_handle_t dtex = { depth_tex.idx };
    const bgfx_texture_handle_t ntex = { normal_tex.idx };
    const uint32_t clampf = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    bgfx_set_texture(0, pr->s_planar, ptex, clampf);
    /* POINT for the two G-buffer reads: a filtered depth sample between two
     * surfaces reconstructs to a world position on NEITHER of them, which
     * puts a one-pixel halo of mirror along every silhouette. */
    bgfx_set_texture(1, pr->s_depth, dtex,
                     clampf | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT
                     | BGFX_SAMPLER_MIP_POINT);
    bgfx_set_texture(2, pr->s_normal, ntex,
                     clampf | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT
                     | BGFX_SAMPLER_MIP_POINT);

    bgfx_set_uniform(pr->u_inv_vp,  JCE_M4_PTR(*inv_view_proj), 1);
    bgfx_set_uniform(pr->u_refl_vp, JCE_M4_PTR(pr->view_proj),  1);

    /* Plane as (n, -dot(n, p0)) so the shader's test is one dot and one add.
     * Packing it here rather than there keeps the CPU's idea of the plane and
     * the GPU's identical by construction. */
    const float plane[4] = { pn.x, pn.y, pn.z,
                             -jce_v3_dot(pn, plane_point) };
    bgfx_set_uniform(pr->u_plane, plane, 1);

    /* Zero means the useful default, not zero's literal meaning -- the same
     * rule the component fields follow, and for the same reason: a probe
     * deserialised from a scene older than this mode reads as zeroes. */
    const float thick = (thickness_m  > 0.0f) ? thickness_m   : 0.05f;
    const float ang   = (max_angle_deg > 0.0f) ? max_angle_deg : 15.0f;
    const float inten = (intensity    > 0.0f) ? intensity     : 1.0f;
    const float rough = (max_roughness > 0.0f) ? max_roughness : 0.6f;
    const float params[4] = { thick, cosf(ang * 0.017453292519943295f),
                              inten, rough };
    bgfx_set_uniform(pr->u_params, params, 1);

    const float box[4]  = { box_centre.x, box_centre.y, box_centre.z,
                            edge_fade_m > 0.0f ? edge_fade_m : 0.0f };
    const float half[4] = { box_half.x > 0.0f ? box_half.x : 1e9f,
                            box_half.y > 0.0f ? box_half.y : 1e9f,
                            box_half.z > 0.0f ? box_half.z : 1e9f, 0.0f };
    bgfx_set_uniform(pr->u_box,      box,  1);
    bgfx_set_uniform(pr->u_box_half, half, 1);

    jce_fs_quad_bind(&pr->quad);
    /* Premultiplied "over", the same state jce_ssr_composite uses: the two
     * write the same kind of value into the same buffer, so they had better
     * agree about how it lands. */
    bgfx_set_state(BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE,
                                           BGFX_STATE_BLEND_INV_SRC_ALPHA), 0);
    bgfx_submit(view_id, pr->prog_apply, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}

JceTextureHandle jce_planar_reflection_texture(const JcePlanarReflection *pr)
{
    JceTextureHandle h;
    h.idx = (pr && pr->target && pr->valid)
              ? jce_offscreen_target_get_color_texture(pr->target)
              : UINT16_MAX;
    return h;
}

jce_mat4 jce_planar_reflection_view_proj(const JcePlanarReflection *pr)
{
    return pr ? pr->view_proj : jce_m4_identity();
}
