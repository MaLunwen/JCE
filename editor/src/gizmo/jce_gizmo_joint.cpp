/*
 * jce_gizmo_joint.cpp  P3-C.6  Joint-gizmo Scene View overlay.
 *
 * Visual contract (Unity parity):
 *   - Anchor A / anchor B  : small yellow wire-spheres
 *   - Connection line A↔B  : cyan
 *   - Primary axis (1m)    : axis-aligned colour (X=red, Y=green, Z=blue)
 *   - Hinge limit          : orange arc segment in the plane perpendicular
 *                            to the axis, spanning [low,high]
 *   - Slider limit         : orange segment along the axis, [low,high]
 *   - 6DOF                 : orange wire box for linear bounds, plus three
 *                            small arcs for angular bounds (one per axis)
 *
 * All draws go through jce_debug_draw_* — no new render path.  This TU
 * lives in editor/src/gizmo per the gizmo subsystem's house rules:
 * scene-view-only, math via jce_math, no SDL/bgfx direct includes.
 */

#include "jce_gizmo_joint.h"

extern "C" {
#include <jce/renderer/jce_debug_draw.h>
#include <jce/middleware/scene/jce_scene.h>
}

#include <math.h>
#include <string.h>
#include <stdint.h>

namespace {

/* Colour palette (0xAABBGGRR). */
constexpr uint32_t COL_ANCHOR  = 0xFF00FFFFu;  /* yellow */
constexpr uint32_t COL_LINE_AB = 0xFFFFFF00u;  /* cyan   */
constexpr uint32_t COL_AXIS_X  = 0xFF0000FFu;  /* red    */
constexpr uint32_t COL_AXIS_Y  = 0xFF00FF00u;  /* green  */
constexpr uint32_t COL_AXIS_Z  = 0xFFFF0000u;  /* blue   */
constexpr uint32_t COL_LIMIT   = 0xFF0080FFu;  /* orange */

constexpr float ANCHOR_RADIUS = 0.06f;
constexpr float AXIS_LENGTH   = 1.0f;
constexpr float ARC_RADIUS    = 0.5f;
constexpr int   ARC_SAMPLES   = 24;

inline uint32_t axis_colour_for(jce_vec3 a)
{
    float ax = fabsf(a.x), ay = fabsf(a.y), az = fabsf(a.z);
    if (ax >= ay && ax >= az) return COL_AXIS_X;
    if (ay >= ax && ay >= az) return COL_AXIS_Y;
    return COL_AXIS_Z;
}

/* Build any unit vector perpendicular to `n` — used as arc reference
 * direction.  Picks the world axis least parallel to `n` to avoid
 * degenerate cross products. */
inline jce_vec3 perp_unit(jce_vec3 n)
{
    jce_vec3 helper = (fabsf(n.x) < 0.9f) ? jce_v3(1.0f, 0.0f, 0.0f)
                                          : jce_v3(0.0f, 1.0f, 0.0f);
    return jce_v3_normalize(jce_v3_cross(n, helper));
}

/* Polyline-approximate arc in the plane perpendicular to `axis`,
 * centred on `centre`, sweeping from `lo` to `hi` radians around
 * `axis`.  When hi <= lo or both are 0 we draw a full circle so the
 * user still sees the rotational plane. */
void draw_arc(jce_vec3 centre, jce_vec3 axis, jce_vec3 ref0,
              float lo, float hi, float radius, uint32_t col)
{
    if (radius <= 0.0f) return;
    float sweep = hi - lo;
    bool full_circle = sweep <= 1e-4f;
    if (full_circle) { lo = 0.0f; sweep = 6.2831853f; }

    jce_vec3 r0 = jce_v3_normalize(ref0);
    jce_vec3 prev = jce_v3(0.0f, 0.0f, 0.0f);
    for (int i = 0; i <= ARC_SAMPLES; ++i) {
        float t = lo + sweep * ((float)i / (float)ARC_SAMPLES);
        jce_quat q = jce_q_from_axis_angle(axis, t);
        jce_vec3 dir = jce_q_rotate(q, r0);
        jce_vec3 p = jce_v3_add(centre, jce_v3_scale(dir, radius));
        if (i > 0) jce_debug_draw_line(prev, p, col);
        prev = p;
    }
}

/* Convenience: draw the three axis half-lines (X/Y/Z) emanating from
 * `centre` with the given length.  Used by both 6DOF and as the
 * primary axis for ball joints. */
void draw_axis_cross(jce_vec3 centre, float length)
{
    jce_debug_draw_line(centre,
        jce_v3_add(centre, jce_v3(length, 0.0f, 0.0f)), COL_AXIS_X);
    jce_debug_draw_line(centre,
        jce_v3_add(centre, jce_v3(0.0f, length, 0.0f)), COL_AXIS_Y);
    jce_debug_draw_line(centre,
        jce_v3_add(centre, jce_v3(0.0f, 0.0f, length)), COL_AXIS_Z);
}

} /* anon */

/* ────────────────────────────────────────────────────────────────── */

extern "C" void jce_gizmo_joint_draw_from_info(const JcePhysicsJointInfo *info)
{
    if (!info || info->kind == JCE_PHYSICS_JOINT_NONE) return;

    jce_vec3 a = info->anchor_a;
    jce_vec3 b = info->anchor_b;
    jce_vec3 axis = info->axis;

    /* Common: anchors + connecting line. */
    jce_debug_draw_sphere(a, ANCHOR_RADIUS, COL_ANCHOR);
    jce_debug_draw_sphere(b, ANCHOR_RADIUS, COL_ANCHOR);
    jce_debug_draw_line(a, b, COL_LINE_AB);

    switch (info->kind) {
    case JCE_PHYSICS_JOINT_BALL: {
        /* No primary axis — show the three world axes at the anchor
         * to communicate 3-DoF rotational freedom. */
        draw_axis_cross(a, AXIS_LENGTH * 0.5f);
        break;
    }
    case JCE_PHYSICS_JOINT_HINGE: {
        uint32_t acol = axis_colour_for(axis);
        jce_debug_draw_line(a,
            jce_v3_add(a, jce_v3_scale(axis, AXIS_LENGTH)), acol);
        jce_debug_draw_line(a,
            jce_v3_sub(a, jce_v3_scale(axis, AXIS_LENGTH * 0.25f)), acol);

        /* Arc lives in the plane perpendicular to the hinge axis. */
        jce_vec3 ref = perp_unit(axis);
        draw_arc(a, axis, ref, info->limit_low, info->limit_high,
                 ARC_RADIUS, COL_LIMIT);
        break;
    }
    case JCE_PHYSICS_JOINT_SLIDER: {
        uint32_t acol = axis_colour_for(axis);
        jce_debug_draw_line(a,
            jce_v3_add(a, jce_v3_scale(axis, AXIS_LENGTH)), acol);

        /* Limit segment from low to high along the axis. */
        jce_vec3 lo_pt = jce_v3_add(a, jce_v3_scale(axis, info->limit_low));
        jce_vec3 hi_pt = jce_v3_add(a, jce_v3_scale(axis, info->limit_high));
        jce_debug_draw_line(lo_pt, hi_pt, COL_LIMIT);
        /* Tick marks. */
        jce_vec3 perp = jce_v3_scale(perp_unit(axis), 0.08f);
        jce_debug_draw_line(jce_v3_sub(lo_pt, perp),
                            jce_v3_add(lo_pt, perp), COL_LIMIT);
        jce_debug_draw_line(jce_v3_sub(hi_pt, perp),
                            jce_v3_add(hi_pt, perp), COL_LIMIT);
        break;
    }
    case JCE_PHYSICS_JOINT_6DOF: {
        /* Primary axis from frame A. */
        uint32_t acol = axis_colour_for(axis);
        jce_debug_draw_line(a,
            jce_v3_add(a, jce_v3_scale(axis, AXIS_LENGTH)), acol);

        /* Linear bounds → wire box centred on anchor A.  Limits are
         * stored in A's local frame; in absence of a per-axis basis we
         * draw axis-aligned which is good enough for an editor hint. */
        jce_vec3 lo = info->linear_lower;
        jce_vec3 hi = info->linear_upper;
        jce_vec3 centre = jce_v3(0.5f * (lo.x + hi.x) + a.x,
                                  0.5f * (lo.y + hi.y) + a.y,
                                  0.5f * (lo.z + hi.z) + a.z);
        jce_vec3 half = jce_v3(0.5f * fabsf(hi.x - lo.x),
                                0.5f * fabsf(hi.y - lo.y),
                                0.5f * fabsf(hi.z - lo.z));
        if (half.x > 1e-4f || half.y > 1e-4f || half.z > 1e-4f)
            jce_debug_draw_box(centre, half, jce_q_identity(), COL_LIMIT);

        /* One small arc per angular axis to hint at rotational range. */
        const struct { jce_vec3 axis; float lo, hi; } arcs[3] = {
            { jce_v3(1.0f, 0.0f, 0.0f),
              info->angular_lower.x, info->angular_upper.x },
            { jce_v3(0.0f, 1.0f, 0.0f),
              info->angular_lower.y, info->angular_upper.y },
            { jce_v3(0.0f, 0.0f, 1.0f),
              info->angular_lower.z, info->angular_upper.z },
        };
        for (int i = 0; i < 3; ++i) {
            jce_vec3 ref = perp_unit(arcs[i].axis);
            draw_arc(a, arcs[i].axis, ref,
                     arcs[i].lo, arcs[i].hi, ARC_RADIUS * 0.5f, COL_LIMIT);
        }
        break;
    }
    default: break;
    }
}

/* ────────────────────────────────────────────────────────────────── */

/* Edit-time helper.  Resolves world-space anchor + axis from the
 * authoring component, then re-uses jce_gizmo_joint_draw_from_info. */
extern "C" void jce_gizmo_joint_draw_from_component(JceScene *scene,
                                                    JceEntity owner,
                                                    const JceConstraintComponent *c)
{
    if (!scene || !c) return;

    JceTransform *t_a = jce_scene_get_transform(scene, owner);
    if (!t_a) return;

    /* Anchor A: pivot_a in owner local space → world. */
    jce_vec3 local_a = jce_v3(c->pivot_a[0], c->pivot_a[1], c->pivot_a[2]);
    jce_vec3 anchor_a = jce_v3_add(t_a->position,
                                    jce_q_rotate(t_a->rotation, local_a));

    /* Anchor B: if target_entity > 0 and resolvable, transform pivot_b
     * by its world TR; otherwise treat pivot_b as a world point (matches
     * the constraint backend's "world anchor" semantics). */
    jce_vec3 local_b = jce_v3(c->pivot_b[0], c->pivot_b[1], c->pivot_b[2]);
    jce_vec3 anchor_b;
    bool has_b = false;
    if (c->target_entity != 0) {
        JceEntity e_b = (JceEntity)c->target_entity;
        JceTransform *t_b = jce_scene_get_transform(scene, e_b);
        if (t_b) {
            anchor_b = jce_v3_add(t_b->position,
                                  jce_q_rotate(t_b->rotation, local_b));
            has_b = true;
        }
    }
    if (!has_b) {
        anchor_b = local_b;
    }

    jce_vec3 axis_local = jce_v3(c->axis[0], c->axis[1], c->axis[2]);
    if (jce_v3_dot(axis_local, axis_local) < 1e-6f)
        axis_local = jce_v3(0.0f, 1.0f, 0.0f);
    jce_vec3 axis_world = jce_v3_normalize(
        jce_q_rotate(t_a->rotation, axis_local));

    JcePhysicsJointInfo info;
    memset(&info, 0, sizeof(info));
    JceBodyHandle h_zero = { 0 };
    JceBodyHandle h_inv  = { UINT32_MAX };
    info.body_a   = h_zero;
    info.body_b   = has_b ? h_zero : h_inv;
    info.anchor_a = anchor_a;
    info.anchor_b = anchor_b;
    info.axis     = axis_world;
    info.limit_low  = c->lower_limit;
    info.limit_high = c->upper_limit;

    switch (c->constraint_type) {
    case 0: info.kind = JCE_PHYSICS_JOINT_BALL;   break;
    case 1: info.kind = JCE_PHYSICS_JOINT_HINGE;  break;
    case 2: info.kind = JCE_PHYSICS_JOINT_SLIDER; break;
    case 3:
        info.kind = JCE_PHYSICS_JOINT_6DOF;
        /* Component carries a single scalar limit pair; broadcast it
         * across linear axes so the 6DOF visual still has a bounding
         * box.  Angular bounds collapse to "full freedom" — full
         * circles via lo==hi==0. */
        info.linear_lower = jce_v3(c->lower_limit, c->lower_limit, c->lower_limit);
        info.linear_upper = jce_v3(c->upper_limit, c->upper_limit, c->upper_limit);
        break;
    default: info.kind = JCE_PHYSICS_JOINT_NONE;  break;
    }

    if (info.kind == JCE_PHYSICS_JOINT_NONE) return;
    jce_gizmo_joint_draw_from_info(&info);
}
