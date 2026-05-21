/*
 * jce_physics_joint_query.c  P3-C.6  Joint introspection front-end.
 *
 * Thin C99 wrapper that forwards JcePhysicsWorld + JceBodyHandle into
 * the Bullet back-end and copies the result into the public
 * JcePhysicsJointInfo struct.  Bullet internals stay confined to
 * jce_physics_bullet.cpp.
 */

#include <jce/middleware/physics/jce_physics_joint_query.h>

#include "jce_physics_internal.h"

#include <string.h>

/* Defined in jce_physics.c.  Avoids re-declaring the world layout here. */
JceBulletWorld *jce_physics_world_bullet_(const JcePhysicsWorld *world);

bool jce_physics_joint_get_info(const JcePhysicsWorld *world,
                                JceBodyHandle body,
                                JcePhysicsJointInfo *info)
{
    if (!world || !info || !jce_body_valid(body)) return false;

    JceBulletWorld *bw = jce_physics_world_bullet_(world);
    if (!bw) return false;

    JceBulletJointInfo raw;
    if (!jce_bullet_joint_get_info_for_body(bw, body.idx, &raw)) return false;

    memset(info, 0, sizeof(*info));
    info->kind          = (JcePhysicsJointKind)raw.kind;
    info->body_a.idx    = raw.body_a;
    info->body_b.idx    = raw.body_b;
    info->anchor_a      = raw.anchor_a;
    info->anchor_b      = raw.anchor_b;
    info->axis          = raw.axis;
    info->limit_low     = raw.limit_low;
    info->limit_high    = raw.limit_high;
    info->linear_lower  = raw.linear_lower;
    info->linear_upper  = raw.linear_upper;
    info->angular_lower = raw.angular_lower;
    info->angular_upper = raw.angular_upper;
    return true;
}
