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

/* Same predicate as jce_aabb_in_frustum, in center/extent form.
 *
 * The positive vertex is p_i = (n_i >= 0) ? mx_i : mn_i, so
 *     n . p + w  ==  (n . c + w) + |n| . e
 * with c the box center and e its half-extent. That is an identity, not an
 * approximation -- it agrees on every input including n_i == 0, where e_i
 * contributes nothing either way. What it buys is that the three sign tests
 * and three selects per plane become |n|, which depends only on the frustum:
 * hoist it out with jce_frustum_abs_normals and the per-box work is pure
 * multiply-add with no data-dependent selection.
 *
 * Worth it wherever one frustum is tested against many boxes -- the broad-phase
 * query runs this ~108k times per frame at the 200k bench. For a one-off test,
 * use jce_aabb_in_frustum and skip the setup. */
JCE_INLINE void jce_frustum_abs_normals(const jce_vec4 planes[6],
                                        float out_absn[6][3])
{
    for (int p = 0; p < 6; p++) {
        out_absn[p][0] = planes[p].x < 0.0f ? -planes[p].x : planes[p].x;
        out_absn[p][1] = planes[p].y < 0.0f ? -planes[p].y : planes[p].y;
        out_absn[p][2] = planes[p].z < 0.0f ? -planes[p].z : planes[p].z;
    }
}

JCE_INLINE bool jce_aabb_in_frustum_fast(const jce_vec4 planes[6],
                                         const float absn[6][3],
                                         jce_vec3 mn, jce_vec3 mx)
{
    const float cx = (mn.x + mx.x) * 0.5f, ex = (mx.x - mn.x) * 0.5f;
    const float cy = (mn.y + mx.y) * 0.5f, ey = (mx.y - mn.y) * 0.5f;
    const float cz = (mn.z + mx.z) * 0.5f, ez = (mx.z - mn.z) * 0.5f;
    for (int p = 0; p < 6; p++) {
        const float d = planes[p].x * cx + planes[p].y * cy
                      + planes[p].z * cz + planes[p].w;
        const float r = absn[p][0] * ex + absn[p][1] * ey + absn[p][2] * ez;
        if (d + r < 0.0f) return false;
    }
    return true;
}

/* Three-way: 0 = outside, 1 = straddles a plane, 2 = fully inside all six.
 *
 * The center/extent form already computes both numbers this needs. `d + r < 0`
 * puts the positive vertex outside and rejects; `d - r >= 0` puts the NEGATIVE
 * vertex inside, which means the whole box is. The second answer therefore
 * costs one subtract and one compare per plane, not a second pass.
 *
 * Worth it for a hierarchy -- a cell known fully inside implies every object
 * registered in it is visible, so its contents skip testing. Meaningless for a
 * lone box, where 1 and 2 lead to the same action. */
JCE_INLINE int jce_aabb_frustum_classify(const jce_vec4 planes[6],
                                         const float absn[6][3],
                                         jce_vec3 mn, jce_vec3 mx)
{
    const float cx = (mn.x + mx.x) * 0.5f, ex = (mx.x - mn.x) * 0.5f;
    const float cy = (mn.y + mx.y) * 0.5f, ey = (mx.y - mn.y) * 0.5f;
    const float cz = (mn.z + mx.z) * 0.5f, ez = (mx.z - mn.z) * 0.5f;
    int all_in = 1;
    for (int p = 0; p < 6; p++) {
        const float d = planes[p].x * cx + planes[p].y * cy
                      + planes[p].z * cz + planes[p].w;
        const float r = absn[p][0] * ex + absn[p][1] * ey + absn[p][2] * ez;
        if (d + r < 0.0f) return 0;
        if (d - r <  0.0f) all_in = 0;
    }
    return all_in ? 2 : 1;
}

/* The 8 frustum corners, derived from the 6 planes by intersecting one plane
 * from each opposing pair.  Relies on the extraction order above:
 * 0=left 1=right 2=bottom 3=top 4=near 5=far.  Corner i uses
 * left/right by bit0, bottom/top by bit1, near/far by bit2.
 *
 * Returns false for a degenerate frustum (parallel planes, singular solve);
 * callers must then fall back to the conservative test. */
JCE_INLINE bool jce_frustum_corners(const jce_vec4 planes[6], jce_vec3 out[8])
{
    for (int i = 0; i < 8; i++) {
        const jce_vec4 a = planes[(i & 1) ? 1 : 0];
        const jce_vec4 b = planes[(i & 2) ? 3 : 2];
        const jce_vec4 c = planes[(i & 4) ? 5 : 4];
        /* Cramer: p = (-da (nb x nc) - db (nc x na) - dc (na x nb)) / det */
        const float bcx = b.y * c.z - b.z * c.y;
        const float bcy = b.z * c.x - b.x * c.z;
        const float bcz = b.x * c.y - b.y * c.x;
        const float cax = c.y * a.z - c.z * a.y;
        const float cay = c.z * a.x - c.x * a.z;
        const float caz = c.x * a.y - c.y * a.x;
        const float abx = a.y * b.z - a.z * b.y;
        const float aby = a.z * b.x - a.x * b.z;
        const float abz = a.x * b.y - a.y * b.x;
        const float det = a.x * bcx + a.y * bcy + a.z * bcz;
        if (det > -1e-9f && det < 1e-9f) return false;
        const float inv = 1.0f / det;
        out[i].x = (-a.w * bcx - b.w * cax - c.w * abx) * inv;
        out[i].y = (-a.w * bcy - b.w * cay - c.w * aby) * inv;
        out[i].z = (-a.w * bcz - b.w * caz - c.w * abz) * inv;
    }
    return true;
}

/* Exact-er AABB/frustum overlap: the plane test above plus the missing half of
 * the separating-axis theorem.
 *
 * The positive-vertex test alone only checks the frustum's 6 face normals.  A
 * box that straddles a frustum corner can be outside the frustum while sitting
 * inside every individual plane's half-space -- the classic false positive, and
 * it gets worse the more elongated the box is.  Adding the box's own 3 face
 * axes (all 8 frustum corners on one side of a box face => separated) removes
 * exactly that class.
 *
 * `corners` comes from jce_frustum_corners for the SAME planes; pass NULL to
 * skip the extra axes and get the plain conservative answer.
 *
 * Still conservative -- the 6 edge-cross axes of a full SAT are omitted, so a
 * few false positives survive -- but never a false negative, which is the
 * property culling depends on. */
JCE_INLINE bool jce_aabb_in_frustum_exact(const jce_vec4 planes[6],
                                          const jce_vec3 *corners,
                                          jce_vec3 mn, jce_vec3 mx)
{
    if (!jce_aabb_in_frustum(planes, mn, mx)) return false;
    if (!corners) return true;
    {
        float cmin[3], cmax[3];
        cmin[0] = cmax[0] = corners[0].x;
        cmin[1] = cmax[1] = corners[0].y;
        cmin[2] = cmax[2] = corners[0].z;
        for (int i = 1; i < 8; i++) {
            const float v[3] = { corners[i].x, corners[i].y, corners[i].z };
            for (int a = 0; a < 3; a++) {
                if (v[a] < cmin[a]) cmin[a] = v[a];
                if (v[a] > cmax[a]) cmax[a] = v[a];
            }
        }
        if (cmin[0] > mx.x || cmax[0] < mn.x) return false;
        if (cmin[1] > mx.y || cmax[1] < mn.y) return false;
        if (cmin[2] > mx.z || cmax[2] < mn.z) return false;
    }
    return true;
}


/* World-space AABB of a local box [lmn,lmx] transformed by an affine matrix.
 * Center/extents is exactly equivalent to transforming all eight corners, but
 * needs one point transform plus an abs(linear3x3)*extent multiply. */
JCE_INLINE void jce_transform_aabb(const jce_mat4 *m, jce_vec3 lmn, jce_vec3 lmx,
                                   jce_vec3 *out_mn, jce_vec3 *out_mx)
{
    const jce_vec3 center = {
        (lmn.x + lmx.x) * 0.5f,
        (lmn.y + lmx.y) * 0.5f,
        (lmn.z + lmx.z) * 0.5f
    };
    const jce_vec3 extent = {
        (lmx.x - lmn.x) * 0.5f,
        (lmx.y - lmn.y) * 0.5f,
        (lmx.z - lmn.z) * 0.5f
    };
    const jce_vec4 world_center = jce_m4_mul_v4(
        m, jce_v4(center.x, center.y, center.z, 1.0f));
    const jce_vec3 world_extent = {
        fabsf(m->col[0].x) * extent.x +
        fabsf(m->col[1].x) * extent.y +
        fabsf(m->col[2].x) * extent.z,
        fabsf(m->col[0].y) * extent.x +
        fabsf(m->col[1].y) * extent.y +
        fabsf(m->col[2].y) * extent.z,
        fabsf(m->col[0].z) * extent.x +
        fabsf(m->col[1].z) * extent.y +
        fabsf(m->col[2].z) * extent.z
    };

    *out_mn = jce_v3(world_center.x - world_extent.x,
                     world_center.y - world_extent.y,
                     world_center.z - world_extent.z);
    *out_mx = jce_v3(world_center.x + world_extent.x,
                     world_center.y + world_extent.y,
                     world_center.z + world_extent.z);
}

JCE_EXTERN_C_END

#endif /* JCE_FRUSTUM_H */
