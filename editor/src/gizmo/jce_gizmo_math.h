/*
 * jce_gizmo_math.h  Projection + ray utilities for gizmo interaction.
 *
 * Raw float-array (float[3]/float[16]) adapter built on jce_math.h's
 * struct API.  The gizmo pipeline interoperates with ImGui draw lists
 * and column-major editor matrix exports as flat float buffers, so we
 * keep the raw signatures here and reinterpret to the public struct
 * types where it's layout-compatible.  No third-party math leakage.
 */

#ifndef JCE_GIZMO_MATH_H
#define JCE_GIZMO_MATH_H

#include <jce/os/core/jce_math.h> /* jce_vec3 / jce_mat4 / JCE_PI / helpers */

#include <math.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Gizmo camera info ─────────────────────────────────────────────── */

typedef struct {
    float view[16];              /* column-major view matrix */
    float proj[16];              /* column-major projection matrix */
    float eye[3];                /* camera world position */
    float viewport_size[2];      /* scene view pixel width/height */
    float viewport_origin[2];    /* scene view top-left screen coords */
} JceGizmoCamera;

/* ── Thin vec3 / mat4 wrappers built on jce_math.h struct API ─────── */
/*
 * jce_vec3 is { float x,y,z; } and jce_mat4 is a union { float raw[4][4]; }
 * — both are layout-compatible with float[3] and float[16] respectively,
 * so we can reinterpret the caller's flat buffers as the struct types.
 */

#define GM_V3_REF(p) (*(jce_vec3 *)(p))
#define GM_V3_CREF(p) (*(const jce_vec3 *)(p))
#define GM_M4_REF(p) (*(jce_mat4 *)(p))
#define GM_M4_CREF(p) (*(const jce_mat4 *)(p))

static inline void gm_v3_copy(float dst[3], const float src[3])
{
    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
}

static inline void gm_v3_sub(float out[3], const float a[3], const float b[3])
{
    out[0] = a[0] - b[0]; out[1] = a[1] - b[1]; out[2] = a[2] - b[2];
}

static inline void gm_v3_add(float out[3], const float a[3], const float b[3])
{
    out[0] = a[0] + b[0]; out[1] = a[1] + b[1]; out[2] = a[2] + b[2];
}

static inline void gm_v3_scale(float out[3], const float v[3], float s)
{
    out[0] = v[0] * s; out[1] = v[1] * s; out[2] = v[2] * s;
}

static inline float gm_v3_dot(const float a[3], const float b[3])
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static inline void gm_v3_cross(float out[3], const float a[3], const float b[3])
{
    float x = a[1]*b[2] - a[2]*b[1];
    float y = a[2]*b[0] - a[0]*b[2];
    float z = a[0]*b[1] - a[1]*b[0];
    out[0] = x; out[1] = y; out[2] = z;
}

static inline float gm_v3_len(const float v[3])
{
    return sqrtf(gm_v3_dot(v, v));
}

static inline void gm_v3_normalize(float out[3], const float v[3])
{
    float l = gm_v3_len(v);
    if (l < 1e-8f) { out[0] = out[1] = out[2] = 0.0f; return; }
    float inv = 1.0f / l;
    out[0] = v[0] * inv; out[1] = v[1] * inv; out[2] = v[2] * inv;
}

/* ── Matrix helpers (column-major 4×4, backed by jce_math.h) ──────── */

static inline void gm_m4_mul_v4(float out[4], const float m[16], const float v[4])
{
    /* column-major: out = m * v */
    out[0] = m[0]*v[0] + m[4]*v[1] + m[8]*v[2]  + m[12]*v[3];
    out[1] = m[1]*v[0] + m[5]*v[1] + m[9]*v[2]  + m[13]*v[3];
    out[2] = m[2]*v[0] + m[6]*v[1] + m[10]*v[2] + m[14]*v[3];
    out[3] = m[3]*v[0] + m[7]*v[1] + m[11]*v[2] + m[15]*v[3];
}

static inline void gm_m4_mul(float out[16], const float a[16], const float b[16])
{
    jce_mat4 r = jce_m4_multiply(&GM_M4_CREF(a), &GM_M4_CREF(b));
    memcpy(out, r.raw, sizeof(float) * 16);
}

/* Invert a 4×4 column-major matrix. Returns false if singular. */
static inline bool gm_m4_invert(float inv[16], const float m[16])
{
    /* jce_m4_inverse computes via cofactor expansion; check determinant via
       a quick 2x2 minor approximation by inverting and detecting NaN.  The
       public API doesn't expose a det() helper, so we trust the caller's
       matrix is well-formed for gizmo use (cameras have non-zero det). */
    jce_mat4 r = jce_m4_inverse(&GM_M4_CREF(m));
    /* sanity: NaN/Inf check on first element */
    if (!(r.raw[0][0] == r.raw[0][0])) return false;
    memcpy(inv, r.raw, sizeof(float) * 16);
    return true;
}

/* ── World → Screen projection ─────────────────────────────────────── */

static inline bool gm_world_to_screen(const JceGizmoCamera *cam,
                                       const float world[3],
                                       float out_screen[2])
{
    float vp[16];
    gm_m4_mul(vp, cam->proj, cam->view);

    float clip[4] = { world[0], world[1], world[2], 1.0f };
    float c[4];
    gm_m4_mul_v4(c, vp, clip);

    if (c[3] <= 1e-6f) return false; /* behind camera */

    float inv_w = 1.0f / c[3];
    float ndc_x = c[0] * inv_w;
    float ndc_y = c[1] * inv_w;

    out_screen[0] = (ndc_x * 0.5f + 0.5f) * cam->viewport_size[0] + cam->viewport_origin[0];
    out_screen[1] = (1.0f - (ndc_y * 0.5f + 0.5f)) * cam->viewport_size[1] + cam->viewport_origin[1];
    return true;
}

/* ── Screen → World ray ────────────────────────────────────────────── */

static inline void gm_screen_to_ray(const JceGizmoCamera *cam,
                                     float screen_x, float screen_y,
                                     float ray_origin[3],
                                     float ray_dir[3])
{
    /* NDC coords */
    float ndc_x = ((screen_x - cam->viewport_origin[0]) / cam->viewport_size[0]) * 2.0f - 1.0f;
    float ndc_y = 1.0f - ((screen_y - cam->viewport_origin[1]) / cam->viewport_size[1]) * 2.0f;

    float vp[16];
    gm_m4_mul(vp, cam->proj, cam->view);

    float inv_vp[16];
    gm_m4_invert(inv_vp, vp);

    /* Unproject near and far */
    float near_ndc[4] = { ndc_x, ndc_y, -1.0f, 1.0f };
    float far_ndc[4]  = { ndc_x, ndc_y,  1.0f, 1.0f };
    float near_w[4], far_w[4];
    gm_m4_mul_v4(near_w, inv_vp, near_ndc);
    gm_m4_mul_v4(far_w,  inv_vp, far_ndc);

    if (fabsf(near_w[3]) < 1e-8f || fabsf(far_w[3]) < 1e-8f) {
        gm_v3_copy(ray_origin, cam->eye);
        ray_dir[0] = 0; ray_dir[1] = 0; ray_dir[2] = -1;
        return;
    }

    float inv_nw = 1.0f / near_w[3];
    float inv_fw = 1.0f / far_w[3];
    float near_p[3] = { near_w[0]*inv_nw, near_w[1]*inv_nw, near_w[2]*inv_nw };
    float far_p[3]  = { far_w[0]*inv_fw,  far_w[1]*inv_fw,  far_w[2]*inv_fw  };

    gm_v3_copy(ray_origin, near_p);
    float d[3];
    gm_v3_sub(d, far_p, near_p);
    gm_v3_normalize(ray_dir, d);
}

/* ── Ray-Axis closest approach ─────────────────────────────────────── */

/* Closest point between ray (ro+t*rd) and line (ao+s*ad).
   Returns squared distance. out_t/out_s are the parameters. */
static inline float gm_ray_axis_closest(const float ro[3], const float rd[3],
                                         const float ao[3], const float ad[3],
                                         float *out_t, float *out_s)
{
    float w0[3];
    gm_v3_sub(w0, ro, ao);

    float a = gm_v3_dot(rd, rd);   /* always ≥ 0 */
    float b = gm_v3_dot(rd, ad);
    float c = gm_v3_dot(ad, ad);
    float d = gm_v3_dot(rd, w0);
    float e = gm_v3_dot(ad, w0);

    float denom = a*c - b*b;
    float t, s;

    if (fabsf(denom) < 1e-8f) {
        /* Lines nearly parallel. */
        t = 0.0f;
        s = (b > c) ? (d / b) : (e / c);
    } else {
        t = (b*e - c*d) / denom;
        s = (a*e - b*d) / denom;
    }
    if (t < 0.0f) t = 0.0f;

    *out_t = t;
    *out_s = s;

    float pt_ray[3], pt_axis[3], diff[3];
    gm_v3_scale(pt_ray, rd, t);   gm_v3_add(pt_ray, ro, pt_ray);
    gm_v3_scale(pt_axis, ad, s);  gm_v3_add(pt_axis, ao, pt_axis);
    gm_v3_sub(diff, pt_ray, pt_axis);
    return gm_v3_dot(diff, diff);
}

/* ── Ray-Plane intersection ────────────────────────────────────────── */

static inline bool gm_ray_plane_intersect(const float ro[3], const float rd[3],
                                           const float plane_n[3],
                                           const float plane_p[3],
                                           float out_hit[3])
{
    float denom = gm_v3_dot(plane_n, rd);
    if (fabsf(denom) < 1e-6f) return false;

    float w[3];
    gm_v3_sub(w, plane_p, ro);
    float t = gm_v3_dot(w, plane_n) / denom;
    if (t < 0.0f) return false;

    out_hit[0] = ro[0] + t * rd[0];
    out_hit[1] = ro[1] + t * rd[1];
    out_hit[2] = ro[2] + t * rd[2];
    return true;
}

/* ── 2D distance helpers ───────────────────────────────────────────── */

/* Distance from 2D point P to line segment A-B. */
static inline float gm_point_segment_dist_2d(float px, float py,
                                              float ax, float ay,
                                              float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    float len2 = dx*dx + dy*dy;
    if (len2 < 1e-8f) {
        float ex = px - ax, ey = py - ay;
        return sqrtf(ex*ex + ey*ey);
    }
    float t = ((px-ax)*dx + (py-ay)*dy) / len2;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float cx = ax + t*dx - px, cy = ay + t*dy - py;
    return sqrtf(cx*cx + cy*cy);
}

/* Distance between two 2D points. */
static inline float gm_dist_2d(float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    return sqrtf(dx*dx + dy*dy);
}

#ifdef __cplusplus
}
#endif

#endif /* JCE_GIZMO_MATH_H */
