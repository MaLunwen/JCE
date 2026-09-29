/*
 * jce_scene_render_draw.cpp  Editor overlay passes.
 *
 * Phase B refactor: sky, shadows, and entity rendering live in the engine
 * `JceSceneRenderer`. This file now only contains overlay passes drawn on
 * top of the engine output: grid, selection outlines, physics debug,
 * ghost, hover.
 */

#include "jce_scene_render_internal.h"
#include "jce_scene_outline_policy.h"

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_sequencer.h>
#include <jce/middleware/physics/jce_physics_debug.h>
#include <jce/middleware/ai/jce_navmesh_recast.h>
#include <jce/os/core/jce_log.h>
}

#include "core/jce_editor_state.h"
#include "gizmo/jce_gizmo_joint.h"
#include "gizmo/jce_gizmo_cloth.h"
#include "gizmo/jce_gizmo_compound_collider.h"
#include "jce_editor_scene_render.h"   /* jce_editor_resolve_asset_path */

#include <cstdio>
#include <cstring>
#include <string>

/* ── Animation timer reset (kept for play.cpp compatibility) ─────── */

void jce_editor_scene_reset_anim_timer(void)
{
    s_sr.anim_last_ticks = 0;
}

/* ── Local entity model builder (overlay-only) ───────────────────── */

/* Builds a TRS model matrix from the entity's Transform component and
 * resolves the same mesh the engine scene renderer would draw: first the
 * file mesh from the editor cache, then the renderer-owned primitive mesh. */
static bool build_overlay_entity_model(uint32_t entity_id,
                                        jce_mat4 *out_model,
                                        JceMesh **out_mesh)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene || entity_id == 0) return false;
    JceEntity e = (JceEntity)entity_id;

    if (!jce_scene_has_transform(scene, e)) return false;

    /* Same composition the engine draw uses (hierarchical, pivot-aware
     * world matrix) so selection outlines stay glued to the mesh when the
     * entity is parented or carries an edited pivot. */
    *out_model = jce_scene_get_world_matrix(scene, e);

    if (out_mesh) {
        *out_mesh = NULL;
        if (jce_scene_has_mesh_renderer(scene, e)) {
            JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
            if (mr) {
                if (mr->mesh_path[0] != '\0') {
                    float wp[3] = {
                        out_model->raw[3][0], out_model->raw[3][1],
                        out_model->raw[3][2]
                    };
                    *out_mesh = get_cached_mesh(mr->mesh_path, wp);
                }
                if (!*out_mesh) {
                    *out_mesh = jce_scene_renderer_get_builtin_mesh(
                        s_sr.scene_renderer,
                        mr->mesh_shape);
                }
            }
        }
    }

    return true;
}

/* ── Infinite Grid Rendering (Blender-like fullscreen shader) ─────── */

void draw_grid(void)
{
    if (!jce_program_valid(s_sr.prog_grid)) return;

    /* Position-only fullscreen quad in NDC. */
    JceVertexLayout layout;
    jce_vertex_layout_begin(&layout);
    jce_vertex_layout_add(&layout, JCE_ATTRIB_POSITION, 3,
                          JCE_ATTRIB_TYPE_FLOAT, false, false);
    jce_vertex_layout_end(&layout);

    struct GridVertex { float x, y, z; };
    JceTransientVertexBuffer tvb;
    JceTransientIndexBuffer  tib;
    if (!jce_alloc_transient_buffers(&tvb, &layout, 4, &tib, 6, false))
        return;

    GridVertex *v = (GridVertex *)tvb.data;
    uint16_t *ix = (uint16_t *)tib.data;
    v[0] = { -1.0f, -1.0f, 0.0f };
    v[1] = {  1.0f, -1.0f, 0.0f };
    v[2] = {  1.0f,  1.0f, 0.0f };
    v[3] = { -1.0f,  1.0f, 0.0f };
    ix[0] = 0; ix[1] = 1; ix[2] = 2;
    ix[3] = 0; ix[4] = 2; ix[5] = 3;

    jce_vec3 cam_pos = jce_camera_get_position(s_sr.camera);
    float fade_near = fmaxf(16.0f, s_sr.orbit_distance * 3.0f);
    /* Pull the far extent in (was 24x): the far grid is sub-pixel anyway and the
       shader's grid-LOD fade now dissolves it, so a tighter fade avoids a vast
       shimmer-prone band while keeping plenty of visible grid. */
    float fade_far  = fmaxf(fade_near + 40.0f, s_sr.orbit_distance * 12.0f);
    /* Keep roughly the same screen density while orbiting by selecting
     * decimal world-space levels.  Fade the fine level before each decade
     * boundary so the next level takes over without a sudden density jump. */
    float grid_decade = log10f(fmaxf(s_sr.orbit_distance, 10.0f)) - 1.0f;
    float minor_spacing = fminf(10000.0f, powf(10.0f, floorf(grid_decade)));
    float transition = fminf(1.0f, fmaxf(0.0f,
                                      (grid_decade - floorf(grid_decade) - 0.7f)
                                      / 0.3f));
    transition = transition * transition * (3.0f - 2.0f * transition);
    float grid_camera[4] = { cam_pos.x, cam_pos.y, cam_pos.z,
                             1.0f - transition };
    float grid_fade[4] = { fade_near, fade_far,
                           minor_spacing * 10.0f, minor_spacing };

    jce_uniform_set(s_sr.u_grid_camera, grid_camera, 1);
    jce_uniform_set(s_sr.u_grid_fade, grid_fade, 1);
    jce_set_transient_vertex_buffer(0, &tvb, 0, 4);
    jce_set_transient_index_buffer(&tib, 0, 6);

    uint64_t state = JCE_STATE_WRITE_RGB
                   | JCE_STATE_WRITE_A
                   | JCE_STATE_MSAA
                   | JCE_STATE_BLEND_FUNC(JCE_BLEND_SRC_ALPHA,
                                          JCE_BLEND_INV_SRC_ALPHA);
    jce_set_state(state, 0);

    jce_mat4 identity = jce_m4_identity();
    jce_set_transform(identity.raw[0], 1);
    jce_submit(scene_view_id(), s_sr.prog_grid, 0, JCE_DISCARD_ALL);
}

/* ── Selection outlines ───────────────────────────────────────────── */

/* Draw 4 axial rays + an end-cap ring → cone gizmo for spot lights. */
static void outline_draw_cone(jce_vec3 apex, jce_vec3 axis_unit,
                              float length, float half_angle_rad,
                              uint32_t abgr)
{
    if (length <= 0.0f) length = 1.0f;
    if (half_angle_rad < 0.01f) half_angle_rad = 0.01f;

    /* Build an orthonormal basis (axis, u, v). */
    jce_vec3 up = (fabsf(axis_unit.y) < 0.95f)
                  ? jce_v3(0.0f, 1.0f, 0.0f)
                  : jce_v3(1.0f, 0.0f, 0.0f);
    jce_vec3 u = jce_v3_normalize(jce_v3_cross(axis_unit, up));
    jce_vec3 v = jce_v3_cross(axis_unit, u);

    jce_vec3 base   = jce_v3_add(apex, jce_v3_scale(axis_unit, length));
    float    radius = length * tanf(half_angle_rad);

    /* End-cap ring (16 segments). */
    const int seg = 16;
    jce_vec3 prev = base;
    for (int k = 0; k <= seg; k++) {
        float a = (float)k * (6.2831853f / (float)seg);
        jce_vec3 p = jce_v3_add(base,
                        jce_v3_add(jce_v3_scale(u, cosf(a) * radius),
                                   jce_v3_scale(v, sinf(a) * radius)));
        if (k > 0) jce_debug_draw_line(prev, p, abgr);
        prev = p;
    }

    /* 4 axial rays from apex to ring at 0°, 90°, 180°, 270°. */
    for (int k = 0; k < 4; k++) {
        float a = (float)k * (3.1415927f * 0.5f);
        jce_vec3 p = jce_v3_add(base,
                        jce_v3_add(jce_v3_scale(u, cosf(a) * radius),
                                   jce_v3_scale(v, sinf(a) * radius)));
        jce_debug_draw_line(apex, p, abgr);
    }
}

/* Draw a perspective/ortho frustum gizmo for camera entities. */
static void outline_draw_frustum(jce_vec3 origin, jce_quat rot,
                                  float fov_deg, float near_z, float far_z,
                                  bool ortho, uint32_t abgr)
{
    if (near_z <= 0.0f) near_z = 0.1f;
    if (far_z  <= near_z) far_z = near_z + 1.0f;

    /* Right-handed: forward = -Z, up = +Y, right = +X. */
    jce_vec3 fwd   = jce_q_rotate(rot, jce_v3(0.0f, 0.0f, -1.0f));
    jce_vec3 up    = jce_q_rotate(rot, jce_v3(0.0f, 1.0f,  0.0f));
    jce_vec3 right = jce_q_rotate(rot, jce_v3(1.0f, 0.0f,  0.0f));

    const float aspect = 16.0f / 9.0f;
    float hn, wn, hf, wf;
    if (ortho) {
        hn = hf = 1.0f;
        wn = wf = aspect;
    } else {
        float t = tanf(fov_deg * 0.5f * 3.1415927f / 180.0f);
        hn = near_z * t;
        wn = hn * aspect;
        hf = far_z  * t;
        wf = hf * aspect;
    }

    jce_vec3 nc = jce_v3_add(origin, jce_v3_scale(fwd, near_z));
    jce_vec3 fc = jce_v3_add(origin, jce_v3_scale(fwd, far_z));

    jce_vec3 ntl = jce_v3_add(nc, jce_v3_add(jce_v3_scale(up,  hn), jce_v3_scale(right, -wn)));
    jce_vec3 ntr = jce_v3_add(nc, jce_v3_add(jce_v3_scale(up,  hn), jce_v3_scale(right,  wn)));
    jce_vec3 nbl = jce_v3_add(nc, jce_v3_add(jce_v3_scale(up, -hn), jce_v3_scale(right, -wn)));
    jce_vec3 nbr = jce_v3_add(nc, jce_v3_add(jce_v3_scale(up, -hn), jce_v3_scale(right,  wn)));
    jce_vec3 ftl = jce_v3_add(fc, jce_v3_add(jce_v3_scale(up,  hf), jce_v3_scale(right, -wf)));
    jce_vec3 ftr = jce_v3_add(fc, jce_v3_add(jce_v3_scale(up,  hf), jce_v3_scale(right,  wf)));
    jce_vec3 fbl = jce_v3_add(fc, jce_v3_add(jce_v3_scale(up, -hf), jce_v3_scale(right, -wf)));
    jce_vec3 fbr = jce_v3_add(fc, jce_v3_add(jce_v3_scale(up, -hf), jce_v3_scale(right,  wf)));

    /* Near rect. */
    jce_debug_draw_line(ntl, ntr, abgr); jce_debug_draw_line(ntr, nbr, abgr);
    jce_debug_draw_line(nbr, nbl, abgr); jce_debug_draw_line(nbl, ntl, abgr);
    /* Far rect. */
    jce_debug_draw_line(ftl, ftr, abgr); jce_debug_draw_line(ftr, fbr, abgr);
    jce_debug_draw_line(fbr, fbl, abgr); jce_debug_draw_line(fbl, ftl, abgr);
    /* Connectors. */
    jce_debug_draw_line(ntl, ftl, abgr); jce_debug_draw_line(ntr, ftr, abgr);
    jce_debug_draw_line(nbl, fbl, abgr); jce_debug_draw_line(nbr, fbr, abgr);
    /* Apex stub so the origin is visible for ortho cameras too. */
    jce_debug_draw_line(origin, nc, abgr);
}

/* Frustum aimed from `origin` toward `target` — for virtual cameras whose pose
 * is RESOLVED from look-at / follow targets (jce_vcam_system resolve_pose), not
 * the entity transform.  Also strokes the sight line origin->target so the aim
 * is unambiguous. */
static void outline_draw_frustum_lookat(jce_vec3 origin, jce_vec3 target,
                                        float fov_deg, float near_z, float far_z,
                                        uint32_t abgr)
{
    if (near_z <= 0.0f) near_z = 0.1f;
    if (far_z  <= near_z) far_z = near_z + 1.0f;
    jce_vec3 fwd = jce_v3_sub(target, origin);
    float l2 = fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z;
    fwd = (l2 < 1e-8f) ? jce_v3(0.0f, 0.0f, -1.0f)
                       : jce_v3_scale(fwd, 1.0f / sqrtf(l2));
    jce_vec3 up0   = (fabsf(fwd.y) > 0.95f) ? jce_v3(0, 0, 1) : jce_v3(0, 1, 0);
    jce_vec3 right = jce_v3_normalize(jce_v3_cross(fwd, up0));
    jce_vec3 up    = jce_v3_normalize(jce_v3_cross(right, fwd));

    const float aspect = 16.0f / 9.0f;
    float t  = tanf(fov_deg * 0.5f * 3.1415927f / 180.0f);
    float hn = near_z * t, wn = hn * aspect;
    float hf = far_z  * t, wf = hf * aspect;
    jce_vec3 nc = jce_v3_add(origin, jce_v3_scale(fwd, near_z));
    jce_vec3 fc = jce_v3_add(origin, jce_v3_scale(fwd, far_z));
    jce_vec3 ntl = jce_v3_add(nc, jce_v3_add(jce_v3_scale(up,  hn), jce_v3_scale(right, -wn)));
    jce_vec3 ntr = jce_v3_add(nc, jce_v3_add(jce_v3_scale(up,  hn), jce_v3_scale(right,  wn)));
    jce_vec3 nbl = jce_v3_add(nc, jce_v3_add(jce_v3_scale(up, -hn), jce_v3_scale(right, -wn)));
    jce_vec3 nbr = jce_v3_add(nc, jce_v3_add(jce_v3_scale(up, -hn), jce_v3_scale(right,  wn)));
    jce_vec3 ftl = jce_v3_add(fc, jce_v3_add(jce_v3_scale(up,  hf), jce_v3_scale(right, -wf)));
    jce_vec3 ftr = jce_v3_add(fc, jce_v3_add(jce_v3_scale(up,  hf), jce_v3_scale(right,  wf)));
    jce_vec3 fbl = jce_v3_add(fc, jce_v3_add(jce_v3_scale(up, -hf), jce_v3_scale(right, -wf)));
    jce_vec3 fbr = jce_v3_add(fc, jce_v3_add(jce_v3_scale(up, -hf), jce_v3_scale(right,  wf)));
    jce_debug_draw_line(ntl, ntr, abgr); jce_debug_draw_line(ntr, nbr, abgr);
    jce_debug_draw_line(nbr, nbl, abgr); jce_debug_draw_line(nbl, ntl, abgr);
    jce_debug_draw_line(ftl, ftr, abgr); jce_debug_draw_line(ftr, fbr, abgr);
    jce_debug_draw_line(fbr, fbl, abgr); jce_debug_draw_line(fbl, ftl, abgr);
    jce_debug_draw_line(ntl, ftl, abgr); jce_debug_draw_line(ntr, ftr, abgr);
    jce_debug_draw_line(nbl, fbl, abgr); jce_debug_draw_line(nbr, fbr, abgr);
    /* Sight line to the look-at point (dimmer). */
    jce_debug_draw_line(origin, target, (abgr & 0x00FFFFFFu) | 0x60000000u);
}

/* ── 2D collider outline (Box2D body, XY plane) ───────────────────── */

/* Rotate (px,py) by (cos=c, sin=s), translate to (cx,cy); z carried for
 * display only — the 2D simulation has no Z. */
static jce_vec3 collider2d_pt(float cx, float cy, float z,
                              float c, float s, float px, float py)
{
    return jce_v3(cx + c * px - s * py, cy + s * px + c * py, z);
}

/* 4-edge box loop in the rotated 2D frame. */
static void collider2d_box_loop(float cx, float cy, float z, float c, float s,
                                float hx, float hy, uint32_t abgr)
{
    jce_vec3 p0 = collider2d_pt(cx, cy, z, c, s, -hx, -hy);
    jce_vec3 p1 = collider2d_pt(cx, cy, z, c, s,  hx, -hy);
    jce_vec3 p2 = collider2d_pt(cx, cy, z, c, s,  hx,  hy);
    jce_vec3 p3 = collider2d_pt(cx, cy, z, c, s, -hx,  hy);
    jce_debug_draw_line(p0, p1, abgr);
    jce_debug_draw_line(p1, p2, abgr);
    jce_debug_draw_line(p2, p3, abgr);
    jce_debug_draw_line(p3, p0, abgr);
}

/* Draw a JceCollider2DComponent as line loops in the XY plane (the 2D
 * world is XY).  Mirrors the RUNTIME interpretation exactly
 * (jce_runtime.c rt_spawn_body2d): `offset` is added to the entity
 * position UNrotated and UNscaled (the shape then rotates about that
 * body origin by the transform's Z angle), box/edge extents scale by the
 * entity's |XY| scale, the circle/capsule radius scales by max(|sx|,|sy|),
 * and the capsule follows capsule_direction (0 = length along Y,
 * 1 = along X) exactly as the runtime now does.  Edge/polygon connect the
 * AUTHORED points array
 * when present; with no points they fall back to the shapes the runtime
 * actually spawns (horizontal segment / bounding box). */
static void draw_collider2d_outline(const JceTransform *t,
                                    const JceCollider2DComponent *col,
                                    uint32_t abgr_box, uint32_t abgr_circle,
                                    uint32_t abgr_capsule)
{
    jce_vec3 ss = jce_v3_abs_safe_scale(t->scale);
    float sx = ss.x, sy = ss.y;
    float smax = fmaxf(sx, sy);

    /* Body origin = entity position + world-axis offset (NOT rotated,
     * NOT scaled — matching rt_spawn_body2d). */
    float cx = t->position.x + col->offset[0];
    float cy = t->position.y + col->offset[1];
    float z  = t->position.z;

    /* Z-rotation angle recovered from the quaternion the same way the
     * runtime seeds the Box2D body angle. */
    jce_quat q = t->rotation;
    float ang = atan2f(2.0f * (q.w * q.z + q.x * q.y),
                       1.0f - 2.0f * (q.y * q.y + q.z * q.z));
    float c = cosf(ang), s = sinf(ang);

    switch (col->shape) {
        case JCE_COLLIDER_2D_CIRCLE: {
            float r = ((col->radius > 0.0f) ? col->radius : 0.5f) * smax;
            const int seg = 24;
            jce_vec3 prev = jce_v3(cx + r, cy, z);
            for (int k = 1; k <= seg; k++) {
                float a = (float)k * (6.2831853f / (float)seg);
                jce_vec3 p = jce_v3(cx + cosf(a) * r, cy + sinf(a) * r, z);
                jce_debug_draw_line(prev, p, abgr_circle);
                prev = p;
            }
            break;
        }
        case JCE_COLLIDER_2D_CAPSULE: {
            /* Stadium along the AUTHORED axis: cap centres at ±hl, radius r.
             * capsule_direction used to be ignored here and in the runtime
             * alike; both honour it now, so the two still agree. */
            const bool horiz = (col->capsule_direction == 1);
            float r  = ((col->radius > 0.0f) ? col->radius : 0.25f) * smax;
            float len = horiz ? ((col->size[0] > 0.0f) ? col->size[0] : 1.0f)
                              : ((col->size[1] > 0.0f) ? col->size[1] : 1.0f);
            float hl = 0.5f * len * (horiz ? sx : sy);
            /* (across, along) -> local (x, y): the loops below are written in
             * capsule space so one body draws both axes. */
            auto cap = [&](float across, float along) {
                return horiz ? collider2d_pt(cx, cy, z, c, s, along, across)
                             : collider2d_pt(cx, cy, z, c, s, across, along);
            };
            const int seg = 12;     /* per semicircle cap */
            jce_vec3 prev = cap(r, -hl);
            /* Bottom cap: 0 → -π. */
            for (int k = 1; k <= seg; k++) {
                float a = -(float)k * (3.1415927f / (float)seg);
                jce_vec3 p = cap(cosf(a) * r, -hl + sinf(a) * r);
                jce_debug_draw_line(prev, p, abgr_capsule);
                prev = p;
            }
            /* Far side up to the far-cap start. */
            jce_vec3 tl = cap(-r, hl);
            jce_debug_draw_line(prev, tl, abgr_capsule);
            prev = tl;
            /* Top cap: π → 0. */
            for (int k = 1; k <= seg; k++) {
                float a = 3.1415927f - (float)k * (3.1415927f / (float)seg);
                jce_vec3 p = cap(cosf(a) * r, hl + sinf(a) * r);
                jce_debug_draw_line(prev, p, abgr_capsule);
                prev = p;
            }
            /* Near side back down to the start. */
            jce_debug_draw_line(prev, cap(r, -hl), abgr_capsule);
            break;
        }
        case JCE_COLLIDER_2D_EDGE:
        case JCE_COLLIDER_2D_POLYGON: {
            int n = col->point_count;
            if (n > JCE_COLLIDER_2D_MAX_POINTS) n = JCE_COLLIDER_2D_MAX_POINTS;
            bool poly = (col->shape == JCE_COLLIDER_2D_POLYGON);
            if (n >= 2) {
                jce_vec3 first = collider2d_pt(cx, cy, z, c, s,
                                               col->points[0][0] * sx,
                                               col->points[0][1] * sy);
                jce_vec3 prev = first;
                for (int k = 1; k < n; k++) {
                    jce_vec3 p = collider2d_pt(cx, cy, z, c, s,
                                               col->points[k][0] * sx,
                                               col->points[k][1] * sy);
                    jce_debug_draw_line(prev, p, abgr_box);
                    prev = p;
                }
                if (poly && n >= 3)   /* close the loop */
                    jce_debug_draw_line(prev, first, abgr_box);
            } else if (!poly) {
                /* No points authored — the runtime spawns a horizontal
                 * segment spanning size.x. */
                float hx = 0.5f * ((col->size[0] > 0.0f) ? col->size[0] : 1.0f) * sx;
                jce_debug_draw_line(collider2d_pt(cx, cy, z, c, s, -hx, 0.0f),
                                    collider2d_pt(cx, cy, z, c, s,  hx, 0.0f),
                                    abgr_box);
            } else {
                /* Degenerate polygon — the runtime falls back to the
                 * collider's bounding box; draw that. */
                collider2d_box_loop(cx, cy, z, c, s,
                    0.5f * ((col->size[0] > 0.0f) ? col->size[0] : 1.0f) * sx,
                    0.5f * ((col->size[1] > 0.0f) ? col->size[1] : 1.0f) * sy,
                    abgr_box);
            }
            break;
        }
        case JCE_COLLIDER_2D_BOX:
        default: {
            collider2d_box_loop(cx, cy, z, c, s,
                0.5f * ((col->size[0] > 0.0f) ? col->size[0] : 1.0f) * sx,
                0.5f * ((col->size[1] > 0.0f) ? col->size[1] : 1.0f) * sy,
                abgr_box);
            break;
        }
    }
}

/* Draws a highlight on every selected entity:
 *   - With a static mesh        → wireframe overlay on the actual geometry.
 *   - Point/Spot light          → real influence sphere or cone.
 *   - Camera                    → real view frustum (fov / near / far).
 *   - Box/Sphere/Capsule/CC     → real collider shape.
 *   - Anything else (empties,
 *     audio sources, particles,
 *     prefab roots, …)         → AABB sized by transform.scale.
 *
 * Goal: selection feedback that visually matches each entity's actual
 * shape — not just static meshes. */
/* ── Cinematic dolly-path gizmo (Sequencer rail in the viewport) ──────────
 *
 * Standard-engine feedback (Unreal Sequencer rail / Unity Cinemachine dolly
 * track): when a SequencePlayer entity is selected, draw the camera path its
 * sequence animates — sample the bound entity's POSITION property tracks over
 * the whole duration and stroke a polyline, with a cross marker at each key.
 * The path is the absolute world track (POSITION props are absolute), so it
 * matches exactly what the runtime drives at Play. */

struct SeqFindCtx { const char *want; jce_vec3 pos; bool found; };
static void seq_find_pos_cb(JceScene *s, JceEntity e, void *ud)
{
    SeqFindCtx *c = (SeqFindCtx *)ud;
    if (c->found) return;
    JceEditorMeta *m = jce_scene_get_editor_meta(s, e);
    if (m && m->name[0] && std::strcmp(m->name, c->want) == 0) {
        JceTransform *t = jce_scene_get_transform(s, e);
        if (t) { c->pos = t->position; c->found = true; }
    }
}

/* Cache the parsed sequence by resolved path; reload only when the path
 * changes (the gizmo overlay runs every frame while an entity is selected). */
static JceSequencer *seq_path_cache_get(const char *seq_path)
{
    static std::string  s_path;
    static JceSequencer *s_seq = nullptr;
    char resolved[1024];
    const char *p = seq_path;
    if (jce_editor_resolve_asset_path(seq_path, resolved, (int)sizeof resolved))
        p = resolved;
    if (s_seq && s_path == p) return s_seq;
    if (s_seq) { jce_sequencer_free(s_seq); s_seq = nullptr; }
    s_seq  = jce_sequencer_load_file(p);
    s_path = p;
    return s_seq;
}

static void draw_sequence_camera_path(JceScene *scene, const char *seq_path)
{
    JceSequencer *seq = seq_path_cache_get(seq_path);
    if (!seq) return;
    float dur = jce_sequencer_duration(seq);
    if (dur <= 0.0f) dur = 1.0f;
    const int n = jce_sequencer_track_count(seq);

    /* Group POS x/y/z property tracks by bound entity name (one path each). */
    struct PathGroup { char ent[96]; int tx, ty, tz; };
    PathGroup groups[8];
    int gcount = 0;
    for (int i = 0; i < n; i++) {
        if (jce_sequencer_track_type(seq, i) != JCE_SEQ_TRACK_PROPERTY) continue;
        const char *prop = jce_sequencer_track_bind_prop_name(seq, i);
        const char *ent  = jce_sequencer_track_bind_entity_name(seq, i);
        if (!ent || !ent[0] || !prop || !prop[0]) continue;
        int axis = (std::strcmp(prop, "transform.position.x") == 0) ? 0
                 : (std::strcmp(prop, "transform.position.y") == 0) ? 1
                 : (std::strcmp(prop, "transform.position.z") == 0) ? 2 : -1;
        if (axis < 0) continue;
        PathGroup *g = nullptr;
        for (int k = 0; k < gcount; k++)
            if (std::strcmp(groups[k].ent, ent) == 0) { g = &groups[k]; break; }
        if (!g && gcount < (int)(sizeof groups / sizeof groups[0])) {
            g = &groups[gcount++];
            std::snprintf(g->ent, sizeof g->ent, "%s", ent);
            g->tx = g->ty = g->tz = -1;
        }
        if (!g) continue;
        if      (axis == 0) g->tx = i;
        else if (axis == 1) g->ty = i;
        else                g->tz = i;
    }

    const uint32_t col_path = 0xFF18C0FF;   /* ABGR: warm amber rail        */
    const uint32_t col_key  = 0xFFFFFFFF;   /* white keyframe crosses       */
    for (int k = 0; k < gcount; k++) {
        PathGroup &g = groups[k];
        if (g.tx < 0 && g.ty < 0 && g.tz < 0) continue;
        /* Base position for any non-animated axis = the rig's current pose. */
        jce_vec3 base = jce_v3(0.0f, 0.0f, 0.0f);
        SeqFindCtx fc = { g.ent, jce_v3(0,0,0), false };
        jce_scene_each_entity(scene, seq_find_pos_cb, &fc);
        if (fc.found) base = fc.pos;

        /* LIVE position marker (green box) at the rig's current transform.  The
         * Sequencer panel's scrub/play preview writes that transform, so this
         * box rides the rail in real time as the playhead moves — the "live
         * motion" feedback on top of the static path. */
        if (fc.found) {
            const float mr = 0.26f;
            jce_debug_draw_box(base, jce_v3(mr, mr, mr), jce_q_identity(),
                               0xFFFFFF00 /* ABGR cyan — live playhead position */);
        }

        const int STEPS = 64;
        jce_vec3 prev = base, p0 = base, pN = base;
        for (int s2 = 0; s2 <= STEPS; s2++) {
            float t = dur * (float)s2 / (float)STEPS;
            jce_vec3 cur = base;
            if (g.tx >= 0) cur.x = jce_sequencer_track_eval_float(seq, g.tx, t);
            if (g.ty >= 0) cur.y = jce_sequencer_track_eval_float(seq, g.ty, t);
            if (g.tz >= 0) cur.z = jce_sequencer_track_eval_float(seq, g.tz, t);
            if (s2 == 0) p0 = cur;
            pN = cur;
            if (s2 > 0) {
                jce_debug_draw_line(prev, cur, col_path);
                /* A few travel-direction arrowheads (kept sparse — refined, not
                 * busy). */
                if (s2 % 16 == 0) {
                    jce_vec3 d = jce_v3_sub(cur, prev);
                    float dl = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
                    if (dl > 1e-4f) {
                        d = jce_v3_scale(d, 1.0f / dl);
                        jce_vec3 up   = (fabsf(d.y) > 0.95f) ? jce_v3(1, 0, 0)
                                                            : jce_v3(0, 1, 0);
                        jce_vec3 side = jce_v3_normalize(jce_v3_cross(d, up));
                        const float ah = 0.22f;
                        jce_vec3 back = jce_v3_scale(d, -ah);
                        jce_debug_draw_line(cur,
                            jce_v3_add(cur, jce_v3_add(back, jce_v3_scale(side,  ah * 0.6f))), col_path);
                        jce_debug_draw_line(cur,
                            jce_v3_add(cur, jce_v3_add(back, jce_v3_scale(side, -ah * 0.6f))), col_path);
                    }
                }
            }
            prev = cur;
        }
        /* Endpoint markers: green = start (t=0), red = end (t=dur) — direction
         * of the shot at a glance. */
        jce_debug_draw_box(p0, jce_v3(0.16f, 0.16f, 0.16f), jce_q_identity(), 0xFF22FF22);
        jce_debug_draw_box(pN, jce_v3(0.16f, 0.16f, 0.16f), jce_q_identity(), 0xFF2222FF);
        /* Keyframe crosses (use whichever axis track exists for the times). */
        int kt = g.tx >= 0 ? g.tx : (g.ty >= 0 ? g.ty : g.tz);
        int kc = jce_sequencer_track_key_count(seq, kt);
        for (int j = 0; j < kc; j++) {
            float tk = jce_sequencer_track_key_time(seq, kt, j);
            jce_vec3 p = base;
            if (g.tx >= 0) p.x = jce_sequencer_track_eval_float(seq, g.tx, tk);
            if (g.ty >= 0) p.y = jce_sequencer_track_eval_float(seq, g.ty, tk);
            if (g.tz >= 0) p.z = jce_sequencer_track_eval_float(seq, g.tz, tk);
            const float r = 0.18f;
            jce_debug_draw_line(jce_v3(p.x - r, p.y, p.z), jce_v3(p.x + r, p.y, p.z), col_key);
            jce_debug_draw_line(jce_v3(p.x, p.y - r, p.z), jce_v3(p.x, p.y + r, p.z), col_key);
            jce_debug_draw_line(jce_v3(p.x, p.y, p.z - r), jce_v3(p.x, p.y, p.z + r), col_key);
        }
    }
}

void draw_selection_outlines(void)
{
    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    if (sel_count == 0) return;

    float flat_dir[4]    = { 0.0f, -1.0f, 0.0f, -1.0f };
    float flat_color[4]  = { 1.0f, 0.75f, 0.0f, 1.0f };

    JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);

    /* ABGR (debug-draw convention). Orange-amber matches the
     * wireframe overlay tone used for selected meshes. */
    const uint32_t col_outline = 0xFF00BFFF;
    bool drew_any_debug = false;

    JceScene *scene = jce_state_get_scene();

    for (int i = 0; i < sel_count; i++) {
        uint32_t id = sel[i];
        if (id == 0 || !jce_state_entity_exists(id)) continue;
        if (!jce_state_entity_enabled(id)) continue;

        JceEntity e = (JceEntity)id;

        /* Cinematic dolly rail: a selected SequencePlayer draws its camera
         * path in the viewport (standard-engine Sequencer feedback).  Done up
         * front so it shows even though the Cutscene entity carries no mesh and
         * would otherwise take an early path below. */
        if (scene && jce_scene_has_sequence_player(scene, e)) {
            JceSequencePlayerComponent *sp = jce_scene_get_sequence_player(scene, e);
            if (sp && sp->seq_path[0])
                draw_sequence_camera_path(scene, sp->seq_path);
        }

        bool has_visual_renderer = false;
        if (scene) {
            has_visual_renderer =
                jce_scene_has_mesh_renderer(scene, e) ||
                jce_scene_has_skeletal_animator(scene, e);
        }

        /* Light DIRECTION indicator — drawn for EVERY selected light here, up
           front, so it stays visible even when the light entity also carries a
           model/mesh (e.g. a lantern). Such an entity takes the wireframe-overlay
           path below and `continue`s before the old per-shape light blocks, which
           is why the cone / arrow vanished for lights with a model. This is now
           the SINGLE place the light gizmo is drawn (the later light blocks were
           removed). `has_visual_renderer` is NOT a sufficient guard because a
           model can be detected by build_overlay_entity_model without being a
           mesh-renderer/skeletal-animator, so we fire for any light. */
        bool drew_light_early = false;
        if (scene) {
            JceTransform *lt = jce_scene_get_transform(scene, e);
            if (lt) {
                if (jce_scene_has_spot_light(scene, e)) {
                    JceSpotLight *sl = jce_scene_get_spot_light(scene, e);
                    if (sl) {
                        /* Match the renderer (sr_light_world_shine_direction):
                         * the component direction is rotated by the entity
                         * transform, so the cone follows the light when rotated
                         * (was drawing the RAW component dir = static/fake). */
                        jce_vec3 d = jce_v3_normalize(sl->direction);
                        if (d.x == 0.0f && d.y == 0.0f && d.z == 0.0f)
                            d = jce_v3(0.0f, 0.0f, -1.0f);
                        jce_vec3 axis = jce_v3_normalize(jce_q_rotate(lt->rotation, d));
                        float clen = (sl->radius > 0.0f) ? sl->radius : 1.0f;
                        float cosA = (sl->outer_cone_cos > 0.0f)
                                     ? sl->outer_cone_cos : 0.7071f;
                        if (cosA > 0.9999f) cosA = 0.9999f;
                        outline_draw_cone(lt->position, axis, clen,
                                          acosf(cosA), col_outline);
                    }
                }
                if (jce_scene_has_dir_light(scene, e)) {
                    JceDirectionalLight *dl = jce_scene_get_dir_light(scene, e);
                    if (dl) {
                        jce_vec3 d = jce_v3_normalize(dl->direction);
                        if (d.x == 0.0f && d.y == 0.0f && d.z == 0.0f)
                            d = jce_v3(0.0f, -1.0f, 0.0f);
                        d = jce_v3_normalize(jce_q_rotate(lt->rotation, d));
                        const float dlen = 3.0f;
                        jce_vec3 tip = jce_v3_add(lt->position,
                                                  jce_v3_scale(d, dlen));
                        jce_debug_draw_line(lt->position, tip, col_outline);
                        jce_vec3 up    = (fabsf(d.y) > 0.95f) ? jce_v3(1, 0, 0)
                                                              : jce_v3(0, 1, 0);
                        jce_vec3 right = jce_v3_normalize(jce_v3_cross(d, up));
                        jce_vec3 back  = jce_v3_scale(d, -0.5f);
                        jce_debug_draw_line(tip,
                            jce_v3_add(tip, jce_v3_add(back,
                                jce_v3_scale(right,  0.3f))), col_outline);
                        jce_debug_draw_line(tip,
                            jce_v3_add(tip, jce_v3_add(back,
                                jce_v3_scale(right, -0.3f))), col_outline);
                    }
                }
                if (jce_scene_has_point_light(scene, e)) {
                    JcePointLight *pl = jce_scene_get_point_light(scene, e);
                    float r = (pl && pl->radius > 0.0f) ? pl->radius : 1.0f;
                    jce_debug_draw_sphere(lt->position, r, col_outline);
                }
                drew_light_early = jce_scene_has_spot_light(scene, e)
                                || jce_scene_has_dir_light(scene, e)
                                || jce_scene_has_point_light(scene, e);
            }
        }
        /* The early light gizmo must trigger the end-of-function debug-draw FLUSH
           even when this entity then takes the wireframe-overlay `continue` path
           below — that path skips the `drew_any_debug = true` at the loop end, so
           without this the gizmo is accumulated into the buffer but never flushed
           and disappears entirely (the regression that hid ALL light gizmos). */
        if (drew_light_early) drew_any_debug = true;

        /* --- Rendered mesh: keep the high-fidelity wireframe overlay.
           NOTE: the per-iteration uniform/texture sets below are REQUIRED —
           do NOT hoist them out of the loop. Every wireframe submit discards
           all draw state (BGFX_DISCARD_ALL), and bgfx replays each draw's
           recorded uniform range in view-SORTED order, so a submit without
           its own preceding sets goes out with no texture bound and an empty
           uniform range that inherits another draw's values. */
        jce_mat4 model;
        JceMesh *mesh = NULL;
        if (build_overlay_entity_model(id, &model, &mesh) && mesh) {
            jce_set_transform(model.raw[0], 1);
            jce_uniform_set(s_sr.u_light_dir,   flat_dir,   1);
            jce_uniform_set(s_sr.u_light_color, flat_color, 1);
            jce_set_texture(0, uh, s_sr.white_tex, JCE_SAMPLER_INHERIT);
            jce_mesh_submit_wireframe_overlay(mesh, s_sr.renderer,
                                              scene_view_id());
            continue;
        }

        if (!scene) continue;
        JceTransform *t = jce_scene_get_transform(scene, e);
        if (!t) continue;

        bool drew_shape = drew_light_early;   /* light gizmo already drawn above */

        /* --- Skeletal-animated (skinned) model.
         *     Submit the true geometric wireframe of every primitive in
         *     the cached JceModel, walking the full node hierarchy. The
         *     bone palette comes from the live animation player when
         *     available; otherwise the bind pose is used. */
        if (jce_scene_has_skeletal_animator(scene, e)) {
            JceSkeletalAnimatorComponent *sa =
                jce_scene_get_skeletal_animator(scene, e);
            JceModel *mdl = (sa && sa->skeleton_path[0])
                ? jce_editor_scene_get_model(sa->skeleton_path, id)
                : NULL;
            if (mdl) {
                /* Same composition the engine skinned draw uses (the
                 * hierarchical, pivot-aware world matrix) so the overlay
                 * stays glued to the rendered mesh. */
                jce_mat4 world = jce_scene_get_world_matrix(scene, e);
                jce_uniform_set(s_sr.u_light_dir,   flat_dir,   1);
                jce_uniform_set(s_sr.u_light_color, flat_color, 1);
                jce_set_texture(0, uh, s_sr.white_tex, JCE_SAMPLER_INHERIT);
                jce_model_submit_wireframe_overlay(mdl, s_sr.renderer,
                                                    scene_view_id(),
                                                    &world, NULL, 0);
                continue;
            }
        }

        if (!jce_editor_scene_outline_should_use_debug_fallback(
                has_visual_renderer,
                drew_shape)) {
            continue;
        }

        /* (Point / spot / directional light gizmos are drawn earlier — see the
           "Light DIRECTION indicator" block above — so they remain visible even
           for lights that also carry a model/mesh.) */

        /* --- Camera → real view frustum. */
        if (jce_scene_has_camera(scene, e)) {
            JceCameraComponent *cc = jce_scene_get_camera(scene, e);
            if (cc) {
                outline_draw_frustum(t->position, t->rotation,
                                     (cc->fov_deg > 0.0f) ? cc->fov_deg : 60.0f,
                                     (cc->near_plane > 0.0f) ? cc->near_plane : 0.1f,
                                     (cc->far_plane > cc->near_plane) ? cc->far_plane : 100.0f,
                                     cc->ortho, col_outline);
            }
            drew_shape = true;
        }

        /* --- Virtual camera (Cinemachine-style) → short preview frustum at
         *     the entity pose. The LIVE vcam pose may differ at runtime
         *     (follow/look-at targets resolve in jce_vcam_system_evaluate);
         *     the gizmo anchors at the selected entity so it tracks what the
         *     user is manipulating. Magenta keeps it distinct from the real
         *     camera frustum (amber col_outline above). */
        if (jce_scene_has_virtual_camera(scene, e)) {
            JceVirtualCameraComponent *vc =
                jce_scene_get_virtual_camera(scene, e);
            if (vc) {
                const uint32_t col_vcam = 0xFFFF00FFu; /* ABGR magenta */
                /* RESOLVED pose (mirrors jce_vcam_system resolve_pose): a vcam's
                 * actual view comes from its static position/look-at OR its
                 * follow / look-at TARGET entities — not the entity transform.
                 * Anchoring the frustum at the transform was the static/fake
                 * camera gizmo (esp. for FOLLOW_LOOK rigs like the dolly cam). */
                jce_vec3 pos = jce_v3(vc->position[0], vc->position[1], vc->position[2]);
                jce_vec3 tgt = jce_v3(vc->look_at[0],  vc->look_at[1],  vc->look_at[2]);
                if ((vc->track_mode == JCE_VCAM_COMP_TRACK_FOLLOW ||
                     vc->track_mode == JCE_VCAM_COMP_TRACK_FOLLOW_LOOK) &&
                    jce_state_entity_alive((uint32_t)vc->follow_target)) {
                    JceTransform *ft = jce_scene_get_transform(scene, (JceEntity)vc->follow_target);
                    if (ft) pos = jce_v3(ft->position.x + vc->follow_offset[0],
                                         ft->position.y + vc->follow_offset[1],
                                         ft->position.z + vc->follow_offset[2]);
                }
                if ((vc->track_mode == JCE_VCAM_COMP_TRACK_LOOK_AT ||
                     vc->track_mode == JCE_VCAM_COMP_TRACK_FOLLOW_LOOK) &&
                    jce_state_entity_alive((uint32_t)vc->look_at_target)) {
                    JceTransform *lkt = jce_scene_get_transform(scene, (JceEntity)vc->look_at_target);
                    if (lkt) tgt = lkt->position;
                }
                /* Unconfigured static vcam (no authored pos/look-at) → fall back
                 * to the entity transform so a fresh vcam still shows something. */
                bool zero_pos = (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f);
                bool zero_tgt = (tgt.x == 0.0f && tgt.y == 0.0f && tgt.z == 0.0f);
                if (vc->track_mode == JCE_VCAM_COMP_TRACK_NONE && zero_pos && zero_tgt) {
                    pos = t->position;
                    tgt = jce_v3_add(pos, jce_q_rotate(t->rotation, jce_v3(0, 0, -1)));
                }
                outline_draw_frustum_lookat(pos, tgt,
                                     (vc->fov_deg > 0.0f) ? vc->fov_deg : 60.0f,
                                     0.1f, 3.5f, col_vcam);
                /* Mark the resolved camera position. */
                jce_debug_draw_box(pos, jce_v3(0.12f, 0.12f, 0.12f),
                                   jce_q_identity(), col_vcam);
                drew_shape = true;
            }
        }

        /* --- Collider shapes (real geometry).  size/radius are
         *     interpreted in local entity space and scaled by the
         *     entity's TRS scale, matching Unity-style authoring. */
        if (jce_scene_has_box_collider(scene, e)) {
            JceBoxColliderComponent *bc = jce_scene_get_box_collider(scene, e);
            jce_vec3 ofs = bc
                ? jce_v3(bc->center[0], bc->center[1], bc->center[2])
                : jce_v3(0, 0, 0);
            jce_vec3 ss = jce_v3_abs_safe_scale(t->scale);
            float sx = ss.x, sy = ss.y, sz = ss.z;
            float bx = bc ? bc->size[0] : 1.0f;
            float by = bc ? bc->size[1] : 1.0f;
            float bz = bc ? bc->size[2] : 1.0f;
            jce_vec3 half = jce_v3(0.5f * bx * sx, 0.5f * by * sy, 0.5f * bz * sz);
            jce_vec3 c = jce_v3_add(t->position,
                              jce_q_rotate(t->rotation,
                                  jce_v3(ofs.x * sx, ofs.y * sy, ofs.z * sz)));
            jce_debug_draw_box(c, half, t->rotation, col_outline);
            drew_shape = true;
        }
        if (jce_scene_has_sphere_collider(scene, e)) {
            JceSphereColliderComponent *sc = jce_scene_get_sphere_collider(scene, e);
            jce_vec3 ofs = sc
                ? jce_v3(sc->center[0], sc->center[1], sc->center[2])
                : jce_v3(0, 0, 0);
            jce_vec3 ss = jce_v3_abs_safe_scale(t->scale);
            float sx = ss.x, sy = ss.y, sz = ss.z;
            float smax = fmaxf(sx, fmaxf(sy, sz));
            float r = ((sc && sc->radius > 0.0f) ? sc->radius : 0.5f) * smax;
            jce_vec3 c = jce_v3_add(t->position,
                              jce_q_rotate(t->rotation,
                                  jce_v3(ofs.x * sx, ofs.y * sy, ofs.z * sz)));
            jce_debug_draw_sphere(c, r, col_outline);
            drew_shape = true;
        }
        if (jce_scene_has_capsule_collider(scene, e)) {
            JceCapsuleColliderComponent *cc = jce_scene_get_capsule_collider(scene, e);
            jce_vec3 ofs = cc
                ? jce_v3(cc->center[0], cc->center[1], cc->center[2])
                : jce_v3(0, 0, 0);
            jce_vec3 ss = jce_v3_abs_safe_scale(t->scale);
            float sx = ss.x, sy = ss.y, sz = ss.z;
            /* Follow the AUTHORED axis, exactly as the runtime now does.
             * jce_debug_draw_capsule draws Y-aligned and already takes a
             * rotation, so the axis composes into that -- no new draw call
             * and no second place for draw and physics to disagree. */
            int cax = cc ? cc->axis : 1;
            if (cax < 0 || cax > 2) cax = 1;
            float a0 = (cax == 0) ? sy : sx;
            float a1 = (cax == 2) ? sy : sz;
            float r_scale = fmaxf(a0, a1);
            float len_scale = (cax == 0) ? sx : ((cax == 2) ? sz : sy);
            float r = ((cc && cc->radius > 0.0f) ? cc->radius : 0.3f) * r_scale;
            float h = ((cc && cc->height > 0.0f) ? cc->height : 1.0f) * len_scale;
            float hh = 0.5f * fmaxf(0.0f, h - 2.0f * r);
            jce_vec3 c = jce_v3_add(t->position,
                              jce_q_rotate(t->rotation,
                                  jce_v3(ofs.x * sx, ofs.y * sy, ofs.z * sz)));
            /* Y -> X is -90 deg about Z; Y -> Z is +90 deg about X. */
            jce_quat axis_q = (cax == 0)
                ? jce_q_from_axis_angle(jce_v3(0.0f, 0.0f, 1.0f), -1.5707963f)
                : (cax == 2)
                ? jce_q_from_axis_angle(jce_v3(1.0f, 0.0f, 0.0f),  1.5707963f)
                : jce_q_identity();
            jce_debug_draw_capsule(c, r, hh, jce_q_multiply(t->rotation, axis_q),
                                   col_outline);
            drew_shape = true;
        }
        if (jce_scene_has_character_controller(scene, e)) {
            JceCharacterControllerComponent *cc =
                jce_scene_get_character_controller(scene, e);
            jce_vec3 ss = jce_v3_abs_safe_scale(t->scale);
            float sx = ss.x, sy = ss.y, sz = ss.z;
            float r_scale = fmaxf(sx, sz);
            float r = ((cc && cc->radius > 0.0f) ? cc->radius : 0.3f) * r_scale;
            float h = ((cc && cc->height > 0.0f) ? cc->height : 1.6f) * sy;
            float hh = 0.5f * fmaxf(0.0f, h - 2.0f * r);
            /* The CharacterController entity Transform is the FEET; the capsule
             * is centered half its height above that, so offset the draw up by
             * (hh + r). Without this the capsule renders half-buried underground. */
            jce_vec3 cap_c = t->position; cap_c.y += hh + r;
            jce_debug_draw_capsule(cap_c, r, hh, t->rotation, col_outline);
            drew_shape = true;
        }
        /* Mesh collider: the real fitted wireframe comes from the dedicated
         * gizmo pass (draw_compound_collider_gizmos) — just suppress the
         * generic unit-box fallback for mesh-collider-only entities. */
        if (jce_scene_has_mesh_collider(scene, e))
            drew_shape = true;

        /* --- 2D collider (Box2D body, XY plane). */
        if (jce_scene_has_collider2d(scene, e)) {
            JceCollider2DComponent *c2 = jce_scene_get_collider2d(scene, e);
            if (c2) {
                draw_collider2d_outline(t, c2, col_outline, col_outline,
                                        col_outline);
                drew_shape = true;
            }
        }

        /* --- Trigger volume (gameplay region, not a collider).  Mirrors the
         *     RUNTIME interpretation exactly (jce_runtime.c rt trigger sync):
         *     center is a world-axis offset from the entity position — NOT
         *     rotated by the entity and NOT scaled — and the OBB basis comes
         *     from the component's own axes. Drawing anything else would lie
         *     about where the trigger actually fires. */
        if (jce_scene_has_trigger_volume(scene, e)) {
            JceTriggerVolumeComponent *tv = jce_scene_get_trigger_volume(scene, e);
            if (tv) {
                jce_vec3 c = jce_v3(t->position.x + tv->center[0],
                                    t->position.y + tv->center[1],
                                    t->position.z + tv->center[2]);
                if (tv->shape == JCE_TRIGGER_VOL_SPHERE) {
                    float r = (tv->half_extents[0] > 0.0f)
                            ? tv->half_extents[0] : 0.5f;
                    jce_debug_draw_sphere(c, r, col_outline);
                } else if (tv->shape == JCE_TRIGGER_VOL_OBB) {
                    /* 12 edges from the component's own basis. */
                    jce_vec3 ax = jce_v3(tv->axis_x[0], tv->axis_x[1], tv->axis_x[2]);
                    jce_vec3 ay = jce_v3(tv->axis_y[0], tv->axis_y[1], tv->axis_y[2]);
                    jce_vec3 az = jce_v3(tv->axis_z[0], tv->axis_z[1], tv->axis_z[2]);
                    ax = jce_v3_scale(ax, tv->half_extents[0]);
                    ay = jce_v3_scale(ay, tv->half_extents[1]);
                    az = jce_v3_scale(az, tv->half_extents[2]);
                    jce_vec3 corners[8];
                    for (int ci = 0; ci < 8; ci++) {
                        jce_vec3 p = c;
                        p = jce_v3_add(p, jce_v3_scale(ax, (ci & 1) ? 1.0f : -1.0f));
                        p = jce_v3_add(p, jce_v3_scale(ay, (ci & 2) ? 1.0f : -1.0f));
                        p = jce_v3_add(p, jce_v3_scale(az, (ci & 4) ? 1.0f : -1.0f));
                        corners[ci] = p;
                    }
                    static const int edges[12][2] = {
                        {0,1},{2,3},{4,5},{6,7},   /* x edges */
                        {0,2},{1,3},{4,6},{5,7},   /* y edges */
                        {0,4},{1,5},{2,6},{3,7},   /* z edges */
                    };
                    for (int ei = 0; ei < 12; ei++)
                        jce_debug_draw_line(corners[edges[ei][0]],
                                            corners[edges[ei][1]], col_outline);
                } else { /* AABB — axis-aligned, ignores entity rotation */
                    jce_vec3 half = jce_v3(tv->half_extents[0],
                                           tv->half_extents[1],
                                           tv->half_extents[2]);
                    jce_debug_draw_box(c, half, jce_q_identity(), col_outline);
                }
                drew_shape = true;
            }
        }

        /* --- AI perception senses (BehaviorTree component).  Mirrors the
         *     runtime perception binding (rt_spawn_gameplay): sight is a cone
         *     along the entity's forward (-Z, same convention as the camera
         *     frustum above), hearing is an omnidirectional radius around the
         *     entity position.  sight_half_angle is stored in RADIANS (the
         *     inspector converts from degrees on edit).  Values <=0 mean "use
         *     the engine default at spawn", so only explicitly authored
         *     ranges are drawn here. */
        if (jce_scene_has_behavior_tree(scene, e)) {
            JceBehaviorTree *bt = jce_scene_get_behavior_tree(scene, e);
            if (bt) {
                /* ABGR. Violet — deliberately distinct from the amber
                 * col_outline used for colliders/lights and from the physics
                 * debug palette; hearing is the same hue alpha-dimmed. */
                const uint32_t col_sight   = 0xFFFF40C0u;
                const uint32_t col_hearing = 0x66FF40C0u;
                if (bt->sight_range > 0.0f) {
                    jce_vec3 fwd = jce_q_rotate(t->rotation,
                                                jce_v3(0.0f, 0.0f, -1.0f));
                    float half = (bt->sight_half_angle > 0.0f)
                               ? bt->sight_half_angle
                               : 1.0472f;          /* engine default: 60° */
                    if (half > 1.55f) half = 1.55f; /* keep tanf() sane */
                    outline_draw_cone(t->position, fwd, bt->sight_range,
                                      half, col_sight);
                    drew_shape = true;
                }
                if (bt->hearing_range > 0.0f) {
                    jce_debug_draw_sphere(t->position, bt->hearing_range,
                                          col_hearing);
                    drew_shape = true;
                }
            }
        }

        /* --- Simulation-LOD tier rings (distance-tiered gameplay tick).
         *     Mirrors the runtime classification (rt_sim_lod_period): NEAR
         *     within near_radius (full / near_hz), MID out to mid_radius, FAR
         *     beyond.  Two concentric spheres at the entity position mark the
         *     near/mid band boundaries — the same world-anchored radius gizmo
         *     idiom as the reverb-zone / audio-source range rings above. */
        if (jce_scene_has_sim_lod(scene, e)) {
            JceSimLodComponent *sl = jce_scene_get_sim_lod(scene, e);
            if (sl && sl->enabled) {
                /* ABGR. Cyan = near band edge, dimmer cyan = mid band edge —
                 * distinct from the amber colliders and violet BT senses. */
                const uint32_t col_near = 0xFFFFD040u; /* near radius */
                const uint32_t col_mid  = 0x66FFD040u; /* mid radius (alpha-dim) */
                float nr = (sl->near_radius > 0.0f) ? sl->near_radius : 25.0f;
                float mr = (sl->mid_radius  > nr)   ? sl->mid_radius  : (nr + 55.0f);
                jce_debug_draw_sphere(t->position, nr, col_near);
                jce_debug_draw_sphere(t->position, mr, col_mid);
                drew_shape = true;
            }
        }

        /* --- Rendering / audio volume gizmos.  Each mirrors its CONSUMER's
         *     interpretation: the post-FX Volume box is AXIS-ALIGNED around
         *     the entity position (jce_volume_system.c ignores rotation),
         *     reflection-probe / light-probe offsets are world-axis and
         *     unscaled (sr_gather_baked_gi), and the reverb zone is a sphere
         *     at the entity position (rt_reverb_zone_collect).  All hues are
         *     alpha-dimmed (0x66 precedent above) and mutually distinct. */
        if (jce_scene_has_volume(scene, e)) {
            JceVolumeComponent *vol = jce_scene_get_volume(scene, e);
            /* Global volumes have no spatial bounds — nothing honest to
             * draw, so let the generic fallback box mark the entity. */
            if (vol && !vol->is_global) {
                const uint32_t col_vol = 0x6680FF40u; /* spring green */
                if (vol->shape == JCE_VOLUME_SHAPE_SPHERE) {
                    float r = (vol->extents.x > 0.0f) ? vol->extents.x : 0.5f;
                    jce_debug_draw_sphere(t->position, r, col_vol);
                } else {
                    jce_vec3 half = jce_v3(
                        (vol->extents.x > 0.0f) ? vol->extents.x : 0.5f,
                        (vol->extents.y > 0.0f) ? vol->extents.y : 0.5f,
                        (vol->extents.z > 0.0f) ? vol->extents.z : 0.5f);
                    jce_debug_draw_box(t->position, half, jce_q_identity(),
                                       col_vol);
                }
                drew_shape = true;
            }
        }
        if (jce_scene_has_reflection_probe(scene, e)) {
            JceReflectionProbeComponent *rp =
                jce_scene_get_reflection_probe(scene, e);
            if (rp) {
                const uint32_t col_rp = 0x66FFA040u; /* sky blue */
                jce_vec3 pc = jce_v3(t->position.x + rp->box_offset[0],
                                     t->position.y + rp->box_offset[1],
                                     t->position.z + rp->box_offset[2]);
                jce_vec3 half = jce_v3(
                    (rp->box_size[0] > 0.0f) ? 0.5f * rp->box_size[0] : 0.5f,
                    (rp->box_size[1] > 0.0f) ? 0.5f * rp->box_size[1] : 0.5f,
                    (rp->box_size[2] > 0.0f) ? 0.5f * rp->box_size[2] : 0.5f);
                jce_debug_draw_box(pc, half, jce_q_identity(), col_rp);
                drew_shape = true;
            }
        }
        if (jce_scene_has_audio_reverb_zone(scene, e)) {
            JceAudioReverbZoneComponent *rz =
                jce_scene_get_audio_reverb_zone(scene, e);
            if (rz) {
                /* Outer sphere = max_distance (blend-out edge), inner =
                 * min_distance (full strength); defaults per the runtime. */
                const uint32_t col_rz = 0x6620D0FFu; /* gold */
                float maxd = (rz->max_distance > 0.0f) ? rz->max_distance
                                                       : 10.0f;
                float mind = (rz->min_distance > 0.0f) ? rz->min_distance
                                                       : 0.0f;
                if (mind > maxd) mind = maxd;
                jce_debug_draw_sphere(t->position, maxd, col_rz);
                if (mind > 0.0f)
                    jce_debug_draw_sphere(t->position, mind, col_rz);
                drew_shape = true;
            }
        }
        /* --- Audio source hearing range.  Mirrors the runtime gate
         *     (rt_finish_audio_source): a voice goes 3D when spatial_blend > 0.5.
         *     The min/max distances are now AUTHORABLE on the component (0 =
         *     engine defaults 1 / 25), so the overlay reflects the same values
         *     the runtime feeds jce_audio_voice_set_attenuation.  Inner sphere =
         *     full-volume radius; dimmed outer sphere = the falloff distance
         *     clamp.  2D sources keep the generic fallback box. */
        if (jce_scene_has_audio_source(scene, e)) {
            JceAudioSourceComponent *as = jce_scene_get_audio_source(scene, e);
            /* Matches the runtime, which became CONTINUOUS: anything above 0
             * is spatial and has an attenuation radius worth drawing.  While
             * this said 0.5 a source authored at 0.3 was spatial in the mix
             * and had no gizmo. */
            if (as && as->spatial_blend > 0.0f) {
                float min_d = as->min_distance > 0.0f ? as->min_distance : 1.0f;
                float max_d = as->max_distance > 0.0f ? as->max_distance : 25.0f;
                const uint32_t col_as_min = 0xCC00C5CCu; /* olive */
                const uint32_t col_as_max = 0x6600C5CCu; /* olive, dimmed */
                jce_debug_draw_sphere(t->position, min_d, col_as_min);
                jce_debug_draw_sphere(t->position, max_d, col_as_max);
                drew_shape = true;
            }
        }
        if (jce_scene_has_light_probe_group(scene, e)) {
            JceLightProbeGroupComponent *lpg =
                jce_scene_get_light_probe_group(scene, e);
            if (lpg && lpg->probe_count > 0) {
                const uint32_t col_lp = 0x66F0F0F0u; /* pale grey */
                const float cr = 0.15f;   /* cross half-size */
                int n = lpg->probe_count;
                if (n > JCE_LIGHT_PROBE_MAX) n = JCE_LIGHT_PROBE_MAX;
                for (int pi = 0; pi < n; pi++) {
                    jce_vec3 p = jce_v3(t->position.x + lpg->positions[pi][0],
                                        t->position.y + lpg->positions[pi][1],
                                        t->position.z + lpg->positions[pi][2]);
                    jce_debug_draw_line(jce_v3(p.x - cr, p.y, p.z),
                                        jce_v3(p.x + cr, p.y, p.z), col_lp);
                    jce_debug_draw_line(jce_v3(p.x, p.y - cr, p.z),
                                        jce_v3(p.x, p.y + cr, p.z), col_lp);
                    jce_debug_draw_line(jce_v3(p.x, p.y, p.z - cr),
                                        jce_v3(p.x, p.y, p.z + cr), col_lp);
                }
                drew_shape = true;
            }
        }
        if (jce_scene_has_decal(scene, e)) {
            JceDecalComponent *dec = jce_scene_get_decal(scene, e);
            if (dec) {
                /* Projector box: width size[0] along local X, height size[1]
                 * along local Z, projecting depth size[2] down local -Y; the
                 * pivot is a world-axis offset of the box centre.  The stamp
                 * lands on the box's far (-Y) face — matching
                 * sr_decal_each_entity, defaults included. */
                const uint32_t col_dc = 0x66B040FFu; /* pink */
                float w = (dec->size[0] > 0.0f) ? dec->size[0] : 1.0f;
                float h = (dec->size[1] > 0.0f) ? dec->size[1] : w;
                float depth = (dec->size[2] > 0.0f) ? dec->size[2] : 1.0f;
                jce_vec3 down = jce_q_rotate(t->rotation,
                                             jce_v3(0.0f, -1.0f, 0.0f));
                jce_vec3 bc2 = jce_v3(t->position.x + dec->pivot[0],
                                      t->position.y + dec->pivot[1],
                                      t->position.z + dec->pivot[2]);
                jce_debug_draw_box(bc2,
                                   jce_v3(0.5f * w, 0.5f * depth, 0.5f * h),
                                   t->rotation, col_dc);
                /* Projection-direction stub down to the stamp face. */
                jce_vec3 face = jce_v3_add(bc2,
                                           jce_v3_scale(down, 0.5f * depth));
                jce_debug_draw_line(bc2, face, col_dc);
                drew_shape = true;
            }
        }

        /* --- IK constraint targets / poles: a cross + small sphere at each
         *     ENABLED constraint's target (and pole), plus a link from the
         *     rigged entity, so the authored IK goals are visible.  IK is
         *     already solved by the renderer; this is the authoring gizmo. */
        if (jce_scene_has_ik_constraints(scene, e)) {
            JceIkConstraintComponent *ik = jce_scene_get_ik_constraints(scene, e);
            if (ik) {
                const uint32_t col_ik   = 0x66FFC000u; /* cyan target */
                const uint32_t col_pole = 0x6600D0FFu; /* gold pole   */
                for (int ci = 0; ci < 16; ++ci) {
                    JceIkConstraint *c = &ik->constraints[ci];
                    if (!c->enabled) continue;
                    if (c->target_entity) {
                        jce_mat4 tm = jce_scene_get_world_matrix(scene, (JceEntity)c->target_entity);
                        jce_vec3 tp = jce_v3(tm.col[3].x, tm.col[3].y, tm.col[3].z);
                        float cr = 0.25f;
                        jce_debug_draw_line(jce_v3(tp.x-cr,tp.y,tp.z), jce_v3(tp.x+cr,tp.y,tp.z), col_ik);
                        jce_debug_draw_line(jce_v3(tp.x,tp.y-cr,tp.z), jce_v3(tp.x,tp.y+cr,tp.z), col_ik);
                        jce_debug_draw_line(jce_v3(tp.x,tp.y,tp.z-cr), jce_v3(tp.x,tp.y,tp.z+cr), col_ik);
                        jce_debug_draw_sphere(tp, 0.12f, col_ik);
                        jce_debug_draw_line(t->position, tp, col_ik);
                    }
                    bool have_pole = false; jce_vec3 pp = jce_v3(0,0,0);
                    if (c->pole_entity) {
                        jce_mat4 pm = jce_scene_get_world_matrix(scene, (JceEntity)c->pole_entity);
                        pp = jce_v3(pm.col[3].x, pm.col[3].y, pm.col[3].z); have_pole = true;
                    } else if (c->pole_offset[0]!=0.0f || c->pole_offset[1]!=0.0f || c->pole_offset[2]!=0.0f) {
                        pp = jce_v3(t->position.x+c->pole_offset[0],
                                    t->position.y+c->pole_offset[1],
                                    t->position.z+c->pole_offset[2]); have_pole = true;
                    }
                    if (have_pole) jce_debug_draw_sphere(pp, 0.1f, col_pole);
                }
                drew_shape = true;
            }
        }

        /* --- Foliage clusters + grass fields: custom-drawn (no MeshRenderer),
         *     so outline their ACTUAL spatial extent (shell sphere / area box)
         *     instead of the tiny generic fallback box. Lets the artist select
         *     and see the real bounds of leaves/grass in the viewport. */
        if (!drew_shape && scene && jce_scene_has_foliage_cluster(scene, e)) {
            JceFoliageClusterComponent *fc = jce_scene_get_foliage_cluster(scene, e);
            if (fc) {
                float r = fc->radius > 0.0f ? fc->radius : 1.0f;
                /* Squashed shell: box half-extents (r, r*squashY, r). */
                float sy = fc->squash_y > 0.0f ? fc->squash_y : 1.0f;
                jce_debug_draw_box(t->position, jce_v3(r, r * sy, r),
                                   t->rotation, col_outline);
                jce_debug_draw_sphere(t->position, r * 0.5f, col_outline);
                drew_shape = true;
            }
        }
        if (!drew_shape && scene && jce_scene_has_grass_field(scene, e)) {
            JceGrassFieldComponent *g = jce_scene_get_grass_field(scene, e);
            if (g) {
                float hx = 0.5f * (g->area_x > 0.0f ? g->area_x : 1.0f);
                float hz = 0.5f * (g->area_z > 0.0f ? g->area_z : 1.0f);
                float hy = 0.5f * (g->blade_height > 0.0f ? g->blade_height : 0.5f);
                jce_debug_draw_box(t->position, jce_v3(hx, hy, hz),
                                   t->rotation, col_outline);
                drew_shape = true;
            }
        }

        /* --- Generic fallback for transform-only entities (empties,
         *     non-spatial audio sources, particle emitters, prefab
         *     roots …). */
        if (!drew_shape) {
            jce_vec3 ss = jce_v3_abs_safe_scale(t->scale);
            float sx = ss.x, sy = ss.y, sz = ss.z;
            jce_vec3 half = jce_v3(0.5f * sx, 0.5f * sy, 0.5f * sz);
            jce_debug_draw_box(t->position, half, t->rotation, col_outline);
        }

        drew_any_debug = true;
    }

    if (drew_any_debug)
        jce_debug_draw_flush(scene_view_id(), s_sr.renderer);

    /* Restore default lighting so subsequent draws aren't tinted. */
    JceDirLight sun = jce_dir_light_default();
    jce_lighting_apply(s_sr.renderer, &sun);
}

/* ── Physics debug visualization ──────────────────────────────────── */

void draw_physics_debug(void)
{
    int count = jce_state_get_entity_count();
    if (count == 0) return;

    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    int sel_n = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_n);

    /* Range-cull the FITTED compound wireframe (Unity-style green mesh): only
     * the nearest few props to the camera get the detailed trimesh; the rest
     * get a cheap bounds box. Keeps it "real but not over-rendered" and bounds
     * the per-frame line count well under the debug-draw buffer. */
    jce_vec3 cam_pos = jce_camera_get_position(s_sr.camera);
    const float detail_dist2 = 22.0f * 22.0f;
    int detail_budget = 10;

    const uint32_t col_box     = 0xFF00FF00; /* green */
    const uint32_t col_sphere  = 0xFF00FFFF; /* cyan */
    const uint32_t col_capsule = 0xFFFFFF00; /* yellow */

    for (int i = 0; i < count; i++) {
        uint32_t id = jce_state_get_entity_id_by_index(i);
        if (id == 0 || !jce_state_entity_exists(id)) continue;
        if (!jce_state_entity_enabled(id)) continue;

        /* Skip the SELECTED entity: the selection pass already outlines its
         * collider (in orange). Drawing the green overlay on top of it fights
         * the selection outline. Non-selected colliders still show below. */
        bool is_selected = false;
        for (int s = 0; s < sel_n; ++s) if (sel[s] == id) { is_selected = true; break; }
        if (is_selected) continue;

        JceEntity e = (JceEntity)id;
        JceTransform *t = jce_scene_get_transform(scene, e);
        if (!t) continue;

        jce_quat q = t->rotation;
        /* size/center/radius/height are interpreted in local entity space and
         * scaled by the entity's TRS scale — matching how the physics body is
         * built (jce_runtime.c) and the selection gizmo. Previously this drew a
         * fixed 0.5*scale cube, so every collider looked like a unit cube. */
        jce_vec3 ss = jce_v3_abs_safe_scale(t->scale);
        float sx = ss.x, sy = ss.y, sz = ss.z;

        if (jce_scene_has_box_collider(scene, e)) {
            JceBoxColliderComponent *bc = jce_scene_get_box_collider(scene, e);
            jce_vec3 ofs = bc ? jce_v3(bc->center[0], bc->center[1], bc->center[2])
                              : jce_v3(0, 0, 0);
            float bx = bc ? bc->size[0] : 1.0f, by = bc ? bc->size[1] : 1.0f,
                  bz = bc ? bc->size[2] : 1.0f;
            jce_vec3 half = jce_v3(0.5f * bx * sx, 0.5f * by * sy, 0.5f * bz * sz);
            jce_vec3 c = jce_v3_add(t->position, jce_q_rotate(q,
                              jce_v3(ofs.x * sx, ofs.y * sy, ofs.z * sz)));
            jce_debug_draw_box(c, half, q, col_box);
        }
        if (jce_scene_has_sphere_collider(scene, e)) {
            JceSphereColliderComponent *sc = jce_scene_get_sphere_collider(scene, e);
            jce_vec3 ofs = sc ? jce_v3(sc->center[0], sc->center[1], sc->center[2])
                              : jce_v3(0, 0, 0);
            float r = ((sc && sc->radius > 0.0f) ? sc->radius : 0.5f)
                      * fmaxf(sx, fmaxf(sy, sz));
            jce_vec3 c = jce_v3_add(t->position, jce_q_rotate(q,
                              jce_v3(ofs.x * sx, ofs.y * sy, ofs.z * sz)));
            jce_debug_draw_sphere(c, r, col_sphere);
        }
        if (jce_scene_has_capsule_collider(scene, e)) {
            JceCapsuleColliderComponent *cc = jce_scene_get_capsule_collider(scene, e);
            jce_vec3 ofs = cc ? jce_v3(cc->center[0], cc->center[1], cc->center[2])
                              : jce_v3(0, 0, 0);
            float r = ((cc && cc->radius > 0.0f) ? cc->radius : 0.3f) * fmaxf(sx, sz);
            float h = ((cc && cc->height > 0.0f) ? cc->height : 1.0f) * sy;
            float hh = 0.5f * fmaxf(0.0f, h - 2.0f * r);
            jce_vec3 c = jce_v3_add(t->position, jce_q_rotate(q,
                              jce_v3(ofs.x * sx, ofs.y * sy, ofs.z * sz)));
            jce_debug_draw_capsule(c, r, hh, q, col_capsule);
        }
        if (jce_scene_has_character_controller(scene, e)) {
            JceCharacterControllerComponent *cc =
                jce_scene_get_character_controller(scene, e);
            float r = ((cc && cc->radius > 0.0f) ? cc->radius : 0.3f) * fmaxf(sx, sz);
            float h = ((cc && cc->height > 0.0f) ? cc->height : 1.6f) * sy;
            float hh = 0.5f * fmaxf(0.0f, h - 2.0f * r);
            jce_vec3 cc_c = t->position; cc_c.y += hh + r;  /* feet -> capsule center */
            jce_debug_draw_capsule(cc_c, r, hh, q, col_capsule);
        }
        /* 2D collider (Box2D body, XY plane): authored-shape outline with
         * the same per-shape palette as the 3D primitives above. */
        if (jce_scene_has_collider2d(scene, e)) {
            JceCollider2DComponent *c2 = jce_scene_get_collider2d(scene, e);
            if (c2)
                draw_collider2d_outline(t, c2, col_box, col_sphere,
                                        col_capsule);
        }
        /* Compound (cooked V-HACD/trimesh) colliders: draw the real fitted
         * hulls/triangles so the overlay reflects EVERY collidable object, not
         * just box/sphere/capsule primitives. Cook is cached per model path. */
        if (jce_scene_has_compound_collider(scene, e)) {
            JceCompoundColliderComponent *cpc =
                jce_scene_get_compound_collider(scene, e);
            if (cpc) {
                jce_vec3 dd = jce_v3_sub(t->position, cam_pos);
                float dist2 = dd.x * dd.x + dd.y * dd.y + dd.z * dd.z;
                int detailed = (dist2 < detail_dist2 && detail_budget > 0) ? 1 : 0;
                if (detailed) detail_budget--;
                jce_gizmo_compound_collider_draw_from_component(
                    scene, e, cpc, col_box, detailed);
            }
        }
        /* Mesh colliders: same fitted-wireframe overlay through the synthetic
         * single-shape forwarder, same near/far detail LOD as compound. */
        if (jce_scene_has_mesh_collider(scene, e)) {
            JceMeshColliderComponent *msc = jce_scene_get_mesh_collider(scene, e);
            if (msc) {
                jce_vec3 dd = jce_v3_sub(t->position, cam_pos);
                float dist2 = dd.x * dd.x + dd.y * dd.y + dd.z * dd.z;
                int detailed = (dist2 < detail_dist2 && detail_budget > 0) ? 1 : 0;
                if (detailed) detail_budget--;
                jce_gizmo_mesh_collider_draw_from_component(
                    scene, e, msc, col_box, detailed);
            }
        }
    }

    jce_debug_draw_flush(scene_view_id(), s_sr.renderer);

    /* P3-C.5 — flush Bullet's debug-draw (wireframes / AABBs / contacts)
     * for the live Play world.  The line sink + flag mask are installed
     * once at editor startup (see jce_panel_physics_debugger.cpp). */
    JcePhysicsWorld *pw = jce_editor_play_get_physics_world();
    if (pw && jce_physics_debug_get_flags() != 0) {
        jce_physics_debug_flush(pw);
        jce_debug_draw_flush(scene_view_id(), s_sr.renderer);
    }
}

/* ── Joint gizmos (P3-C.6) ────────────────────────────────────────── */

/* Draw Unity-parity joint visualisation (anchors, A↔B line, axis,
 * type-specific limit geometry) for every selected entity that owns a
 * JceConstraintComponent.  Selection-driven by design — gizmos for all
 * joints would clutter Scene View. */
void draw_joint_gizmos(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    if (!sel || sel_count <= 0) return;

    bool drew_any = false;
    for (int i = 0; i < sel_count; ++i) {
        uint32_t id = sel[i];
        if (id == 0 || !jce_state_entity_exists(id)) continue;
        if (!jce_state_entity_enabled(id)) continue;

        JceEntity e = (JceEntity)id;
        if (!jce_scene_has_constraint(scene, e)) continue;

        JceConstraintComponent *c = jce_scene_get_constraint(scene, e);
        if (!c) continue;

        jce_gizmo_joint_draw_from_component(scene, e, c);
        drew_any = true;
    }

    if (drew_any) jce_debug_draw_flush(scene_view_id(), s_sr.renderer);
}

/* Draw cloth wireframe for every selected entity that owns a
 * JceClothComponent.  Selection-driven for the same reason as joints. */
void draw_cloth_gizmos(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    if (!sel || sel_count <= 0) return;

    bool drew_any = false;
    for (int i = 0; i < sel_count; ++i) {
        uint32_t id = sel[i];
        if (id == 0 || !jce_state_entity_exists(id)) continue;
        if (!jce_state_entity_enabled(id)) continue;

        JceEntity e = (JceEntity)id;
        if (!jce_scene_has_cloth(scene, e)) continue;

        JceClothComponent *cl = jce_scene_get_cloth(scene, e);
        if (!cl) continue;

        jce_gizmo_cloth_draw_from_component(cl);
        drew_any = true;
    }

    if (drew_any) jce_debug_draw_flush(scene_view_id(), s_sr.renderer);
}

/* ── Navmesh overlay (P1-navmesh-chain) ──────────────────────────── */

/* Edge sink for jce_recast_debug_edges: boundary edges draw bright,
 * inner edges translucent; both lifted slightly above the surface so
 * the lines never z-fight the walkable geometry. */
static void navmesh_overlay_emit_edge(void *user, const float a[3],
                                      const float b[3], bool boundary)
{
    (void)user;
    const float lift = 0.05f;
    uint32_t abgr = boundary ? 0xFF00A5FFu : 0x6600A5FFu; /* orange-ish */
    jce_debug_draw_line(jce_v3(a[0], a[1] + lift, a[2]),
                        jce_v3(b[0], b[1] + lift, b[2]), abgr);
}

/* Draw the baked navmesh (the "<scene>.navmesh.bin" sibling of the current
 * scene — the same file Play mode hands the runtime) as a wireframe overlay.
 * Gated on JCE_SHOW_FLAG_NAVMESH; the mesh is lazily loaded and cached per
 * (path, mtime), so a re-bake from the NavMesh panel is picked up on the
 * next draw and a missing/broken file is not retried every frame. */
void draw_navmesh_overlay(void)
{
    static JceRecastNavMesh *s_nm = NULL;
    static char    s_nm_path[1024] = { 0 };
    static int64_t s_nm_mtime      = 0;

    if (!jce_state_show_flag(JCE_SHOW_FLAG_NAVMESH)) return;

    /* Derive "<scene path minus extension>.navmesh.bin" (same rule as
     * jce_editor_play.cpp hands the runtime). */
    const char *scene_path = jce_state_get_current_scene_path();
    if (!scene_path || !scene_path[0]) return;
    std::string np = scene_path;
    const char *sfxs[] = { ".scene.json", ".json" };
    for (const char *sfx : sfxs) {
        size_t sl = strlen(sfx);
        if (np.size() >= sl && np.compare(np.size() - sl, sl, sfx) == 0) {
            np.erase(np.size() - sl);
            break;
        }
    }
    np += ".navmesh.bin";

    int64_t mtime = 0;
    if (!jce_fs_host_get_mtime(np.c_str(), &mtime)) mtime = 0;

    /* (Re)load only when the path or mtime changed; a failed load leaves
     * s_nm NULL for that (path, mtime) pair, so it is not retried per frame. */
    if (np != s_nm_path || mtime != s_nm_mtime) {
        if (s_nm) { jce_recast_destroy(s_nm); s_nm = NULL; }
        snprintf(s_nm_path, sizeof(s_nm_path), "%s", np.c_str());
        s_nm_mtime = mtime;
        if (mtime != 0)
            s_nm = jce_recast_load_file(np.c_str());
    }
    if (!s_nm) return;

    if (jce_recast_debug_edges(s_nm, navmesh_overlay_emit_edge, NULL) > 0)
        jce_debug_draw_flush(scene_view_id(), s_sr.renderer);
}

/* ── World-streaming overlay (P2-world-streaming) ─────────────────── */

/* Horizontal (XZ-plane) debug circle helper for the streaming overlay. */
static void streaming_overlay_circle(float cx, float cy, float cz,
                                     float radius, uint32_t abgr)
{
    if (radius <= 0.0f) return;
    const int SEGS = 48;
    float px = cx + radius, pz = cz;
    for (int i = 1; i <= SEGS; ++i) {
        float a  = (float)i * (2.0f * JCE_PI / (float)SEGS);
        float nx = cx + cosf(a) * radius;
        float nz = cz + sinf(a) * radius;
        jce_debug_draw_line(jce_v3(px, cy, pz), jce_v3(nx, cy, nz), abgr);
        px = nx; pz = nz;
    }
}

/* Draw the scene's authored streaming chunks (horizontal circle at each
 * chunk's center/radius) plus the load/unload rings around the EDITOR
 * camera.  Gated on JCE_SHOW_FLAG_STREAMING.  Colors (ABGR): when the
 * preview streamer is live, each chunk reflects its live state — loaded
 * green / loading yellow / unloaded gray (0x66-dimmed); without preview
 * everything draws as dimmed gray authoring guides. */
void draw_streaming_overlay(void)
{
    if (!jce_state_show_flag(JCE_SHOW_FLAG_STREAMING)) return;

    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    const JceSceneStreamingSettings *st =
        jce_scene_get_streaming_settings(scene);
    if (!st) return;

    JceWorldStreamer *ws = jce_state_get_streaming_preview()
                         ? jce_editor_get_world_streamer() : NULL;

    bool drew_any = false;

    for (uint32_t i = 0; i < st->chunk_count; ++i) {
        const JceSceneStreamChunk *c = &st->chunks[i];
        uint32_t abgr = 0x66AAAAAAu;               /* unloaded: dim gray  */
        if (ws) {
            switch (jce_world_streamer_chunk_state(ws, c->id)) {
            case JCE_CHUNK_LOADED:    abgr = 0xFF00FF00u; break; /* green  */
            case JCE_CHUNK_LOADING:
            case JCE_CHUNK_UNLOADING: abgr = 0xFF00FFFFu; break; /* yellow */
            case JCE_CHUNK_UNLOADED:
            default:                  abgr = 0x66AAAAAAu; break; /* gray   */
            }
        }
        streaming_overlay_circle(c->center[0], c->center[1], c->center[2],
                                 c->radius > 0.0f ? c->radius : 1.0f, abgr);
        drew_any = true;
    }

    /* Load / unload radii around the editor camera — shows the authored
     * ring pair the distance test uses (cyan = load, red = unload). */
    if (s_sr.camera) {
        jce_vec3 eye = jce_camera_get_position(s_sr.camera);
        streaming_overlay_circle(eye.x, eye.y, eye.z, st->load_radius,
                                 0xFFFFFF00u);     /* cyan */
        streaming_overlay_circle(eye.x, eye.y, eye.z, st->unload_radius,
                                 0xFF0000FFu);     /* red  */
        drew_any = true;
    }

    if (drew_any) jce_debug_draw_flush(scene_view_id(), s_sr.renderer);
}

/* Draw per-object compound-collider wireframe for every selected entity
 * that owns a JceCompoundColliderComponent.  Selection-driven; the cooked
 * wireframe is cached inside the gizmo (model path + settings keyed). */
void draw_compound_collider_gizmos(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    if (!sel || sel_count <= 0) return;

    bool drew_any = false;
    for (int i = 0; i < sel_count; ++i) {
        uint32_t id = sel[i];
        if (id == 0 || !jce_state_entity_exists(id)) continue;
        if (!jce_state_entity_enabled(id)) continue;

        JceEntity e = (JceEntity)id;

        /* Selected: fitted (detailed=1) wireframe in the selection colour
         * (orange). Only ONE prop, so the trimesh line load is bounded. */
        if (jce_scene_has_compound_collider(scene, e)) {
            JceCompoundColliderComponent *cc =
                jce_scene_get_compound_collider(scene, e);
            if (cc) {
                jce_gizmo_compound_collider_draw_from_component(
                    scene, e, cc, 0xFF00BFFFu, 1);
                drew_any = true;
            }
        }
        if (jce_scene_has_mesh_collider(scene, e)) {
            JceMeshColliderComponent *mc = jce_scene_get_mesh_collider(scene, e);
            if (mc) {
                jce_gizmo_mesh_collider_draw_from_component(
                    scene, e, mc, 0xFF00BFFFu, 1);
                drew_any = true;
            }
        }
    }

    if (drew_any) jce_debug_draw_flush(scene_view_id(), s_sr.renderer);
}

/* ── Ghost (drag-preview) model rendering ─────────────────────────── */

void draw_ghost_entity(void)
{
    if (!s_sr.ghost_active || s_sr.ghost_mesh_path[0] == '\0')
        return;

    JceMesh *mesh = get_cached_mesh(s_sr.ghost_mesh_path, s_sr.ghost_pos);
    if (!mesh) return;

    jce_mat4 model = jce_m4_identity();
    model.raw[3][0] = s_sr.ghost_pos[0];
    model.raw[3][1] = s_sr.ghost_pos[1];
    model.raw[3][2] = s_sr.ghost_pos[2];
    jce_set_transform(model.raw[0], 1);

    JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);
    jce_set_texture(0, uh, s_sr.white_tex, JCE_SAMPLER_INHERIT);

    uint64_t state = JCE_STATE_WRITE_RGB
                   | JCE_STATE_WRITE_A
                   | JCE_STATE_DEPTH_TEST_LESS
                   | JCE_STATE_BLEND_FUNC(JCE_BLEND_SRC_ALPHA,
                                          JCE_BLEND_INV_SRC_ALPHA)
                   | JCE_STATE_MSAA;
    jce_set_state(state, 0);

    float green_dir[4]   = { 0.0f, -1.0f, 0.0f, 0.0f };
    float green_color[4] = { 0.2f, 0.9f, 0.3f, 0.45f };
    jce_uniform_set(s_sr.u_light_dir,   green_dir,   1);
    jce_uniform_set(s_sr.u_light_color, green_color, 1);

    jce_mesh_submit_overlay(mesh, s_sr.renderer, scene_view_id());
}

/* ── Hover highlight for drag-drop onto entity ────────────────────── */

void draw_hover_highlight(void)
{
    if (s_sr.hover_entity_id == 0) return;

    uint32_t id = s_sr.hover_entity_id;
    if (!jce_state_entity_exists(id) || !jce_state_entity_enabled(id)) return;

    jce_mat4 model;
    JceMesh *mesh = NULL;
    if (!build_overlay_entity_model(id, &model, &mesh)) return;
    if (!mesh) return;

    jce_set_transform(model.raw[0], 1);

    JceUniformHandle uh = jce_renderer_get_tex_uniform(s_sr.renderer);
    jce_set_texture(0, uh, s_sr.white_tex, JCE_SAMPLER_INHERIT);

    uint64_t state = JCE_STATE_WRITE_RGB
                   | JCE_STATE_DEPTH_TEST_LEQUAL
                   | JCE_STATE_BLEND_FUNC(JCE_BLEND_ONE,
                                          JCE_BLEND_ONE)
                   | JCE_STATE_MSAA;
    jce_set_state(state, 0);

    float hover_dir[4]   = { 0.0f, -1.0f, 0.0f, 0.0f };
    float hover_color[4] = { 0.28f, 0.28f, 0.34f, 1.0f };
    jce_uniform_set(s_sr.u_light_dir,   hover_dir,   1);
    jce_uniform_set(s_sr.u_light_color, hover_color, 1);

    jce_mesh_submit_overlay(mesh, s_sr.renderer, scene_view_id());

    /* Restore default lighting so subsequent draws aren't tinted. */
    JceDirLight sun = jce_dir_light_default();
    jce_lighting_apply(s_sr.renderer, &sun);
}
