/*
 * jce_math.h  C99 math utilities backed by cglm (SIMD-accelerated).
 *
 * Provides vec2/vec3/vec4/mat4/quat typedefs over cglm struct types
 * with thin inline wrappers for API stability.
 * Column-major matrices (matches bgfx / OpenGL convention).
 */

#ifndef JCE_MATH_H
#define JCE_MATH_H

#include <cglm/struct.h>
/* Raw-array clipspace functions needed by the struct wrappers below. */
#include <cglm/clipspace/ortho_rh_zo.h>
#include <cglm/clipspace/ortho_rh_no.h>
#include <cglm/clipspace/persp_rh_zo.h>
#include <cglm/clipspace/persp_rh_no.h>
/* Struct wrappers for both _zo and _no projection variants. */
#include <cglm/struct/clipspace/ortho_rh_zo.h>
#include <cglm/struct/clipspace/ortho_rh_no.h>
#include <cglm/struct/clipspace/persp_rh_zo.h>
#include <cglm/struct/clipspace/persp_rh_no.h>
#include <math.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Constants ------------------------------------------------------ */

#define JCE_PI       GLM_PIf
#define JCE_DEG2RAD  (GLM_PIf / 180.0f)
#define JCE_RAD2DEG  (180.0f / GLM_PIf)

/* -- Types (aliases to cglm struct types) --------------------------- */

typedef vec2s    jce_vec2;
typedef vec3s    jce_vec3;
typedef vec4s    jce_vec4;
typedef versors  jce_quat;
typedef mat4s    jce_mat4;

/* -- Accessors (mat4 flat-float interop for bgfx etc.) -------------- */

/* Pointer to 16 contiguous floats (column-major). */
#define JCE_M4_PTR(m)           ((const float *)(m).raw)
#define JCE_M4_MUT_PTR(m)       ((float *)(m).raw)

/* -- Vec2 ----------------------------------------------------------- */

static inline jce_vec2 jce_v2(float x, float y)
{
    jce_vec2 v;
    v.x = x; v.y = y;
    return v;
}

static inline float jce_v2_len(jce_vec2 v)
{
    return glms_vec2_norm(v);
}

static inline jce_vec2 jce_v2_normalize(jce_vec2 v)
{
    float len = glms_vec2_norm(v);
    if (len < 1e-8f) { jce_vec2 z; z.x = z.y = 0; return z; }
    return glms_vec2_scale(v, 1.0f / len);
}

/* -- Vec3 ----------------------------------------------------------- */

static inline jce_vec3 jce_v3(float x, float y, float z)
{
    jce_vec3 v;
    v.x = x; v.y = y; v.z = z;
    return v;
}

static inline jce_vec3 jce_v3_add(jce_vec3 a, jce_vec3 b)
{
    return glms_vec3_add(a, b);
}

static inline jce_vec3 jce_v3_sub(jce_vec3 a, jce_vec3 b)
{
    return glms_vec3_sub(a, b);
}

static inline jce_vec3 jce_v3_scale(jce_vec3 v, float s)
{
    return glms_vec3_scale(v, s);
}

static inline float jce_v3_dot(jce_vec3 a, jce_vec3 b)
{
    return glms_vec3_dot(a, b);
}

static inline jce_vec3 jce_v3_cross(jce_vec3 a, jce_vec3 b)
{
    return glms_vec3_cross(a, b);
}

static inline float jce_v3_len(jce_vec3 v)
{
    return glms_vec3_norm(v);
}

static inline jce_vec3 jce_v3_normalize(jce_vec3 v)
{
    float len = glms_vec3_norm(v);
    if (len < 1e-8f) { jce_vec3 z; z.x = z.y = z.z = 0; return z; }
    return glms_vec3_scale(v, 1.0f / len);
}

static inline jce_vec3 jce_v3_negate(jce_vec3 v)
{
    return glms_vec3_negate(v);
}

static inline jce_vec3 jce_v3_lerp(jce_vec3 a, jce_vec3 b, float t)
{
    return glms_vec3_lerp(a, b, t);
}

/* -- Mat4 ----------------------------------------------------------- */

static inline jce_mat4 jce_m4_identity(void)
{
    jce_mat4 m = GLMS_MAT4_IDENTITY_INIT;
    return m;
}

static inline jce_mat4 jce_m4_multiply(const jce_mat4 *a, const jce_mat4 *b)
{
    return glms_mat4_mul(*a, *b);
}

/*
 * Orthographic projection.
 *   homogeneous_ndc = true  → clip z ∈ [-1, 1] (OpenGL)
 *   homogeneous_ndc = false → clip z ∈ [ 0, 1] (D3D / Metal / Vulkan)
 */
static inline jce_mat4 jce_m4_ortho(float left, float right,
                                     float bottom, float top,
                                     float near_val, float far_val,
                                     bool homogeneous_ndc)
{
    return homogeneous_ndc
        ? glms_ortho_rh_no(left, right, bottom, top, near_val, far_val)
        : glms_ortho_rh_zo(left, right, bottom, top, near_val, far_val);
}

static inline jce_mat4 jce_m4_perspective(float fov_y_rad, float aspect,
                                           float near_val, float far_val,
                                           bool homogeneous_ndc)
{
    return homogeneous_ndc
        ? glms_perspective_rh_no(fov_y_rad, aspect, near_val, far_val)
        : glms_perspective_rh_zo(fov_y_rad, aspect, near_val, far_val);
}

static inline jce_mat4 jce_m4_look_at(jce_vec3 eye, jce_vec3 target,
                                       jce_vec3 up)
{
    return glms_lookat(eye, target, up);
}

static inline jce_mat4 jce_m4_translate(jce_vec3 t)
{
    return glms_translate_make(t);
}

static inline jce_mat4 jce_m4_scale(jce_vec3 s)
{
    return glms_scale_make(s);
}

static inline jce_mat4 jce_m4_transpose(const jce_mat4 *m)
{
    return glms_mat4_transpose(*m);
}

static inline jce_mat4 jce_m4_inverse(const jce_mat4 *m)
{
    return glms_mat4_inv(*m);
}

/* Compose a model matrix from translation, rotation (quat), and scale. */
static inline jce_mat4 jce_m4_from_trs(jce_vec3 t, jce_quat r, jce_vec3 s)
{
    jce_mat4 rot = glms_quat_mat4(r);
    jce_mat4 out;
    out.col[0] = glms_vec4_scale(rot.col[0], s.x);
    out.col[1] = glms_vec4_scale(rot.col[1], s.y);
    out.col[2] = glms_vec4_scale(rot.col[2], s.z);
    vec4s col3; col3.x = t.x; col3.y = t.y; col3.z = t.z; col3.w = 1.0f;
    out.col[3] = col3;
    return out;
}

/* -- Quaternion ----------------------------------------------------- */

static inline jce_quat jce_q_identity(void)
{
    jce_quat q = GLMS_QUAT_IDENTITY_INIT;
    return q;
}

static inline jce_quat jce_q_from_axis_angle(jce_vec3 axis, float angle_rad)
{
    return glms_quatv(angle_rad, jce_v3_normalize(axis));
}

static inline jce_quat jce_q_multiply(jce_quat a, jce_quat b)
{
    return glms_quat_mul(a, b);
}

static inline jce_quat jce_q_normalize(jce_quat q)
{
    return glms_quat_normalize(q);
}

/* Rotate a vec3 by a unit quaternion: v' = q * v * q^{-1}.
 * Uses the efficient cross-product form:
 *   t = 2 * cross(q.xyz, v)
 *   result = v + q.w * t + cross(q.xyz, t)                              */
static inline jce_vec3 jce_q_rotate(jce_quat q, jce_vec3 v)
{
    jce_vec3 u; u.x = q.x; u.y = q.y; u.z = q.z;
    float    w = q.w;
    jce_vec3 t = jce_v3_scale(jce_v3_cross(u, v), 2.0f);
    return jce_v3_add(jce_v3_add(v, jce_v3_scale(t, w)), jce_v3_cross(u, t));
}

static inline jce_mat4 jce_q_to_mat4(jce_quat q)
{
    return glms_quat_mat4(q);
}

/* Euler angles (radians) → quaternion.  Order: Y (yaw) → X (pitch) → Z (roll). */
static inline jce_quat jce_q_from_euler(float pitch, float yaw, float roll)
{
    jce_vec3 ax_y; ax_y.x = 0; ax_y.y = 1; ax_y.z = 0;
    jce_vec3 ax_x; ax_x.x = 1; ax_x.y = 0; ax_x.z = 0;
    jce_vec3 ax_z; ax_z.x = 0; ax_z.y = 0; ax_z.z = 1;
    jce_quat qy = glms_quatv(yaw,   ax_y);
    jce_quat qx = glms_quatv(pitch, ax_x);
    jce_quat qz = glms_quatv(roll,  ax_z);
    return glms_quat_mul(glms_quat_mul(qy, qx), qz);
}

static inline jce_quat jce_q_slerp(jce_quat a, jce_quat b, float t)
{
    return glms_quat_slerp(a, b, t);
}

/* Quaternion → Euler angles (radians).  Returns (pitch, yaw, roll) in YXZ order. */
static inline jce_vec3 jce_q_to_euler(jce_quat q)
{
    jce_vec3 e;
    float sinr_cosp = 2.0f * (q.w * q.x + q.y * q.z);
    float cosr_cosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    e.x = atan2f(sinr_cosp, cosr_cosp); /* pitch */
    float sinp = 2.0f * (q.w * q.y - q.z * q.x);
    e.y = (fabsf(sinp) >= 1.0f)
        ? copysignf((float)GLM_PI * 0.5f, sinp)
        : asinf(sinp); /* yaw */
    float siny_cosp = 2.0f * (q.w * q.z + q.x * q.y);
    float cosy_cosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    e.z = atan2f(siny_cosp, cosy_cosp); /* roll */
    return e;
}

/* Convenience: Euler degrees → quaternion (YXZ order). */
static inline jce_quat jce_euler_to_q(float pitch_deg, float yaw_deg, float roll_deg)
{
    return jce_q_from_euler(pitch_deg * JCE_DEG2RAD,
                            yaw_deg   * JCE_DEG2RAD,
                            roll_deg  * JCE_DEG2RAD);
}

/* Ray–AABB intersection test (slab method).
   Returns true if the ray hits the box; *t_min receives the entry distance (can be < 0). */
static inline bool jce_ray_aabb_intersect(jce_vec3 origin, jce_vec3 dir,
                                           jce_vec3 box_min, jce_vec3 box_max,
                                           float *t_min)
{
    float tmin = -1e30f, tmax = 1e30f;
    const float *o = &origin.x, *d = &dir.x;
    const float *bmin = &box_min.x, *bmax = &box_max.x;
    for (int i = 0; i < 3; i++) {
        if (fabsf(d[i]) < 1e-8f) {
            if (o[i] < bmin[i] || o[i] > bmax[i]) return false;
        } else {
            float inv = 1.0f / d[i];
            float t1 = (bmin[i] - o[i]) * inv;
            float t2 = (bmax[i] - o[i]) * inv;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }
    if (t_min) *t_min = tmin;
    return true;
}

/* Project a world-space point to normalised screen coordinates [0,1].
   Returns false if the point is behind the camera. */
static inline bool jce_world_to_screen(jce_vec3 world_pos,
                                        const jce_mat4 *view,
                                        const jce_mat4 *proj,
                                        float *out_x, float *out_y)
{
    jce_mat4 vp = glms_mat4_mul(*proj, *view);
    vec4s p; p.x = world_pos.x; p.y = world_pos.y; p.z = world_pos.z; p.w = 1.0f;
    vec4s clip = glms_mat4_mulv(vp, p);
    if (clip.w <= 1e-6f) return false;
    /* Use unit viewport [0,0,1,1] so glms_project returns normalised coords. */
    vec4s unit_vp; unit_vp.x = 0; unit_vp.y = 0; unit_vp.z = 1; unit_vp.w = 1;
    vec3s r = glms_project(world_pos, vp, unit_vp);
    if (out_x) *out_x = r.x;
    if (out_y) *out_y = 1.0f - r.y; /* flip Y: screen top = 0 */
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* JCE_MATH_H */
