/*
 * jce_anim_rigging.c  Constraint solvers.
 *
 * Pure math; no ozz / bgfx coupling.  Each solver mutates the
 * caller's pose buffer in place, with `weight` blending between
 * the original pose and the solved one (lerp on position, slerp on
 * rotation).
 */

#include <jce/middleware/animation/jce_anim_rigging.h>

#include <math.h>
#include <string.h>

/* ── Vec / quat helpers ─────────────────────────────────────── */

static void v3_sub(const float a[3], const float b[3], float o[3])
{ o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }

static float v3_dot(const float a[3], const float b[3])
{ return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

static float v3_len(const float a[3])
{ return sqrtf(v3_dot(a, a)); }

static void v3_norm(float v[3])
{
    float L = v3_len(v);
    if (L > 1e-7f) { v[0]/=L; v[1]/=L; v[2]/=L; }
}

static void v3_cross(const float a[3], const float b[3], float o[3])
{
    o[0] = a[1]*b[2] - a[2]*b[1];
    o[1] = a[2]*b[0] - a[0]*b[2];
    o[2] = a[0]*b[1] - a[1]*b[0];
}

static void v3_lerp(const float a[3], const float b[3], float t, float o[3])
{
    o[0] = a[0] + (b[0]-a[0]) * t;
    o[1] = a[1] + (b[1]-a[1]) * t;
    o[2] = a[2] + (b[2]-a[2]) * t;
}

/* Quaternion (xyzw). */
static void q_identity(float q[4]) { q[0]=q[1]=q[2]=0; q[3]=1; }

static void q_mul(const float a[4], const float b[4], float o[4])
{
    float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    float y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    float w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    o[0]=x; o[1]=y; o[2]=z; o[3]=w;
}

static void q_normalize(float q[4])
{
    float L = sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    if (L > 1e-7f) { q[0]/=L; q[1]/=L; q[2]/=L; q[3]/=L; }
}

static void q_from_axis_angle(const float axis[3], float angle, float q[4])
{
    float h = angle * 0.5f;
    float s = sinf(h);
    q[0] = axis[0]*s; q[1] = axis[1]*s; q[2] = axis[2]*s; q[3] = cosf(h);
}

/* Shortest-arc rotation from `from` (unit) to `to` (unit). */
static void q_from_to(const float from[3], const float to[3], float q[4])
{
    float d = v3_dot(from, to);
    if (d > 0.9999f) { q_identity(q); return; }
    if (d < -0.9999f) {
        /* 180° — pick any axis orthogonal to `from`. */
        float axis[3] = { 0, 1, 0 };
        float c[3]; v3_cross(from, axis, c);
        if (v3_len(c) < 1e-4f) { axis[0]=1; axis[1]=0; axis[2]=0; v3_cross(from,axis,c); }
        v3_norm(c);
        q_from_axis_angle(c, 3.14159265f, q);
        return;
    }
    float c[3]; v3_cross(from, to, c);
    q[0] = c[0]; q[1] = c[1]; q[2] = c[2];
    q[3] = sqrtf((1+d)*0.5f) + 0.5f / sqrtf((1+d)*0.5f) * 0; /* placeholder */
    /* Correct half-angle form. */
    float w = sqrtf((1.0f + d) * 0.5f);
    float s = 1.0f / (2.0f * w);
    q[0] = c[0]*s; q[1] = c[1]*s; q[2] = c[2]*s; q[3] = w;
    q_normalize(q);
}

/* Spherical lerp. */
static void q_slerp(const float a[4], const float b[4], float t, float o[4])
{
    float d = a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];
    float bb[4] = { b[0], b[1], b[2], b[3] };
    if (d < 0) { bb[0]=-bb[0]; bb[1]=-bb[1]; bb[2]=-bb[2]; bb[3]=-bb[3]; d = -d; }
    if (d > 0.9995f) {
        /* Linear lerp. */
        for (int i = 0; i < 4; ++i) o[i] = a[i] + (bb[i]-a[i])*t;
        q_normalize(o);
        return;
    }
    float th = acosf(d);
    float s  = sinf(th);
    float ra = sinf((1-t)*th) / s;
    float rb = sinf(t*th) / s;
    for (int i = 0; i < 4; ++i) o[i] = a[i]*ra + bb[i]*rb;
}

/* Apply quaternion to vector: v' = q * v * conj(q). */
static void q_rotate(const float q[4], const float v[3], float o[3])
{
    float qv[3] = { q[0], q[1], q[2] };
    float t[3];  v3_cross(qv, v, t); t[0]*=2; t[1]*=2; t[2]*=2;
    float qvt[3]; v3_cross(qv, t, qvt);
    o[0] = v[0] + q[3]*t[0] + qvt[0];
    o[1] = v[1] + q[3]*t[1] + qvt[1];
    o[2] = v[2] + q[3]*t[2] + qvt[2];
}

/* ── Two-bone IK ────────────────────────────────────────────── */

void jce_rig_solve_two_bone_ik(JceRigPose *poses, uint32_t n,
                                 const JceRigTwoBoneIK *c)
{
    if (!poses || !c || c->weight <= 0.0f) return;
    if ((uint32_t)c->root_joint >= n || (uint32_t)c->mid_joint >= n ||
        (uint32_t)c->tip_joint  >= n) return;

    float p_root[3] = { poses[c->root_joint].position[0],
                         poses[c->root_joint].position[1],
                         poses[c->root_joint].position[2] };
    float p_mid [3] = { poses[c->mid_joint ].position[0],
                         poses[c->mid_joint ].position[1],
                         poses[c->mid_joint ].position[2] };
    float p_tip [3] = { poses[c->tip_joint ].position[0],
                         poses[c->tip_joint ].position[1],
                         poses[c->tip_joint ].position[2] };

    float upper[3]; v3_sub(p_mid, p_root, upper);
    float lower[3]; v3_sub(p_tip, p_mid,  lower);
    float to_target[3]; v3_sub(c->target_pos, p_root, to_target);

    float L_u = v3_len(upper);
    float L_l = v3_len(lower);
    float L_t = v3_len(to_target);
    if (L_u < 1e-5f || L_l < 1e-5f) return;
    /* Clamp target distance to reachable range. */
    float max_reach = L_u + L_l - 1e-3f;
    float min_reach = fabsf(L_u - L_l) + 1e-3f;
    if (L_t > max_reach) L_t = max_reach;
    if (L_t < min_reach) L_t = min_reach;

    /* Law of cosines for the new mid-angle. */
    float cos_mid = (L_u*L_u + L_l*L_l - L_t*L_t) / (2.0f * L_u * L_l);
    if (cos_mid >  1.0f) cos_mid =  1.0f;
    if (cos_mid < -1.0f) cos_mid = -1.0f;
    float mid_angle = 3.14159265f - acosf(cos_mid); /* interior angle */

    /* Pole vector — pick from pole_joint, else use upper × dir. */
    float pole_dir[3];
    if (c->pole_joint >= 0 && (uint32_t)c->pole_joint < n) {
        v3_sub(poses[c->pole_joint].position, p_root, pole_dir);
    } else {
        float to_t_n[3] = { to_target[0], to_target[1], to_target[2] };
        v3_norm(to_t_n);
        float up_n[3] = { upper[0], upper[1], upper[2] }; v3_norm(up_n);
        v3_cross(up_n, to_t_n, pole_dir);
        if (v3_len(pole_dir) < 1e-3f) { pole_dir[0]=0; pole_dir[1]=1; pole_dir[2]=0; }
    }

    /* Build the rotation plane.  Solve mid bend then root rotation. */
    float to_t_dir[3] = { to_target[0], to_target[1], to_target[2] };
    v3_norm(to_t_dir);
    float bend_axis[3]; v3_cross(to_t_dir, pole_dir, bend_axis); v3_norm(bend_axis);
    if (v3_len(bend_axis) < 1e-4f) {
        bend_axis[0] = 0; bend_axis[1] = 0; bend_axis[2] = 1;
    }

    /* Compute desired upper direction = rotate to_t_dir by half the
     * compensating mid-angle around bend_axis. */
    float root_angle = acosf(
        ((L_u*L_u + L_t*L_t - L_l*L_l) / (2.0f * L_u * L_t)));
    if (root_angle != root_angle) root_angle = 0.0f; /* NaN guard */

    /* New upper direction = to_t_dir rotated by `root_angle` around bend_axis. */
    float qr[4]; q_from_axis_angle(bend_axis, root_angle, qr);
    float new_upper[3]; q_rotate(qr, to_t_dir, new_upper);
    /* Resulting world positions. */
    float new_mid[3] = { p_root[0] + new_upper[0]*L_u,
                          p_root[1] + new_upper[1]*L_u,
                          p_root[2] + new_upper[2]*L_u };
    float to_new_tip[3]; v3_sub(c->target_pos, new_mid, to_new_tip);

    /* Blend. */
    float blended_mid[3], blended_tip[3];
    v3_lerp(p_mid, new_mid,        c->weight, blended_mid);
    v3_lerp(p_tip, c->target_pos,  c->weight, blended_tip);
    memcpy(poses[c->mid_joint].position, blended_mid, 12);
    memcpy(poses[c->tip_joint].position, blended_tip, 12);

    if (c->match_target_rotation) {
        q_slerp(poses[c->tip_joint].orientation, c->target_rot,
                 c->weight, poses[c->tip_joint].orientation);
    }
    /* mid_angle is the desired interior bend; encoded via the lerped
     * tip placement above. */
    (void)mid_angle; (void)to_new_tip;
}

/* ── Multi-aim ──────────────────────────────────────────────── */

void jce_rig_solve_multi_aim(JceRigPose *poses, uint32_t n,
                               const JceRigMultiAim *c)
{
    if (!poses || !c || (uint32_t)c->source_joint >= n) return;
    if (c->target_count == 0 || c->weight <= 0.0f) return;

    /* Weighted-average direction. */
    float src[3] = { poses[c->source_joint].position[0],
                      poses[c->source_joint].position[1],
                      poses[c->source_joint].position[2] };
    float total_w = 0;
    for (uint32_t i = 0; i < c->target_count; ++i) total_w += c->target_weight[i];
    if (total_w < 1e-6f) return;
    float aim_dir[3] = { 0, 0, 0 };
    for (uint32_t i = 0; i < c->target_count; ++i) {
        float w = c->target_weight[i] / total_w;
        float d[3]; v3_sub(c->target_pos[i], src, d); v3_norm(d);
        aim_dir[0] += d[0]*w; aim_dir[1] += d[1]*w; aim_dir[2] += d[2]*w;
    }
    v3_norm(aim_dir);
    /* Rotate source's axis toward aim direction. */
    float local_axis[3] = { c->source_axis[0], c->source_axis[1], c->source_axis[2] };
    v3_norm(local_axis);
    float world_axis[3];
    q_rotate(poses[c->source_joint].orientation, local_axis, world_axis);
    float q[4]; q_from_to(world_axis, aim_dir, q);
    /* Blend the rotation by weight. */
    float identity[4]; q_identity(identity);
    float blend[4]; q_slerp(identity, q, c->weight, blend);
    float result[4]; q_mul(blend, poses[c->source_joint].orientation, result);
    q_normalize(result);
    memcpy(poses[c->source_joint].orientation, result, 16);
}

/* ── Damped transform ───────────────────────────────────────── */

void jce_rig_solve_damped_transform(JceRigPose *poses, uint32_t n,
                                      const JceRigDampedTransform *c,
                                      float dt)
{
    if (!poses || !c || (uint32_t)c->joint >= n) return;
    float k = 1.0f;
    if (c->damping_time > 0.0f && dt > 0.0f) {
        k = 1.0f - expf(-0.6931472f * dt / c->damping_time);
    }
    JceRigPose *p = &poses[c->joint];
    if (c->pos_weight > 0.0f) {
        float w = c->pos_weight * k;
        p->position[0] += (c->target_pos[0] - p->position[0]) * w;
        p->position[1] += (c->target_pos[1] - p->position[1]) * w;
        p->position[2] += (c->target_pos[2] - p->position[2]) * w;
    }
    if (c->rot_weight > 0.0f) {
        float w = c->rot_weight * k;
        q_slerp(p->orientation, c->target_rot, w, p->orientation);
    }
}

/* ── Override transform ─────────────────────────────────────── */

void jce_rig_solve_override_transform(JceRigPose *poses, uint32_t n,
                                        const JceRigOverrideTransform *c)
{
    if (!poses || !c || (uint32_t)c->joint >= n) return;
    JceRigPose *p = &poses[c->joint];
    if (c->pos_weight > 0.0f) {
        v3_lerp(p->position, c->override_pos, c->pos_weight, p->position);
    }
    if (c->rot_weight > 0.0f) {
        q_slerp(p->orientation, c->override_rot, c->rot_weight, p->orientation);
    }
    (void)c->override_in_world_space; /* always treated as world for now */
}

/* ── Twist chain ────────────────────────────────────────────── */

void jce_rig_solve_twist_chain(JceRigPose *poses, uint32_t n,
                                 const JceRigTwistChain *c)
{
    if (!poses || !c || c->weight <= 0.0f) return;
    if ((uint32_t)c->root_joint >= n || (uint32_t)c->tip_joint >= n) return;

    /* Compute the twist of the tip relative to the root around
     * `twist_axis`.  Distribute fractions to each mid bone. */
    float axis[3] = { c->twist_axis[0], c->twist_axis[1], c->twist_axis[2] };
    v3_norm(axis);
    /* Project tip orientation onto axis to extract twist angle. */
    const float *qt = poses[c->tip_joint].orientation;
    /* swing-twist decomposition: project quaternion's vector part
     * onto axis. */
    float dot = qt[0]*axis[0] + qt[1]*axis[1] + qt[2]*axis[2];
    float qproj[4] = { axis[0]*dot, axis[1]*dot, axis[2]*dot, qt[3] };
    q_normalize(qproj);
    float twist_angle = 2.0f * atan2f(sqrtf(qproj[0]*qproj[0]+qproj[1]*qproj[1]+qproj[2]*qproj[2]),
                                       qproj[3]);
    if (twist_angle > 3.14159265f) twist_angle -= 6.28318530f;

    for (uint32_t i = 0; i < c->mid_count; ++i) {
        int j = c->mid_joints[i];
        if (j < 0 || (uint32_t)j >= n) continue;
        float share = twist_angle * c->mid_fractions[i] * c->weight;
        float q[4]; q_from_axis_angle(axis, share, q);
        float r[4]; q_mul(q, poses[j].orientation, r);
        q_normalize(r);
        memcpy(poses[j].orientation, r, 16);
    }
}
