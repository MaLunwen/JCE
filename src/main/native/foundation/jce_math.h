/*
 * jce_math.h  Lightweight C99 math utilities for JCE.
 *
 * Provides vec3, vec4, mat4, quat types and common operations.
 * All functions are static inline for zero-overhead abstraction.
 * Column-major matrices (matches bgfx / OpenGL convention).
 */

#ifndef JCE_MATH_H
#define JCE_MATH_H

#include <math.h>
#include <string.h>
#include <stdbool.h>

/* -- Constants ------------------------------------------------------ */

#define JCE_PI       3.14159265358979323846f
#define JCE_DEG2RAD  (JCE_PI / 180.0f)
#define JCE_RAD2DEG  (180.0f / JCE_PI)

/* -- Types ---------------------------------------------------------- */

typedef struct { float x, y; }       jce_vec2;
typedef struct { float x, y, z; }    jce_vec3;
typedef struct { float x, y, z, w; } jce_vec4;
typedef struct { float x, y, z, w; } jce_quat;

/* Column-major 44 matrix: m[col][row] stored as flat m[16]. */
typedef struct { float m[16]; } jce_mat4;

/* -- Vec3 ----------------------------------------------------------- */

static inline jce_vec3 jce_v3(float x, float y, float z)
{
    return (jce_vec3){ x, y, z };
}

static inline jce_vec3 jce_v3_add(jce_vec3 a, jce_vec3 b)
{
    return (jce_vec3){ a.x + b.x, a.y + b.y, a.z + b.z };
}

static inline jce_vec3 jce_v3_sub(jce_vec3 a, jce_vec3 b)
{
    return (jce_vec3){ a.x - b.x, a.y - b.y, a.z - b.z };
}

static inline jce_vec3 jce_v3_scale(jce_vec3 v, float s)
{
    return (jce_vec3){ v.x * s, v.y * s, v.z * s };
}

static inline float jce_v3_dot(jce_vec3 a, jce_vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline jce_vec3 jce_v3_cross(jce_vec3 a, jce_vec3 b)
{
    return (jce_vec3){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static inline float jce_v3_len(jce_vec3 v)
{
    return sqrtf(jce_v3_dot(v, v));
}

static inline jce_vec3 jce_v3_normalize(jce_vec3 v)
{
    float len = jce_v3_len(v);
    if (len < 1e-8f) return (jce_vec3){ 0, 0, 0 };
    return jce_v3_scale(v, 1.0f / len);
}

static inline jce_vec3 jce_v3_negate(jce_vec3 v)
{
    return (jce_vec3){ -v.x, -v.y, -v.z };
}

/* -- Mat4 ----------------------------------------------------------- */

/*
 * Column-major layout (same as bgfx/OpenGL):
 *   m[0..3]   = column 0
 *   m[4..7]   = column 1
 *   m[8..11]  = column 2
 *   m[12..15] = column 3
 *
 * Index: m[col*4 + row]
 */

static inline jce_mat4 jce_m4_identity(void)
{
    jce_mat4 out;
    memset(&out, 0, sizeof(out));
    out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.0f;
    return out;
}

static inline jce_mat4 jce_m4_multiply(const jce_mat4 *a, const jce_mat4 *b)
{
    jce_mat4 out;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++)
                sum += a->m[k * 4 + row] * b->m[col * 4 + k];
            out.m[col * 4 + row] = sum;
        }
    }
    return out;
}

static inline jce_mat4 jce_m4_ortho(float left, float right,
                                     float bottom, float top,
                                     float near_val, float far_val,
                                     bool homogeneous_ndc)
{
    jce_mat4 out;
    memset(&out, 0, sizeof(out));
    out.m[ 0] = 2.0f / (right - left);
    out.m[ 5] = 2.0f / (top - bottom);
    out.m[10] = (homogeneous_ndc ? 2.0f : 1.0f) / (far_val - near_val);
    out.m[12] = (left + right) / (left - right);
    out.m[13] = (top + bottom) / (bottom - top);
    out.m[14] = homogeneous_ndc
              ? (near_val + far_val) / (near_val - far_val)
              : near_val / (near_val - far_val);
    out.m[15] = 1.0f;
    return out;
}

static inline jce_mat4 jce_m4_perspective(float fov_y_rad, float aspect,
                                           float near_val, float far_val,
                                           bool homogeneous_ndc)
{
    float t = tanf(fov_y_rad * 0.5f);
    jce_mat4 out;
    memset(&out, 0, sizeof(out));
    out.m[ 0] = 1.0f / (aspect * t);
    out.m[ 5] = 1.0f / t;
    if (homogeneous_ndc) {
        out.m[10] = -(far_val + near_val) / (far_val - near_val);
        out.m[14] = -(2.0f * far_val * near_val) / (far_val - near_val);
    } else {
        out.m[10] = -far_val / (far_val - near_val);
        out.m[14] = -(far_val * near_val) / (far_val - near_val);
    }
    out.m[11] = -1.0f;
    return out;
}

static inline jce_mat4 jce_m4_look_at(jce_vec3 eye, jce_vec3 target,
                                       jce_vec3 up)
{
    jce_vec3 f = jce_v3_normalize(jce_v3_sub(target, eye));
    jce_vec3 s = jce_v3_normalize(jce_v3_cross(f, up));
    jce_vec3 u = jce_v3_cross(s, f);

    jce_mat4 out = jce_m4_identity();
    out.m[ 0] =  s.x;    out.m[ 4] =  s.y;    out.m[ 8] =  s.z;
    out.m[ 1] =  u.x;    out.m[ 5] =  u.y;    out.m[ 9] =  u.z;
    out.m[ 2] = -f.x;    out.m[ 6] = -f.y;    out.m[10] = -f.z;
    out.m[12] = -jce_v3_dot(s, eye);
    out.m[13] = -jce_v3_dot(u, eye);
    out.m[14] =  jce_v3_dot(f, eye);
    return out;
}

static inline jce_mat4 jce_m4_translate(jce_vec3 t)
{
    jce_mat4 out = jce_m4_identity();
    out.m[12] = t.x;
    out.m[13] = t.y;
    out.m[14] = t.z;
    return out;
}

static inline jce_mat4 jce_m4_scale(jce_vec3 s)
{
    jce_mat4 out;
    memset(&out, 0, sizeof(out));
    out.m[ 0] = s.x;
    out.m[ 5] = s.y;
    out.m[10] = s.z;
    out.m[15] = 1.0f;
    return out;
}

/* -- Quaternion ----------------------------------------------------- */

static inline jce_quat jce_q_identity(void)
{
    return (jce_quat){ 0, 0, 0, 1 };
}

static inline jce_quat jce_q_from_axis_angle(jce_vec3 axis, float angle_rad)
{
    float half = angle_rad * 0.5f;
    float s = sinf(half);
    jce_vec3 n = jce_v3_normalize(axis);
    return (jce_quat){ n.x * s, n.y * s, n.z * s, cosf(half) };
}

static inline jce_quat jce_q_multiply(jce_quat a, jce_quat b)
{
    return (jce_quat){
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    };
}

static inline jce_quat jce_q_normalize(jce_quat q)
{
    float len = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-8f) return jce_q_identity();
    float inv = 1.0f / len;
    return (jce_quat){ q.x * inv, q.y * inv, q.z * inv, q.w * inv };
}

static inline jce_mat4 jce_q_to_mat4(jce_quat q)
{
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    jce_mat4 out = jce_m4_identity();
    out.m[ 0] = 1 - 2 * (yy + zz);
    out.m[ 1] =     2 * (xy + wz);
    out.m[ 2] =     2 * (xz - wy);
    out.m[ 4] =     2 * (xy - wz);
    out.m[ 5] = 1 - 2 * (xx + zz);
    out.m[ 6] =     2 * (yz + wx);
    out.m[ 8] =     2 * (xz + wy);
    out.m[ 9] =     2 * (yz - wx);
    out.m[10] = 1 - 2 * (xx + yy);
    return out;
}

/* Euler angles (radians)  quaternion. Order: Y (yaw)  X (pitch)  Z (roll). */
static inline jce_quat jce_q_from_euler(float pitch, float yaw, float roll)
{
    jce_quat qy = jce_q_from_axis_angle(jce_v3(0, 1, 0), yaw);
    jce_quat qx = jce_q_from_axis_angle(jce_v3(1, 0, 0), pitch);
    jce_quat qz = jce_q_from_axis_angle(jce_v3(0, 0, 1), roll);
    return jce_q_multiply(jce_q_multiply(qy, qx), qz);
}

#endif /* JCE_MATH_H */
