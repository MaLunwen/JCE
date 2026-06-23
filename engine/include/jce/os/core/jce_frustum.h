/*
 * jce_frustum.h  Shared frustum-plane + AABB culling math (Layer 1 core).
 *
 * One canonical implementation of the three routines that were previously
 * copy-pasted byte-for-byte across the scene renderer, the GPU pick pass and
 * the spatial-partition broadphase.  A fix to the math (plane normalisation,
 * a degenerate-matrix guard, the positive-vertex test) now lives in exactly
 * one place and can never silently diverge between cull sites.
 *
 * Header-only (JCE_INLINE) like the rest of jce_math.h — no link dependency.
 */

#ifndef JCE_FRUSTUM_H
#define JCE_FRUSTUM_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <float.h>
#include <math.h>
#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* Extract the 6 frustum planes (left,right,bottom,top,near,far) from a
 * view-projection matrix (Gribb-Hartmann), normalised so plane.w is a true
 * signed distance.  `planes[i] = {nx,ny,nz,d}` with inside == dot(n,p)+d >= 0. */
JCE_INLINE void jce_frustum_extract_planes(const jce_mat4 *m, jce_vec4 planes[6])
{
    #define JCE_FR_RC(col, row) m->raw[(col)][(row)]
    for (int i = 0; i < 6; i++) {
        const int row  = i / 2;          /* 0..2 */
        const int sign = (i & 1) ? -1 : 1;
        planes[i].x = JCE_FR_RC(0, 3) + sign * JCE_FR_RC(0, row);
        planes[i].y = JCE_FR_RC(1, 3) + sign * JCE_FR_RC(1, row);
        planes[i].z = JCE_FR_RC(2, 3) + sign * JCE_FR_RC(2, row);
        planes[i].w = JCE_FR_RC(3, 3) + sign * JCE_FR_RC(3, row);
        const float L = sqrtf(planes[i].x * planes[i].x +
                              planes[i].y * planes[i].y +
                              planes[i].z * planes[i].z);
        if (L > 1e-6f) {
            const float inv = 1.0f / L;
            planes[i].x *= inv;
            planes[i].y *= inv;
            planes[i].z *= inv;
            planes[i].w *= inv;
        }
    }
    #undef JCE_FR_RC
}

/* True if the world-space AABB [mn,mx] is at least partially inside the frustum
 * (positive-vertex test: conservative, no false negatives). */
JCE_INLINE bool jce_aabb_in_frustum(const jce_vec4 planes[6],
                                    jce_vec3 mn, jce_vec3 mx)
{
    for (int p = 0; p < 6; p++) {
        /* Positive vertex (farthest along the plane normal). If even that is
         * outside (signed distance < 0), the whole box is outside this plane. */
        float px = (planes[p].x >= 0.0f) ? mx.x : mn.x;
        float py = (planes[p].y >= 0.0f) ? mx.y : mn.y;
        float pz = (planes[p].z >= 0.0f) ? mx.z : mn.z;
        float d  = planes[p].x * px + planes[p].y * py + planes[p].z * pz
                 + planes[p].w;
        if (d < 0.0f) return false;
    }
    return true;
}

/* World-space AABB of a local box [lmn,lmx] transformed by `m` (8-corner). */
JCE_INLINE void jce_transform_aabb(const jce_mat4 *m, jce_vec3 lmn, jce_vec3 lmx,
                                   jce_vec3 *out_mn, jce_vec3 *out_mx)
{
    jce_vec3 wmn = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
    jce_vec3 wmx = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (int c = 0; c < 8; c++) {
        jce_vec4 lv = {
            (c & 1) ? lmx.x : lmn.x,
            (c & 2) ? lmx.y : lmn.y,
            (c & 4) ? lmx.z : lmn.z,
            1.0f
        };
        jce_vec4 wv = jce_m4_mul_v4(m, lv);
        if (wv.x < wmn.x) wmn.x = wv.x; if (wv.x > wmx.x) wmx.x = wv.x;
        if (wv.y < wmn.y) wmn.y = wv.y; if (wv.y > wmx.y) wmx.y = wv.y;
        if (wv.z < wmn.z) wmn.z = wv.z; if (wv.z > wmx.z) wmx.z = wv.z;
    }
    *out_mn = wmn;
    *out_mx = wmx;
}

JCE_EXTERN_C_END

#endif /* JCE_FRUSTUM_H */
