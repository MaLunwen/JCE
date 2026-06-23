/*
 * jce_fracture.c  Voronoi shatter of a convex box (pure CPU geometry).
 *
 * See jce_fracture.h for the algorithm and the determinism contract.  This
 * translation unit depends ONLY on the C math library, the engine allocator
 * (jce_malloc / jce_free), and its own public header — NO Bullet, NO flecs,
 * NO scene.  It is the reusable, headless-testable kernel behind the runtime
 * fracture body-swap.
 *
 * Internals use `double` for the linear solve / inside tests / volume so the
 * box-shatter is numerically robust; the OUTPUT (cell verts/centroid/volume)
 * is `float` to match the physics ABI.
 */

#include <jce/middleware/physics/jce_fracture.h>
#include <jce/os/core/jce_alloc.h>

#include <math.h>
#include <stdbool.h>
#include <string.h>

/* Geometry epsilons (in source units; the box-shatter operates on metre-scale
 * shapes).  EPS_SINGULAR rejects near-parallel plane triples; EPS_INSIDE is
 * the slack on the "point satisfies every half-space" test; EPS_MERGE folds
 * coincident vertices (a box corner is shared by 3 face planes -> the same
 * point is enumerated from several triples). */
#define JF_EPS_SINGULAR 1e-9
#define JF_EPS_INSIDE   1e-6
#define JF_EPS_MERGE    1e-5

/* A half-space: the set of points p with dot(n, p) <= d  (i.e. n is the
 * OUTWARD normal and d = dot(n, point_on_plane)).  "Inside" means on the
 * non-normal side, with EPS_INSIDE slack. */
typedef struct {
    double n[3];
    double d;
} JfPlane;

/* Generous upper bound on the half-space count for one box cell: 6 box faces
 * + one bisector per OTHER seed.  The runtime caps fragment_count well under
 * this, but clamp defensively so a pathological seed count can't overflow. */
#define JF_MAX_PLANES 256

/* ── small vector helpers (double) ──────────────────────────────────────── */

static double jf_dot(const double a[3], const double b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void jf_cross(const double a[3], const double b[3], double out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/* Build an outward-normal half-space from a normal and a point on the plane.
 * The normal is normalized so EPS thresholds have consistent scale. */
static JfPlane jf_plane_from_point_normal(const double n[3], const double p[3])
{
    JfPlane pl;
    double len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len < JF_EPS_SINGULAR) len = 1.0;
    pl.n[0] = n[0] / len;
    pl.n[1] = n[1] / len;
    pl.n[2] = n[2] / len;
    pl.d    = jf_dot(pl.n, p);
    return pl;
}

/*
 * Solve the 3x3 system for the common point of three planes:
 *   n0.p = d0,  n1.p = d1,  n2.p = d2
 * via Cramer's rule.  Returns false (skip) when the triple is near-singular
 * (parallel / collinear normals).
 */
static bool jf_intersect_three(const JfPlane *a, const JfPlane *b,
                               const JfPlane *c, double out[3])
{
    double bc[3], ca[3], ab[3];
    jf_cross(b->n, c->n, bc);
    double det = jf_dot(a->n, bc);
    if (fabs(det) < JF_EPS_SINGULAR) return false;

    jf_cross(c->n, a->n, ca);
    jf_cross(a->n, b->n, ab);

    /* p = (d0*(n1×n2) + d1*(n2×n0) + d2*(n0×n1)) / det */
    double inv = 1.0 / det;
    out[0] = (a->d * bc[0] + b->d * ca[0] + c->d * ab[0]) * inv;
    out[1] = (a->d * bc[1] + b->d * ca[1] + c->d * ab[1]) * inv;
    out[2] = (a->d * bc[2] + b->d * ca[2] + c->d * ab[2]) * inv;
    return true;
}

/* True if `p` is inside EVERY half-space (with slack). */
static bool jf_point_inside_all(const double p[3], const JfPlane *planes,
                                int nplanes)
{
    for (int i = 0; i < nplanes; ++i) {
        if (jf_dot(planes[i].n, p) > planes[i].d + JF_EPS_INSIDE)
            return false;
    }
    return true;
}

/*
 * Enumerate the vertices of the convex cell bounded by `planes`: for every
 * plane triple, solve for the intersection point and keep it iff it satisfies
 * all half-spaces and is not a duplicate of an already-kept vertex.  Writes up
 * to JCE_FRACTURE_MAX_CELL_VERTS deduped vertices into `out`; returns the
 * count.  Deterministic: the (i<j<k) triple order and the append order are
 * fixed, so a given plane set always yields the same vertex list.
 */
static int jf_cell_vertices(const JfPlane *planes, int nplanes,
                            double out[][3])
{
    int count = 0;
    for (int i = 0; i < nplanes; ++i) {
        for (int j = i + 1; j < nplanes; ++j) {
            for (int k = j + 1; k < nplanes; ++k) {
                double p[3];
                if (!jf_intersect_three(&planes[i], &planes[j], &planes[k], p))
                    continue;
                if (!jf_point_inside_all(p, planes, nplanes))
                    continue;
                /* dedup against kept vertices (epsilon merge) */
                bool dup = false;
                for (int v = 0; v < count; ++v) {
                    double dx = out[v][0] - p[0];
                    double dy = out[v][1] - p[1];
                    double dz = out[v][2] - p[2];
                    if (dx * dx + dy * dy + dz * dz < JF_EPS_MERGE * JF_EPS_MERGE) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;
                if (count >= JCE_FRACTURE_MAX_CELL_VERTS)
                    return count;   /* cap reached — drop the rest */
                out[count][0] = p[0];
                out[count][1] = p[1];
                out[count][2] = p[2];
                count++;
            }
        }
    }
    return count;
}

/*
 * Volume of the convex cell via tetrahedral decomposition from the cell
 * centroid, face by face.  The cell's faces are exactly its bounding planes:
 * for each plane we gather the vertices lying ON it (within EPS) into a face
 * polygon, order them CCW about the plane normal, fan-triangulate the polygon,
 * and sum the signed volumes of the tetrahedra (centroid, a, b, c).  Because
 * the cell is convex with planar faces (true for box / Voronoi cells), the
 * absolute total is the exact cell volume.  Returns 0 for fewer than 4 verts.
 */
static double jf_cell_volume(double verts[][3], int vcount,
                             const JfPlane *planes, int nplanes,
                             const double centroid[3])
{
    if (vcount < 4) return 0.0;
    double vol = 0.0;

    for (int pi = 0; pi < nplanes; ++pi) {
        const JfPlane *pl = &planes[pi];

        /* Collect the vertices lying on this plane (a face polygon). */
        int face[JCE_FRACTURE_MAX_CELL_VERTS];
        int fc = 0;
        for (int v = 0; v < vcount; ++v) {
            double dist = jf_dot(pl->n, verts[v]) - pl->d;
            if (fabs(dist) <= JF_EPS_MERGE) {
                if (fc < JCE_FRACTURE_MAX_CELL_VERTS)
                    face[fc++] = v;
            }
        }
        if (fc < 3) continue;   /* not a real face */

        /* Order the face vertices CCW about the plane normal so the fan
         * triangulation is consistent (sort by angle around the face
         * centroid in the plane's tangent frame). */
        double fcen[3] = { 0, 0, 0 };
        for (int i = 0; i < fc; ++i) {
            fcen[0] += verts[face[i]][0];
            fcen[1] += verts[face[i]][1];
            fcen[2] += verts[face[i]][2];
        }
        fcen[0] /= fc; fcen[1] /= fc; fcen[2] /= fc;

        /* Tangent basis (u,w) in the plane. */
        double up[3] = { 0, 0, 0 };
        /* pick an axis least aligned with the normal for a stable tangent */
        double ax = fabs(pl->n[0]), ay = fabs(pl->n[1]), az = fabs(pl->n[2]);
        if (ax <= ay && ax <= az)      { up[0] = 1; }
        else if (ay <= az)             { up[1] = 1; }
        else                           { up[2] = 1; }
        double u[3], w[3];
        jf_cross(pl->n, up, u);
        double ulen = sqrt(jf_dot(u, u));
        if (ulen < JF_EPS_SINGULAR) continue;
        u[0] /= ulen; u[1] /= ulen; u[2] /= ulen;
        jf_cross(pl->n, u, w);

        /* angle of each face vertex about fcen */
        double ang[JCE_FRACTURE_MAX_CELL_VERTS];
        for (int i = 0; i < fc; ++i) {
            double r[3] = { verts[face[i]][0] - fcen[0],
                            verts[face[i]][1] - fcen[1],
                            verts[face[i]][2] - fcen[2] };
            ang[i] = atan2(jf_dot(r, w), jf_dot(r, u));
        }
        /* insertion sort face[] by ang[] (small fc) */
        for (int i = 1; i < fc; ++i) {
            double ka = ang[i];
            int    kf = face[i];
            int    j  = i - 1;
            while (j >= 0 && ang[j] > ka) {
                ang[j + 1]  = ang[j];
                face[j + 1] = face[j];
                j--;
            }
            ang[j + 1]  = ka;
            face[j + 1] = kf;
        }

        /* Fan-triangulate the face and add tetra volumes from the cell
         * centroid. Each tetra volume = (1/6)|(a-c0).((b-c0)x(p-c0))|; we use
         * the signed form and take the magnitude at the end. */
        for (int t = 1; t + 1 < fc; ++t) {
            const double *a = verts[face[0]];
            const double *b = verts[face[t]];
            const double *c = verts[face[t + 1]];
            double e1[3] = { a[0] - centroid[0], a[1] - centroid[1], a[2] - centroid[2] };
            double e2[3] = { b[0] - centroid[0], b[1] - centroid[1], b[2] - centroid[2] };
            double e3[3] = { c[0] - centroid[0], c[1] - centroid[1], c[2] - centroid[2] };
            double cx[3];
            jf_cross(e2, e3, cx);
            vol += jf_dot(e1, cx);   /* 6 * signed tetra volume */
        }
    }
    return fabs(vol) / 6.0;
}

/* ── public API ─────────────────────────────────────────────────────────── */

int JCE_CALL jce_fracture_box(const float aabb_min[3], const float aabb_max[3],
                              const float *seed_points, int n_seeds,
                              JceFractureResult *out)
{
    if (!aabb_min || !aabb_max || !out) return 0;
    memset(out, 0, sizeof(*out));

    /* Box bounds (double) — guard against an inverted/degenerate box. */
    double bmin[3], bmax[3];
    for (int a = 0; a < 3; ++a) {
        bmin[a] = (double)aabb_min[a];
        bmax[a] = (double)aabb_max[a];
        if (bmax[a] <= bmin[a]) return 0;   /* degenerate axis */
    }

    /* The 6 box face half-spaces (outward normals). */
    JfPlane box_planes[6];
    {
        double pmin[3] = { bmin[0], bmin[1], bmin[2] };
        double pmax[3] = { bmax[0], bmax[1], bmax[2] };
        double nx_p[3] = { 1, 0, 0 },  nx_m[3] = { -1, 0, 0 };
        double ny_p[3] = { 0, 1, 0 },  ny_m[3] = { 0, -1, 0 };
        double nz_p[3] = { 0, 0, 1 },  nz_m[3] = { 0, 0, -1 };
        box_planes[0] = jf_plane_from_point_normal(nx_p, pmax);
        box_planes[1] = jf_plane_from_point_normal(nx_m, pmin);
        box_planes[2] = jf_plane_from_point_normal(ny_p, pmax);
        box_planes[3] = jf_plane_from_point_normal(ny_m, pmin);
        box_planes[4] = jf_plane_from_point_normal(nz_p, pmax);
        box_planes[5] = jf_plane_from_point_normal(nz_m, pmin);
    }

    /* n_seeds <= 1  ->  a single cell that is the whole box. */
    int cells_n = (n_seeds <= 1 || !seed_points) ? 1 : n_seeds;

    JceFractureCell *cells =
        (JceFractureCell *)jce_malloc((size_t)cells_n * sizeof(JceFractureCell));
    if (!cells) return 0;
    memset(cells, 0, (size_t)cells_n * sizeof(JceFractureCell));

    /* Seeds clamped into the box (a seed outside would otherwise give an
     * empty / inverted cell). */
    int produced = 0;
    for (int i = 0; i < cells_n; ++i) {
        double si[3];
        if (n_seeds <= 1 || !seed_points) {
            /* whole-box cell: centre seed (only used for the cell volume fan) */
            si[0] = 0.5 * (bmin[0] + bmax[0]);
            si[1] = 0.5 * (bmin[1] + bmax[1]);
            si[2] = 0.5 * (bmin[2] + bmax[2]);
        } else {
            for (int a = 0; a < 3; ++a) {
                double v = (double)seed_points[i * 3 + a];
                if (v < bmin[a]) v = bmin[a];
                if (v > bmax[a]) v = bmax[a];
                si[a] = v;
            }
        }

        /* Build this cell's half-space set: 6 box planes + bisectors. */
        JfPlane planes[JF_MAX_PLANES];
        int np = 0;
        for (int b = 0; b < 6; ++b) planes[np++] = box_planes[b];

        if (n_seeds > 1 && seed_points) {
            for (int j = 0; j < n_seeds && np < JF_MAX_PLANES; ++j) {
                if (j == i) continue;
                double sj[3];
                for (int a = 0; a < 3; ++a) {
                    double v = (double)seed_points[j * 3 + a];
                    if (v < bmin[a]) v = bmin[a];
                    if (v > bmax[a]) v = bmax[a];
                    sj[a] = v;
                }
                /* bisector: outward normal toward sj, through the midpoint */
                double nrm[3] = { sj[0] - si[0], sj[1] - si[1], sj[2] - si[2] };
                double nl = sqrt(jf_dot(nrm, nrm));
                if (nl < JF_EPS_SINGULAR) continue;   /* coincident seeds */
                double mid[3] = { 0.5 * (si[0] + sj[0]),
                                  0.5 * (si[1] + sj[1]),
                                  0.5 * (si[2] + sj[2]) };
                planes[np++] = jf_plane_from_point_normal(nrm, mid);
            }
        }

        /* Enumerate vertices of the cell. */
        double raw[JCE_FRACTURE_MAX_CELL_VERTS][3];
        int vc = jf_cell_vertices(planes, np, raw);

        JceFractureCell *cell = &cells[produced];
        cell->vcount = 0;
        cell->volume = 0.0f;
        cell->centroid[0] = cell->centroid[1] = cell->centroid[2] = 0.0f;

        if (vc >= 4) {
            double cen[3] = { 0, 0, 0 };
            for (int v = 0; v < vc; ++v) {
                cen[0] += raw[v][0];
                cen[1] += raw[v][1];
                cen[2] += raw[v][2];
            }
            cen[0] /= vc; cen[1] /= vc; cen[2] /= vc;

            double vol = jf_cell_volume(raw, vc, planes, np, cen);

            for (int v = 0; v < vc; ++v) {
                cell->verts[v][0] = (float)raw[v][0];
                cell->verts[v][1] = (float)raw[v][1];
                cell->verts[v][2] = (float)raw[v][2];
            }
            cell->vcount      = vc;
            cell->centroid[0] = (float)cen[0];
            cell->centroid[1] = (float)cen[1];
            cell->centroid[2] = (float)cen[2];
            cell->volume      = (float)vol;
        }
        produced++;
    }

    out->cells      = cells;
    out->cell_count = produced;
    return produced;
}

void JCE_CALL jce_fracture_free(JceFractureResult *r)
{
    if (!r) return;
    if (r->cells) jce_free(r->cells);
    r->cells      = NULL;
    r->cell_count = 0;
}

/*
 * Deterministic interior seed scatter.  A tiny xorshift32 PRNG (no global
 * state, fully reproducible from the (seed) argument) places each point in
 * the box with a small inset so faces are not seeded (which would create
 * zero-volume sliver cells).
 */
int JCE_CALL jce_fracture_scatter_seeds(const float aabb_min[3],
                                        const float aabb_max[3],
                                        int n, uint32_t seed, float *out_seeds)
{
    if (!aabb_min || !aabb_max || !out_seeds || n <= 0) return 0;

    uint32_t state = seed ? seed : 0x9E3779B9u;   /* non-zero state */

    for (int i = 0; i < n; ++i) {
        for (int a = 0; a < 3; ++a) {
            /* xorshift32 */
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            float t = (float)(state & 0xFFFFFFu) / (float)0x1000000u; /* [0,1) */
            float lo = aabb_min[a];
            float hi = aabb_max[a];
            float ext = hi - lo;
            /* 10% inset on each side keeps seeds strictly interior */
            float inset = 0.1f * ext;
            out_seeds[i * 3 + a] = lo + inset + t * (ext - 2.0f * inset);
        }
    }
    return n;
}
