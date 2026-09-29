/*
 * jce_sr_portal.c  Occlusion portals as conservative CPU occluder volumes.
 *
 * See jce_sr_portal.h for why this exists and what it is NOT (a portal/cell
 * culler).
 *
 * THE PROOF THIS IMPLEMENTS.  A closed portal is a solid box.  Take one of its
 * six faces: a planar convex quad.  Projected with every corner in front of the
 * camera, it stays a convex quad in NDC.  If an occludee's whole screen
 * footprint lies inside that quad, every eye ray reaching the occludee crosses
 * the face; if the occludee's NEAREST point is farther than the face's FARTHEST
 * point, every one of those crossings happens first.  So the occludee is hidden.
 *
 * Both halves are one-sided, and that is the whole safety argument:
 *   - "all 8 projected corners inside the quad" implies the convex hull of the
 *     footprint is inside it, so it never over-claims coverage.  (The screen
 *     BOUNDING RECT of a projected box does over-claim -- the silhouette of a
 *     projected box is a hexagon, not a rectangle -- which is why this tests
 *     faces and not the box's rect.)
 *   - "nearest occludee point behind farthest face point" is stricter than the
 *     per-pixel depth comparison a rasteriser would do.
 * Anything unproven returns false, so this can drop draws but never geometry.
 */

#include "jce_sr_portal.h"

#include <jce/middleware/scene/jce_scene.h>

#include <jce/os/core/jce_alloc.h>

#include <string.h>

/* A closed portal contributes at most this many occluders.  Exceeding it drops
 * the extras, which costs culling and never correctness -- fewer occluders can
 * only mean fewer things proven hidden.  Said out loud rather than silently
 * truncated: a cap nobody mentions reads as "covered everything". */
#define SR_PORTAL_MAX 32

/* Faces of the unit box in corner-index order, wound consistently.  Corner i
 * has bit 0 = +x, bit 1 = +y, bit 2 = +z. */
static const int k_face_idx[6][4] = {
    { 0, 2, 6, 4 },   /* -x */
    { 1, 5, 7, 3 },   /* +x */
    { 0, 4, 5, 1 },   /* -y */
    { 2, 3, 7, 6 },   /* +y */
    { 0, 1, 3, 2 },   /* -z */
    { 4, 6, 7, 5 },   /* +z */
};

typedef struct {
    float qx[4], qy[4];   /* the face's projected quad, NDC */
    float zmax;           /* farthest NDC depth over the quad */
    bool  usable;         /* false = degenerate (edge-on) or too small */
} SrPortalFace;

typedef struct {
    SrPortalFace faces[6];
    float rx0, ry0, rx1, ry1;   /* NDC bounding rect over all 8 corners */
    float znear;                /* nearest NDC depth over all 8 corners */
} SrPortalBox;

struct SrPortalSet {
    jce_mat4    viewproj;
    int         count;
    SrPortalBox box[SR_PORTAL_MAX];
};

/* ── projection ──────────────────────────────────────────────────────── */

/* Project a world point.  Returns false when it is at or behind the eye plane,
 * which is the caller's signal to give up on this box rather than guess. */
static bool sr_portal_project(const jce_mat4 *vp, float x, float y, float z,
                              float *out_x, float *out_y, float *out_z)
{
    const float cx = vp->raw[0][0] * x + vp->raw[1][0] * y +
                     vp->raw[2][0] * z + vp->raw[3][0];
    const float cy = vp->raw[0][1] * x + vp->raw[1][1] * y +
                     vp->raw[2][1] * z + vp->raw[3][1];
    const float cz = vp->raw[0][2] * x + vp->raw[1][2] * y +
                     vp->raw[2][2] * z + vp->raw[3][2];
    const float cw = vp->raw[0][3] * x + vp->raw[1][3] * y +
                     vp->raw[2][3] * z + vp->raw[3][3];
    if (cw <= 1e-6f) return false;
    const float inv = 1.0f / cw;
    *out_x = cx * inv;
    *out_y = cy * inv;
    *out_z = cz * inv;
    return true;
}

/* All 8 corners of an axis-aligned box, in the bit order k_face_idx assumes. */
static void sr_portal_aabb_corners(jce_vec3 mn, jce_vec3 mx, float c[8][3])
{
    for (int i = 0; i < 8; ++i) {
        c[i][0] = (i & 1) ? mx.x : mn.x;
        c[i][1] = (i & 2) ? mx.y : mn.y;
        c[i][2] = (i & 4) ? mx.z : mn.z;
    }
}

/* Is p inside the convex quad q?  Same-sign cross products around the four
 * edges.  Winding may be either way -- a mirrored transform flips it -- so the
 * test asks for consistency, not a particular orientation. */
static bool sr_portal_in_quad(const float *qx, const float *qy,
                              float px, float py)
{
    int pos = 0, neg = 0;
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) & 3;
        const float ex = qx[j] - qx[i], ey = qy[j] - qy[i];
        const float vx = px - qx[i],    vy = py - qy[i];
        const float cr = ex * vy - ey * vx;
        if (cr > 1e-7f)       ++pos;
        else if (cr < -1e-7f) ++neg;
        /* A point exactly on an edge counts for neither: it is inside as far
         * as either winding is concerned. */
    }
    return pos == 0 || neg == 0;
}

/* ── per-frame build ─────────────────────────────────────────────────── */

typedef struct {
    JceScene            *scene;
    struct SrPortalSet  *set;
} SrPortalGather;

static void sr_portal_gather_cb(JceScene *scene, JceEntity e, void *ud)
{
    SrPortalGather *g = (SrPortalGather *)ud;
    if (!g || g->set->count >= SR_PORTAL_MAX) return;

    JceOcclusionPortalComponent *op = jce_scene_get_occlusion_portal(scene, e);
    if (!op || op->open) return;          /* an OPEN portal occludes nothing */
    if (op->size.x <= 0.0f || op->size.y <= 0.0f || op->size.z <= 0.0f) return;

    /* size is the box's FULL extent, matching the inspector's "Size" and the
     * 1,1,1 default a newly added portal carries.  Local, then transformed by
     * the entity world matrix, so rotation and scale apply -- a portal turned
     * to fit a doorway is the normal case, and an axis-aligned-only reading
     * would silently occlude the wrong volume. */
    const jce_mat4 w = jce_scene_get_world_matrix(scene, e);
    const jce_vec3 h = jce_v3(op->size.x * 0.5f,
                              op->size.y * 0.5f,
                              op->size.z * 0.5f);
    float lc[8][3];
    sr_portal_aabb_corners(jce_v3(-h.x, -h.y, -h.z), h, lc);

    SrPortalBox b;
    float nx[8], ny[8], nz[8];
    for (int i = 0; i < 8; ++i) {
        const float x = lc[i][0], y = lc[i][1], z = lc[i][2];
        const float wx = w.raw[0][0] * x + w.raw[1][0] * y +
                         w.raw[2][0] * z + w.raw[3][0];
        const float wy = w.raw[0][1] * x + w.raw[1][1] * y +
                         w.raw[2][1] * z + w.raw[3][1];
        const float wz = w.raw[0][2] * x + w.raw[1][2] * y +
                         w.raw[2][2] * z + w.raw[3][2];
        /* One corner behind the eye plane and the whole box is unusable: its
         * projection is no longer a bounded convex quad, and every guarantee
         * above rests on that.  A camera standing inside a portal therefore
         * culls nothing, which is the safe answer. */
        if (!sr_portal_project(&g->set->viewproj, wx, wy, wz,
                               &nx[i], &ny[i], &nz[i]))
            return;
    }

    b.rx0 = b.rx1 = nx[0];
    b.ry0 = b.ry1 = ny[0];
    b.znear = nz[0];
    for (int i = 1; i < 8; ++i) {
        if (nx[i] < b.rx0) b.rx0 = nx[i];
        if (nx[i] > b.rx1) b.rx1 = nx[i];
        if (ny[i] < b.ry0) b.ry0 = ny[i];
        if (ny[i] > b.ry1) b.ry1 = ny[i];
        if (nz[i] < b.znear) b.znear = nz[i];
    }

    for (int f = 0; f < 6; ++f) {
        SrPortalFace *fa = &b.faces[f];
        float area2 = 0.0f;
        fa->zmax = -3.0e38f;
        for (int k = 0; k < 4; ++k) {
            const int ci = k_face_idx[f][k];
            fa->qx[k] = nx[ci];
            fa->qy[k] = ny[ci];
            if (nz[ci] > fa->zmax) fa->zmax = nz[ci];
        }
        for (int k = 0; k < 4; ++k) {          /* shoelace */
            const int j = (k + 1) & 3;
            area2 += fa->qx[k] * fa->qy[j] - fa->qx[j] * fa->qy[k];
        }
        if (area2 < 0.0f) area2 = -area2;
        /* An edge-on face projects to a sliver; its four cross products are all
         * near zero, and the containment test would then accept anything.
         * Drop it rather than let a degenerate quad swallow the screen. */
        fa->usable = area2 > 1.0e-6f;
    }

    g->set->box[g->set->count++] = b;
}

struct SrPortalSet *sr_portal_set_create(void)
{
    struct SrPortalSet *set =
        (struct SrPortalSet *)jce_malloc(sizeof(struct SrPortalSet));
    if (set) memset(set, 0, sizeof(*set));
    return set;
}

void sr_portal_set_destroy(struct SrPortalSet *set)
{
    jce_free(set);
}

void sr_portal_set_build(struct SrPortalSet *set, JceScene *scene,
                         const JceCamera *camera, float aspect,
                         bool homogeneous_depth)
{
    if (!set) return;
    set->count = 0;
    if (!scene || !camera) return;

    const jce_mat4 v = jce_camera_view(camera);
    const jce_mat4 p = jce_camera_proj(camera, aspect, homogeneous_depth);
    set->viewproj = jce_m4_multiply(&p, &v);

    SrPortalGather g = { scene, set };
    jce_scene_each_entity(scene, sr_portal_gather_cb, &g);
}

void sr_portal_begin(JceSceneRenderer *sr, JceScene *scene,
                     const JceCamera *camera, float aspect)
{
    if (!sr) return;
    if (!sr->portals) {
        /* Allocated on first use: a scene with no portals never pays. */
        sr->portals = sr_portal_set_create();
        if (!sr->portals) return;
    }
    sr_portal_set_build(sr->portals, scene, camera, aspect,
                        sr->homogeneous_depth);
}

/* ── per-entity test ─────────────────────────────────────────────────── */

bool sr_portal_set_occludes(const struct SrPortalSet *set,
                            jce_vec3 wmin, jce_vec3 wmax)
{
    /* One branch in the overwhelmingly common case: no closed portals. */
    if (!set || set->count <= 0) return false;

    float c[8][3], nx[8], ny[8], nz[8];
    sr_portal_aabb_corners(wmin, wmax, c);

    float rx0 = 0.0f, ry0 = 0.0f, rx1 = 0.0f, ry1 = 0.0f, zmin = 0.0f;
    for (int i = 0; i < 8; ++i) {
        if (!sr_portal_project(&set->viewproj, c[i][0], c[i][1], c[i][2],
                               &nx[i], &ny[i], &nz[i]))
            return false;   /* straddles the eye plane -- prove nothing */
        if (i == 0) {
            rx0 = rx1 = nx[0]; ry0 = ry1 = ny[0]; zmin = nz[0];
        } else {
            if (nx[i] < rx0) rx0 = nx[i];
            if (nx[i] > rx1) rx1 = nx[i];
            if (ny[i] < ry0) ry0 = ny[i];
            if (ny[i] > ry1) ry1 = ny[i];
            if (nz[i] < zmin) zmin = nz[i];
        }
    }

    for (int b = 0; b < set->count; ++b) {
        const SrPortalBox *pb = &set->box[b];
        /* Two cheap rejects before any per-face work: in front of the portal,
         * or outside its screen footprint entirely. */
        if (zmin <= pb->znear) continue;
        if (rx0 < pb->rx0 || rx1 > pb->rx1 ||
            ry0 < pb->ry0 || ry1 > pb->ry1) continue;

        for (int f = 0; f < 6; ++f) {
            const SrPortalFace *fa = &pb->faces[f];
            if (!fa->usable || zmin <= fa->zmax) continue;
            int inside = 0;
            for (int i = 0; i < 8; ++i) {
                if (!sr_portal_in_quad(fa->qx, fa->qy, nx[i], ny[i])) break;
                ++inside;
            }
            if (inside == 8) return true;
        }
    }
    return false;
}

bool sr_portal_occludes(const JceSceneRenderer *sr, jce_vec3 wmin, jce_vec3 wmax)
{
    return sr ? sr_portal_set_occludes(sr->portals, wmin, wmax) : false;
}

void sr_portal_free(JceSceneRenderer *sr)
{
    if (!sr) return;
    sr_portal_set_destroy(sr->portals);
    sr->portals = NULL;
}
