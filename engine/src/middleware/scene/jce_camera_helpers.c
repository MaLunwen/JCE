/*
 * jce_camera_helpers.c  Scripted-camera math.
 *
 * Pure functions, no engine state.  Uses jce_math primitives — no
 * bgfx, no flecs.  Output is JceCameraFrame the caller composes into
 * a final view matrix.
 */

#include <jce/middleware/scene/jce_camera_helpers.h>

#include <math.h>

static jce_vec3 v3(float x, float y, float z)
{ return jce_v3(x, y, z); }

static jce_vec3 v3_add(jce_vec3 a, jce_vec3 b)
{ jce_vec3 r = { a.x+b.x, a.y+b.y, a.z+b.z }; return r; }
static jce_vec3 v3_sub(jce_vec3 a, jce_vec3 b)
{ jce_vec3 r = { a.x-b.x, a.y-b.y, a.z-b.z }; return r; }
static jce_vec3 v3_scale(jce_vec3 a, float s)
{ jce_vec3 r = { a.x*s, a.y*s, a.z*s }; return r; }
static jce_vec3 v3_cross(jce_vec3 a, jce_vec3 b)
{
    jce_vec3 r = { a.y*b.z - a.z*b.y,
                    a.z*b.x - a.x*b.z,
                    a.x*b.y - a.y*b.x };
    return r;
}
static float v3_dot(jce_vec3 a, jce_vec3 b)
{ return a.x*b.x + a.y*b.y + a.z*b.z; }
static jce_vec3 v3_normalize(jce_vec3 a)
{
    float lsq = v3_dot(a, a);
    if (lsq < 1e-12f) { jce_vec3 r = {0, 0, 1}; return r; }
    float inv = 1.0f / sqrtf(lsq);
    return v3_scale(a, inv);
}

JceCameraFrame jce_cam_look_at_with_offset(jce_vec3 eye, jce_vec3 target,
                                            jce_vec3 world_up,
                                            jce_vec3 offset_local)
{
    /* Compute look-at orientation first. */
    jce_vec3 fwd   = v3_normalize(v3_sub(target, eye));
    jce_vec3 right = v3_normalize(v3_cross(fwd, world_up));
    jce_vec3 up    = v3_cross(right, fwd);
    /* Apply local offset using the look-at basis. */
    jce_vec3 shift = v3_add(v3_add(v3_scale(right,  offset_local.x),
                                   v3_scale(up,     offset_local.y)),
                            v3_scale(fwd, offset_local.z));
    JceCameraFrame f;
    f.eye    = v3_add(eye, shift);
    f.target = v3_add(target, shift);
    f.up     = up;
    return f;
}

JceCameraFrame jce_cam_orbit(jce_vec3 target, float yaw, float pitch,
                              float distance, jce_vec3 world_up)
{
    if (distance <= 0.0f) distance = 1.0f;
    /* Clamp pitch slightly inside ±π/2 to avoid gimbal flips. */
    const float kPitchLimit = 1.5533f;  /* ~89° */
    if (pitch >  kPitchLimit) pitch =  kPitchLimit;
    if (pitch < -kPitchLimit) pitch = -kPitchLimit;

    float cp = cosf(pitch);
    /* Forward vector in world space (right-handed Y-up convention). */
    jce_vec3 fwd = v3(sinf(yaw) * cp, sinf(pitch), cosf(yaw) * cp);
    /* Eye is target minus forward × distance. */
    jce_vec3 eye = v3_sub(target, v3_scale(fwd, distance));

    JceCameraFrame f;
    f.eye    = eye;
    f.target = target;
    f.up     = world_up;
    return f;
}

JceCameraFrame jce_cam_dolly_along_path(const JceAnimCurve *cx,
                                         const JceAnimCurve *cy,
                                         const JceAnimCurve *cz,
                                         float t,
                                         jce_vec3 look_target,
                                         jce_vec3 world_up)
{
    JceCameraFrame f;
    f.eye.x = cx ? jce_anim_curve_evaluate(cx, t) : 0.0f;
    f.eye.y = cy ? jce_anim_curve_evaluate(cy, t) : 0.0f;
    f.eye.z = cz ? jce_anim_curve_evaluate(cz, t) : 0.0f;
    f.target = look_target;
    f.up     = world_up;
    return f;
}

JceCameraFrame jce_cam_smooth_damp(JceCameraFrame cur, JceCameraFrame tgt,
                                    float smooth_time, float dt)
{
    if (smooth_time <= 0.0001f) return tgt;
    if (dt <= 0.0f) return cur;

    /* Critically-damped spring constant: 2 / smooth_time matches
     * Unity's SmoothDamp curvature. */
    float omega = 2.0f / smooth_time;
    float x = omega * dt;
    /* 1 / (1 + x + 0.48*x² + 0.235*x³)  — fast exp approximation
     * (Game Programming Gems #4, Bobic). */
    float exp_decay = 1.0f / (1.0f + x + 0.48f*x*x + 0.235f*x*x*x);

    JceCameraFrame r;
    r.eye    = v3_add(tgt.eye,    v3_scale(v3_sub(cur.eye,    tgt.eye),    exp_decay));
    r.target = v3_add(tgt.target, v3_scale(v3_sub(cur.target, tgt.target), exp_decay));
    /* Up doesn't smooth — it's a basis vector; snap. */
    r.up     = tgt.up;
    return r;
}
