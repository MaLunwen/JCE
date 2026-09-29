/*
 * jce_math.h  Public C99 math ABI.
 *
 * The implementation is layout-compatible with the engine's internal
 * column-major math usage.  Client code can use JCE math types without
 * adding third-party include paths or depending on third-party ABI.
 */

#ifndef JCE_MATH_H
#define JCE_MATH_H

#include <jce/os/core/jce_defs.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

JCE_EXTERN_C_BEGIN

#define JCE_PI       3.14159265358979323846f
#define JCE_DEG2RAD  (JCE_PI / 180.0f)
#define JCE_RAD2DEG  (180.0f / JCE_PI)

typedef struct jce_vec2 {
    float x, y;
} jce_vec2;

typedef struct jce_vec3 {
    float x, y, z;
} jce_vec3;

typedef struct jce_vec4 {
    float x, y, z, w;
} jce_vec4;

typedef jce_vec4 jce_quat;

typedef union jce_mat4 {
    float    raw[4][4]; /* raw[column][row], column-major */
    jce_vec4 col[4];
} jce_mat4;

#define JCE_M4_PTR(m)           ((const float *)&(m).raw[0][0])
#define JCE_M4_MUT_PTR(m)       ((float *)&(m).raw[0][0])

/*
 * IEEE binary16 -> float.
 *
 * HERE, AND INLINE, BECAUSE THREE LAYERS NEEDED IT.  A READ_BACK staging copy
 * of an RGBA16F target has to be decoded on the CPU, and so does a quantised
 * network vector -- so the renderer's headless capture, the post-fx exposure
 * metering and jce_quant_f16_to_f32 all want the same twenty lines.  They had
 * begun to grow their own; "read the physical memory" reached four independent
 * implementations in this tree before anyone counted, and the copies do not
 * agree at the edges, which is where this function is hard.
 *
 * Inline in the header so it costs no link edge in either direction: the
 * metering lives in the renderer and the quantiser in middleware, and neither
 * layer may depend on the other.
 *
 * The three cases a shift-and-mask gets wrong are all handled: exp == 0
 * (zero and subnormals, which need renormalising, not just shifting),
 * exp == 0x1F (Inf and NaN, whose exponent must saturate rather than rebias),
 * and the exponent rebias between them.
 */
JCE_INLINE float jce_half_to_float(uint16_t h)
{
    uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    uint32_t exp  = ((uint32_t)h >> 10) & 0x1Fu;
    uint32_t mant = (uint32_t)h & 0x3FFu;
    uint32_t out;

    if (exp == 0u) {
        if (mant == 0u) {
            out = sign;                              /* +/- 0 */
        } else {
            int32_t e = -1;                          /* subnormal: normalise */
            do { mant <<= 1; e += 1; } while ((mant & 0x400u) == 0u);
            mant &= 0x3FFu;
            out = sign | ((uint32_t)(127 - 15 - e) << 23) | (mant << 13);
        }
    } else if (exp == 31u) {
        out = sign | 0x7F800000u | (mant << 13);     /* Inf / NaN */
    } else {
        out = sign | ((exp + 127u - 15u) << 23) | (mant << 13);
    }

    float f;
    memcpy(&f, &out, sizeof f);
    return f;
}

JCE_INLINE jce_vec2 jce_v2(float x, float y)
{
    jce_vec2 v;
    v.x = x;
    v.y = y;
    return v;
}

JCE_INLINE float jce_v2_len(jce_vec2 v)
{
    return sqrtf(v.x * v.x + v.y * v.y);
}

JCE_INLINE jce_vec2 jce_v2_normalize(jce_vec2 v)
{
    float len = jce_v2_len(v);
    if (len < 1e-8f)
        return jce_v2(0.0f, 0.0f);
    return jce_v2(v.x / len, v.y / len);
}

JCE_INLINE jce_vec3 jce_v3(float x, float y, float z)
{
    jce_vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

JCE_INLINE jce_vec4 jce_v4(float x, float y, float z, float w)
{
    jce_vec4 v;
    v.x = x;
    v.y = y;
    v.z = z;
    v.w = w;
    return v;
}

JCE_INLINE jce_vec3 jce_v3_add(jce_vec3 a, jce_vec3 b)
{
    return jce_v3(a.x + b.x, a.y + b.y, a.z + b.z);
}

JCE_INLINE jce_vec3 jce_v3_sub(jce_vec3 a, jce_vec3 b)
{
    return jce_v3(a.x - b.x, a.y - b.y, a.z - b.z);
}

JCE_INLINE jce_vec3 jce_v3_scale(jce_vec3 v, float s)
{
    return jce_v3(v.x * s, v.y * s, v.z * s);
}

JCE_INLINE float jce_v3_dot(jce_vec3 a, jce_vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

JCE_INLINE jce_vec3 jce_v3_cross(jce_vec3 a, jce_vec3 b)
{
    return jce_v3(a.y * b.z - a.z * b.y,
                  a.z * b.x - a.x * b.z,
                  a.x * b.y - a.y * b.x);
}

JCE_INLINE float jce_v3_len(jce_vec3 v)
{
    return sqrtf(jce_v3_dot(v, v));
}

JCE_INLINE jce_vec3 jce_v3_normalize(jce_vec3 v)
{
    float len = jce_v3_len(v);
    if (len < 1e-8f)
        return jce_v3(0.0f, 0.0f, 0.0f);
    return jce_v3_scale(v, 1.0f / len);
}

JCE_INLINE jce_vec3 jce_v3_negate(jce_vec3 v)
{
    return jce_v3(-v.x, -v.y, -v.z);
}

JCE_INLINE jce_vec3 jce_v3_lerp(jce_vec3 a, jce_vec3 b, float t)
{
    return jce_v3(a.x + (b.x - a.x) * t,
                  a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t);
}

JCE_INLINE jce_vec4 jce_v4_scale(jce_vec4 v, float s)
{
    return jce_v4(v.x * s, v.y * s, v.z * s, v.w * s);
}

JCE_INLINE jce_mat4 jce_m4_identity(void)
{
    jce_mat4 m = { 0 };
    m.raw[0][0] = 1.0f;
    m.raw[1][1] = 1.0f;
    m.raw[2][2] = 1.0f;
    m.raw[3][3] = 1.0f;
    return m;
}

JCE_INLINE jce_mat4 jce_m4_multiply(const jce_mat4 *a, const jce_mat4 *b)
{
    jce_mat4 out = { 0 };
    int c, r;

    for (c = 0; c < 4; c++) {
        for (r = 0; r < 4; r++) {
            out.raw[c][r] =
                a->raw[0][r] * b->raw[c][0] +
                a->raw[1][r] * b->raw[c][1] +
                a->raw[2][r] * b->raw[c][2] +
                a->raw[3][r] * b->raw[c][3];
        }
    }
    return out;
}

JCE_INLINE jce_mat4 jce_m4_ortho(float left, float right,
                                 float bottom, float top,
                                 float near_val, float far_val,
                                 bool homogeneous_ndc)
{
    float rl = right - left;
    float tb = top - bottom;
    float fn = far_val - near_val;
    jce_mat4 m = jce_m4_identity();

    m.raw[0][0] = 2.0f / rl;
    m.raw[1][1] = 2.0f / tb;
    m.raw[3][0] = -(right + left) / rl;
    m.raw[3][1] = -(top + bottom) / tb;
    if (homogeneous_ndc) {
        m.raw[2][2] = -2.0f / fn;
        m.raw[3][2] = -(far_val + near_val) / fn;
    } else {
        m.raw[2][2] = -1.0f / fn;
        m.raw[3][2] = -near_val / fn;
    }
    return m;
}

JCE_INLINE jce_mat4 jce_m4_perspective(float fov_y_rad, float aspect,
                                       float near_val, float far_val,
                                       bool homogeneous_ndc)
{
    float f = 1.0f / tanf(fov_y_rad * 0.5f);
    jce_mat4 m = { 0 };

    m.raw[0][0] = f / aspect;
    m.raw[1][1] = f;
    m.raw[2][3] = -1.0f;
    if (homogeneous_ndc) {
        m.raw[2][2] = -(far_val + near_val) / (far_val - near_val);
        m.raw[3][2] = -(2.0f * far_val * near_val) /
                      (far_val - near_val);
    } else {
        m.raw[2][2] = far_val / (near_val - far_val);
        m.raw[3][2] = -(far_val * near_val) / (far_val - near_val);
    }
    return m;
}

JCE_INLINE jce_mat4 jce_m4_look_at(jce_vec3 eye, jce_vec3 target,
                                   jce_vec3 up)
{
    jce_vec3 f = jce_v3_normalize(jce_v3_sub(target, eye));
    jce_vec3 s = jce_v3_normalize(jce_v3_cross(f, up));
    jce_vec3 u = jce_v3_cross(s, f);
    jce_mat4 m = jce_m4_identity();

    m.raw[0][0] = s.x;
    m.raw[0][1] = u.x;
    m.raw[0][2] = -f.x;
    m.raw[1][0] = s.y;
    m.raw[1][1] = u.y;
    m.raw[1][2] = -f.y;
    m.raw[2][0] = s.z;
    m.raw[2][1] = u.z;
    m.raw[2][2] = -f.z;
    m.raw[3][0] = -jce_v3_dot(s, eye);
    m.raw[3][1] = -jce_v3_dot(u, eye);
    m.raw[3][2] = jce_v3_dot(f, eye);
    return m;
}

JCE_INLINE jce_mat4 jce_m4_translate(jce_vec3 t)
{
    jce_mat4 m = jce_m4_identity();
    m.raw[3][0] = t.x;
    m.raw[3][1] = t.y;
    m.raw[3][2] = t.z;
    return m;
}

JCE_INLINE jce_mat4 jce_m4_scale(jce_vec3 s)
{
    jce_mat4 m = { 0 };
    m.raw[0][0] = s.x;
    m.raw[1][1] = s.y;
    m.raw[2][2] = s.z;
    m.raw[3][3] = 1.0f;
    return m;
}

JCE_INLINE jce_mat4 jce_m4_transpose(const jce_mat4 *m)
{
    jce_mat4 out;
    int c, r;

    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++)
            out.raw[c][r] = m->raw[r][c];
    return out;
}

JCE_INLINE jce_mat4 jce_m4_inverse(const jce_mat4 *m)
{
    const float *a = &m->raw[0][0];
    float inv[16];
    float det;
    int i;
    jce_mat4 out;

    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] -
             a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
             a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] +
             a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
             a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] -
             a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
             a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] +
              a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
              a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] +
             a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
             a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] -
             a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
             a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] +
             a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
             a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] -
              a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
              a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] -
             a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
             a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] +
             a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
             a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] -
              a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
              a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] +
              a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
              a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] +
             a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
             a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] -
             a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
             a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] +
              a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
              a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] -
              a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
              a[8] * a[1] * a[6] - a[8] * a[2] * a[5];

    det = a[0] * inv[0] + a[1] * inv[4] +
          a[2] * inv[8] + a[3] * inv[12];
    if (fabsf(det) < 1e-8f)
        return jce_m4_identity();

    det = 1.0f / det;
    for (i = 0; i < 16; i++)
        (&out.raw[0][0])[i] = inv[i] * det;
    return out;
}

JCE_INLINE jce_vec4 jce_m4_mul_v4(const jce_mat4 *m, jce_vec4 v)
{
    return jce_v4(m->raw[0][0] * v.x + m->raw[1][0] * v.y +
                  m->raw[2][0] * v.z + m->raw[3][0] * v.w,
                  m->raw[0][1] * v.x + m->raw[1][1] * v.y +
                  m->raw[2][1] * v.z + m->raw[3][1] * v.w,
                  m->raw[0][2] * v.x + m->raw[1][2] * v.y +
                  m->raw[2][2] * v.z + m->raw[3][2] * v.w,
                  m->raw[0][3] * v.x + m->raw[1][3] * v.y +
                  m->raw[2][3] * v.z + m->raw[3][3] * v.w);
}

JCE_INLINE jce_vec3 jce_m4_extract_scale(const jce_mat4 *m)
{
    return jce_v3(sqrtf(m->raw[0][0] * m->raw[0][0] +
                        m->raw[0][1] * m->raw[0][1] +
                        m->raw[0][2] * m->raw[0][2]),
                  sqrtf(m->raw[1][0] * m->raw[1][0] +
                        m->raw[1][1] * m->raw[1][1] +
                        m->raw[1][2] * m->raw[1][2]),
                  sqrtf(m->raw[2][0] * m->raw[2][0] +
                        m->raw[2][1] * m->raw[2][1] +
                        m->raw[2][2] * m->raw[2][2]));
}

JCE_INLINE jce_quat jce_q_identity(void)
{
    return jce_v4(0.0f, 0.0f, 0.0f, 1.0f);
}

JCE_INLINE jce_quat jce_q_from_axis_angle(jce_vec3 axis, float angle_rad)
{
    float half = angle_rad * 0.5f;
    float s = sinf(half);
    jce_vec3 n = jce_v3_normalize(axis);
    return jce_v4(n.x * s, n.y * s, n.z * s, cosf(half));
}

JCE_INLINE jce_quat jce_q_multiply(jce_quat a, jce_quat b)
{
    return jce_v4(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                  a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                  a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                  a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}

JCE_INLINE jce_quat jce_q_normalize(jce_quat q)
{
    float len = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-8f)
        return jce_q_identity();
    return jce_v4(q.x / len, q.y / len, q.z / len, q.w / len);
}

JCE_INLINE jce_vec3 jce_q_rotate(jce_quat q, jce_vec3 v)
{
    jce_vec3 u = jce_v3(q.x, q.y, q.z);
    jce_vec3 uv = jce_v3_cross(u, v);
    jce_vec3 uuv = jce_v3_cross(u, uv);
    uv = jce_v3_scale(uv, 2.0f * q.w);
    uuv = jce_v3_scale(uuv, 2.0f);
    return jce_v3_add(v, jce_v3_add(uv, uuv));
}

JCE_INLINE jce_mat4 jce_q_to_mat4(jce_quat q)
{
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2;
    float yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;
    jce_mat4 m = jce_m4_identity();

    m.raw[0][0] = 1.0f - (yy + zz);
    m.raw[0][1] = xy + wz;
    m.raw[0][2] = xz - wy;
    m.raw[1][0] = xy - wz;
    m.raw[1][1] = 1.0f - (xx + zz);
    m.raw[1][2] = yz + wx;
    m.raw[2][0] = xz + wy;
    m.raw[2][1] = yz - wx;
    m.raw[2][2] = 1.0f - (xx + yy);
    return m;
}

JCE_INLINE jce_mat4 jce_m4_from_trs(jce_vec3 t, jce_quat r, jce_vec3 s)
{
    jce_mat4 out = jce_q_to_mat4(r);
    out.col[0] = jce_v4_scale(out.col[0], s.x);
    out.col[1] = jce_v4_scale(out.col[1], s.y);
    out.col[2] = jce_v4_scale(out.col[2], s.z);
    out.col[3] = jce_v4(t.x, t.y, t.z, 1.0f);
    return out;
}

/* Sanitize a transform scale for TRS composition: zero components default to
 * 1 so a degenerate authored scale cannot collapse the matrix. Sign is
 * PRESERVED — mirrored (negative) scales stay mirrored. Use this flavor when
 * feeding jce_m4_from_trs. */
JCE_INLINE jce_vec3 jce_v3_safe_scale(jce_vec3 s)
{
    return jce_v3(s.x != 0.0f ? s.x : 1.0f,
                  s.y != 0.0f ? s.y : 1.0f,
                  s.z != 0.0f ? s.z : 1.0f);
}

/* Magnitude flavor of jce_v3_safe_scale: |component| with zeros defaulting to
 * 1. For radius / half-extent / gizmo math that needs positive extents. NOT
 * for TRS composition — it drops mirroring. */
JCE_INLINE jce_vec3 jce_v3_abs_safe_scale(jce_vec3 s)
{
    return jce_v3(s.x != 0.0f ? fabsf(s.x) : 1.0f,
                  s.y != 0.0f ? fabsf(s.y) : 1.0f,
                  s.z != 0.0f ? fabsf(s.z) : 1.0f);
}

JCE_INLINE jce_quat jce_m4_to_quat(const jce_mat4 *m)
{
    float m00 = m->raw[0][0], m11 = m->raw[1][1], m22 = m->raw[2][2];
    float trace = m00 + m11 + m22;
    jce_quat q;

    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m->raw[1][2] - m->raw[2][1]) / s;
        q.y = (m->raw[2][0] - m->raw[0][2]) / s;
        q.z = (m->raw[0][1] - m->raw[1][0]) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m->raw[1][2] - m->raw[2][1]) / s;
        q.x = 0.25f * s;
        q.y = (m->raw[1][0] + m->raw[0][1]) / s;
        q.z = (m->raw[2][0] + m->raw[0][2]) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m->raw[2][0] - m->raw[0][2]) / s;
        q.x = (m->raw[1][0] + m->raw[0][1]) / s;
        q.y = 0.25f * s;
        q.z = (m->raw[2][1] + m->raw[1][2]) / s;
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m->raw[0][1] - m->raw[1][0]) / s;
        q.x = (m->raw[2][0] + m->raw[0][2]) / s;
        q.y = (m->raw[2][1] + m->raw[1][2]) / s;
        q.z = 0.25f * s;
    }
    return jce_q_normalize(q);
}

/* Decompose an affine TRS matrix into translation, rotation and scale.
 * Lossy for sheared matrices (non-uniform parent scale + child rotation),
 * like Unity's decompose — the common case (uniform scale / no mixed
 * rotation) round-trips exactly. Any out param may be NULL. */
JCE_INLINE void jce_m4_decompose(const jce_mat4 *m, jce_vec3 *out_t,
                                 jce_quat *out_r, jce_vec3 *out_s)
{
    const float sx = sqrtf(m->raw[0][0]*m->raw[0][0] + m->raw[0][1]*m->raw[0][1] + m->raw[0][2]*m->raw[0][2]);
    const float sy = sqrtf(m->raw[1][0]*m->raw[1][0] + m->raw[1][1]*m->raw[1][1] + m->raw[1][2]*m->raw[1][2]);
    const float sz = sqrtf(m->raw[2][0]*m->raw[2][0] + m->raw[2][1]*m->raw[2][1] + m->raw[2][2]*m->raw[2][2]);

    if (out_t) *out_t = jce_v3(m->raw[3][0], m->raw[3][1], m->raw[3][2]);
    if (out_s) *out_s = jce_v3(sx, sy, sz);
    if (out_r) {
        jce_mat4 r = *m;
        if (sx > 1e-8f) { r.raw[0][0]/=sx; r.raw[0][1]/=sx; r.raw[0][2]/=sx; }
        if (sy > 1e-8f) { r.raw[1][0]/=sy; r.raw[1][1]/=sy; r.raw[1][2]/=sy; }
        if (sz > 1e-8f) { r.raw[2][0]/=sz; r.raw[2][1]/=sz; r.raw[2][2]/=sz; }
        r.raw[0][3] = r.raw[1][3] = r.raw[2][3] = 0.0f;
        r.raw[3][0] = r.raw[3][1] = r.raw[3][2] = 0.0f; r.raw[3][3] = 1.0f;
        *out_r = jce_m4_to_quat(&r);
    }
}

JCE_INLINE jce_quat jce_q_from_euler(float pitch, float yaw, float roll)
{
    jce_quat qy = jce_q_from_axis_angle(jce_v3(0.0f, 1.0f, 0.0f), yaw);
    jce_quat qx = jce_q_from_axis_angle(jce_v3(1.0f, 0.0f, 0.0f), pitch);
    jce_quat qz = jce_q_from_axis_angle(jce_v3(0.0f, 0.0f, 1.0f), roll);
    return jce_q_multiply(jce_q_multiply(qy, qx), qz);
}

JCE_INLINE jce_quat jce_q_slerp(jce_quat a, jce_quat b, float t)
{
    float cos_theta = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;

    if (cos_theta < 0.0f) {
        b = jce_v4(-b.x, -b.y, -b.z, -b.w);
        cos_theta = -cos_theta;
    }

    if (cos_theta > 0.9995f) {
        return jce_q_normalize(jce_v4(
            a.x + t * (b.x - a.x),
            a.y + t * (b.y - a.y),
            a.z + t * (b.z - a.z),
            a.w + t * (b.w - a.w)));
    }

    {
        float theta = acosf(cos_theta);
        float sin_theta = sinf(theta);
        float wa = sinf((1.0f - t) * theta) / sin_theta;
        float wb = sinf(t * theta) / sin_theta;
        return jce_v4(a.x * wa + b.x * wb,
                      a.y * wa + b.y * wb,
                      a.z * wa + b.z * wb,
                      a.w * wa + b.w * wb);
    }
}

JCE_INLINE jce_vec3 jce_q_to_euler(jce_quat q)
{
    jce_vec3 e;
    float sinx = 2.0f * (q.w * q.x - q.y * q.z);

    if (sinx > 1.0f)
        sinx = 1.0f;
    if (sinx < -1.0f)
        sinx = -1.0f;
    e.x = asinf(sinx);
    if (fabsf(sinx) >= 0.99999f) {
        e.y = atan2f(-2.0f * (q.x * q.z - q.w * q.y),
                     1.0f - 2.0f * (q.y * q.y + q.z * q.z));
        e.z = 0.0f;
    } else {
        e.y = atan2f(2.0f * (q.x * q.z + q.w * q.y),
                     1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        e.z = atan2f(2.0f * (q.x * q.y + q.w * q.z),
                     1.0f - 2.0f * (q.x * q.x + q.z * q.z));
    }
    return e;
}

JCE_INLINE jce_quat jce_euler_to_q(float pitch_deg, float yaw_deg,
                                   float roll_deg)
{
    return jce_q_from_euler(pitch_deg * JCE_DEG2RAD,
                            yaw_deg * JCE_DEG2RAD,
                            roll_deg * JCE_DEG2RAD);
}

JCE_INLINE bool jce_ray_aabb_intersect(jce_vec3 origin, jce_vec3 dir,
                                       jce_vec3 box_min, jce_vec3 box_max,
                                       float *t_min)
{
    float tmin = -1e30f, tmax = 1e30f;
    float o[3] = { origin.x, origin.y, origin.z };
    float d[3] = { dir.x, dir.y, dir.z };
    float bmin[3] = { box_min.x, box_min.y, box_min.z };
    float bmax[3] = { box_max.x, box_max.y, box_max.z };
    int i;

    for (i = 0; i < 3; i++) {
        if (fabsf(d[i]) < 1e-8f) {
            if (o[i] < bmin[i] || o[i] > bmax[i])
                return false;
        } else {
            float inv = 1.0f / d[i];
            float t1 = (bmin[i] - o[i]) * inv;
            float t2 = (bmax[i] - o[i]) * inv;
            if (t1 > t2) {
                float tmp = t1;
                t1 = t2;
                t2 = tmp;
            }
            if (t1 > tmin)
                tmin = t1;
            if (t2 < tmax)
                tmax = t2;
            if (tmin > tmax)
                return false;
        }
    }
    if (t_min)
        *t_min = tmin;
    return true;
}

JCE_INLINE bool jce_world_to_screen(jce_vec3 world_pos,
                                    const jce_mat4 *view,
                                    const jce_mat4 *proj,
                                    float *out_x, float *out_y)
{
    jce_mat4 vp = jce_m4_multiply(proj, view);
    jce_vec4 p = jce_v4(world_pos.x, world_pos.y, world_pos.z, 1.0f);
    jce_vec4 clip = jce_m4_mul_v4(&vp, p);

    if (clip.w <= 1e-6f)
        return false;
    if (out_x)
        *out_x = (clip.x / clip.w) * 0.5f + 0.5f;
    if (out_y)
        *out_y = 1.0f - ((clip.y / clip.w) * 0.5f + 0.5f);
    return true;
}

JCE_EXTERN_C_END

#endif /* JCE_MATH_H */
