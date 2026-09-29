/*
 * jce_sr_planar.c -- the planar reflection PROBE: which one is live this
 * frame, the mirrored render it drives, and the screen-space composite that
 * puts it on every surface lying on its plane.
 *
 * WHY ITS OWN TRANSLATION UNIT.  jce_scene_renderer.c is 6797 lines and the
 * file-size gate refuses to let a file already over the cap grow; it offers
 * "split it, or move the addition into a new translation unit" and this is
 * the second.  The probe is also a clean seam: it reads the scene and the
 * renderer's per-frame state, and nothing else in the renderer reads it back.
 *
 * WHAT A PLANAR PROBE IS, and why it is not the cube probe next door: the
 * other reflection-probe modes capture a cubemap from a point, which is right
 * for a room and wrong for a mirror -- a flat surface shows the world from
 * the camera's MIRRORED position, and that moves when the camera does.  This
 * renders the scene once per frame from that mirrored position and composites
 * it in screen space, so it reaches a polished floor, a wet road and a still
 * lake without any of them needing a shader written for it.  fs_pbr has no
 * seventeenth sampler slot to hand a mirror texture to, which is exactly why
 * the engine's first planar reflection reached water and nothing else.
 *
 * Layer: middleware/scene (Layer 4) -- internal.
 */
#include "middleware/scene/jce_sr_internal.h"

#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_planar_reflection.h>
#include <jce/renderer/jce_views.h>

#include <math.h>
#include <string.h>

/* THE NEAREST PLANAR PROBE WHOSE INFLUENCE BOX CONTAINS THE CAMERA.
 *
 * "Nearest" rather than "first" because first is an answer about file order:
 * two probes would swap which one is live when the entities are re-saved, and
 * nothing would look wrong in either frame.  Containing the camera rather
 * than merely overlapping it, because a probe box is where its reflection is
 * CORRECT -- outside it the mirrored render is of the wrong plane. */
typedef struct {
    jce_vec3   eye;
    bool       found;
    float      best_d2;
    JceEntity  best;
} PlanarProbePick;

static void sr_pick_planar_probe(JceScene *s, JceEntity e, void *user)
{
    PlanarProbePick *pk = (PlanarProbePick *)user;
    const JceReflectionProbeComponent *rp = jce_scene_get_reflection_probe(s, e);
    if (!rp || rp->mode != JCE_REFLECTION_PROBE_PLANAR) return;

    const jce_mat4 m = jce_scene_get_world_matrix(s, e);
    const jce_vec3 c = jce_v3(m.raw[3][0] + rp->box_offset[0],
                              m.raw[3][1] + rp->box_offset[1],
                              m.raw[3][2] + rp->box_offset[2]);
    const jce_vec3 h = jce_v3(rp->box_size[0] * 0.5f,
                              rp->box_size[1] * 0.5f,
                              rp->box_size[2] * 0.5f);
    const jce_vec3 d = jce_v3_sub(pk->eye, c);
    if (h.x > 0.0f && fabsf(d.x) > h.x) return;
    if (h.y > 0.0f && fabsf(d.y) > h.y) return;
    if (h.z > 0.0f && fabsf(d.z) > h.z) return;

    const float d2 = jce_v3_dot(d, d);
    if (!pk->found || d2 < pk->best_d2) {
        pk->found   = true;
        pk->best_d2 = d2;
        pk->best    = e;
    }
}

void jce_scene_renderer_composite_planar(JceSceneRenderer *sr,
                                         uint16_t view_id,
                                         JceFrameBufferHandle dst_fb)
{
    if (!sr || !sr->planar || !sr->planar_probe_valid) return;

    /* The main camera's inverse view-projection, captured by the main render
     * itself -- jce_sr_internal.h says why it is not read from
     * cur_view_proj, which only exists when TAA velocity is on. */
    if (!sr->planar_probe_inv_vp_valid) return;
    const jce_mat4 inv_vp = sr->planar_probe_inv_vp;

    JceTextureHandle depth  = { sr->ssao_depth_tex.idx };
    JceTextureHandle normal = { sr->ssao_normal_tex.idx };
    jce_planar_reflection_apply(sr->planar, view_id, dst_fb,
                                depth, normal, &inv_vp,
                                sr->planar_probe_point,
                                sr->planar_probe_normal,
                                sr->planar_probe_thickness,
                                sr->planar_probe_angle_deg,
                                sr->planar_probe_intensity,
                                sr->planar_probe_max_rough,
                                sr->planar_probe_box_c,
                                sr->planar_probe_box_h,
                                sr->planar_probe_edge_fade,
                                sr->ssao_w, sr->ssao_h);
}

void jce_scene_renderer_render_planar_reflection(JceSceneRenderer *sr,
                                                 JceScene *scene,
                                                 const JceCamera *camera,
                                                 float dt_sec)
{
    if (!sr || !scene || !camera) return;

    /* Cleared every frame and re-established from the scene: a probe is not
     * drawn by anything, so unlike water there is no submit to register it. */
    sr->planar_probe_valid = false;
    sr->planar_probe_inv_vp_valid = false;
    if (!sr->planar_wanted) {
        PlanarProbePick pk;
        memset(&pk, 0, sizeof pk);
        pk.eye = jce_camera_get_position(camera);
        jce_scene_each_reflection_probe(scene, sr_pick_planar_probe, &pk);
        if (pk.found) {
            const JceReflectionProbeComponent *rp =
                jce_scene_get_reflection_probe(scene, pk.best);
            const jce_mat4 m = jce_scene_get_world_matrix(scene, pk.best);
            jce_vec3 n = jce_v3(rp->plane_normal[0], rp->plane_normal[1],
                                rp->plane_normal[2]);
            /* All zeroes means +Y -- the floor and the water surface, and what
             * a probe deserialised from a scene older than this mode reads as.
             * jce_scene.h says so beside the field. */
            if (jce_v3_len(n) < 1e-6f) n = jce_v3(0.0f, 1.0f, 0.0f);
            /* The plane passes through the ENTITY, not through the box: the
             * box says how far the mirror reaches, the transform says where it
             * is, and an offset box must not move the glass. */
            sr->planar_probe_point  = jce_v3(m.raw[3][0], m.raw[3][1],
                                             m.raw[3][2]);
            sr->planar_probe_normal = n;
            sr->planar_probe_box_c  = jce_v3(m.raw[3][0] + rp->box_offset[0],
                                             m.raw[3][1] + rp->box_offset[1],
                                             m.raw[3][2] + rp->box_offset[2]);
            sr->planar_probe_box_h  = jce_v3(rp->box_size[0] * 0.5f,
                                             rp->box_size[1] * 0.5f,
                                             rp->box_size[2] * 0.5f);
            sr->planar_probe_thickness = rp->planar_thickness;
            sr->planar_probe_angle_deg = rp->planar_angle_deg;
            sr->planar_probe_intensity = rp->intensity;
            sr->planar_probe_edge_fade = rp->blend_distance;
            /* Roughness cutoff is not its own field: a probe that mirrors a
             * matte floor is a probe nobody asked for, and 0.6 is where the
             * SSR pass already stops believing its own reflections. */
            sr->planar_probe_max_rough = 0.6f;
            sr->planar_probe_valid     = true;
        }
    }

    /* WHO ASKED is already known: sr_draw_water registers it as it submits,
     * so this reads a fact the frame just established instead of walking the
     * scene a second time -- and the plane comes from the same matrix the
     * draw used, not from a re-derivation that could disagree with it. */
    if (!sr->planar_wanted && !sr->planar_probe_valid) {
        /* Give the target back rather than hold it for the process lifetime,
         * the same hysteresis-free rule the other effects follow once their
         * scene stops asking. */
        if (sr->planar) {
            jce_planar_reflection_destroy(sr->planar);
            sr->planar = NULL;
        }
        return;
    }

    if (!sr->planar) {
        JcePlanarReflectionDesc d;
        memset(&d, 0, sizeof(d));
        d.renderer = sr->renderer;
        d.view_id  = JCE_VIEW_PLANAR_REFLECTION_BASE;
        /* Half of 720p: a reflection is read through a rough, Fresnel-weighted
         * surface, so the resolution it deserves is not the viewport's. */
        d.width  = 640;
        d.height = 360;
        /* The pak is what lets the APPLY pass exist.  Water does not need it
         * -- it samples the texture in its own material -- but a probe has no
         * material of its own, which is the whole point of it. */
        d.pak = sr->pak;
        sr->planar = jce_planar_reflection_create(&d);
        if (!sr->planar) return;
    }

    if (sr->planar_probe_valid) {
        jce_planar_reflection_render_plane(sr->planar, sr, scene, camera,
                                           sr->planar_probe_point,
                                           sr->planar_probe_normal,
                                           sr->last_viewport_aspect,
                                           JCE_VIEW_PLANAR_REFLECTION_BASE,
                                           dt_sec);
    } else {
        jce_planar_reflection_render(sr->planar, sr, scene, camera,
                                     sr->planar_plane_y,
                                     JCE_VIEW_PLANAR_REFLECTION_BASE, dt_sec);
    }

    /* Cleared here, not at the top: the flag is set by the DRAW earlier in
     * this frame, so clearing it on entry would erase the very thing this
     * function came to read.  Next frame's water re-registers. */
    sr->planar_wanted = false;
}
