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
    jce_mat4 out = GLMS_MAT4_ZERO_INIT;
    out.raw[0][0] = 2.0f / (right - left);
    out.raw[1][1] = 2.0f / (top - bottom);
    out.raw[2][2] = (homogeneous_ndc ? 2.0f : 1.0f) / (far_val - near_val);
    out.raw[3][0] = (left + right) / (left - right);
    out.raw[3][1] = (top + bottom) / (bottom - top);
    out.raw[3][2] = homogeneous_ndc
                  ? (near_val + far_val) / (near_val - far_val)
                  : near_val / (near_val - far_val);
    out.raw[3][3] = 1.0f;
    return out;
}

static inline jce_mat4 jce_m4_perspective(float fov_y_rad, float aspect,
                                           float near_val, float far_val,
                                           bool homogeneous_ndc)
{
    float t = tanf(fov_y_rad * 0.5f);
    jce_mat4 out = GLMS_MAT4_ZERO_INIT;
    out.raw[0][0] = 1.0f / (aspect * t);
    out.raw[1][1] = 1.0f / t;
    if (homogeneous_ndc) {
        out.raw[2][2] = -(far_val + near_val) / (far_val - near_val);
        out.raw[3][2] = -(2.0f * far_val * near_val) / (far_val - near_val);
    } else {
        out.raw[2][2] = -far_val / (far_val - near_val);
        out.raw[3][2] = -(far_val * near_val) / (far_val - near_val);
    }
    out.raw[2][3] = -1.0f;
    return out;
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

#ifdef __cplusplus
}
#endif

#endif /* JCE_MATH_H */
