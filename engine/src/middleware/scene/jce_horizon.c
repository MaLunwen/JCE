/*
 * jce_horizon.c -- see jce_horizon.h.
 */

#include "jce_horizon.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define JCE_HORIZON_TWO_PI 6.283185307179586476925286766559

/* Above this the slice is closed for all practical purposes (cos^2 = 1e-12);
 * clamping here is what keeps s*s from overflowing to inf on absurd input. */
#define JCE_HORIZON_MAX_SLOPE 1.0e6f

/* 1/sqrt(2): the cone-centre angle is theta/2 + pi/4, so the half-angle terms
 * come back through a 45-degree rotation. */
#define JCE_HORIZON_SQRT1_2 0.707106781186547524f

/* Rejects inf and NaN cell sizes along with the <= 0 cases. */
#define JCE_HORIZON_MAX_CELL 1.0e30f

typedef struct {
    float d; /* signed distance along the sweep direction, world units */
    float y; /* terrain height at that sample */
} JceHorizonPt;

typedef struct {
    const float *heights;
    int          w, h;
    float        csx, csz;
    float        inv_dirs;
    float       *vis;
    float       *bent;
    JceHorizonPt *stack; /* capacity JCE_HORIZON_MAX_DIM, owned by the caller frame */
} JceHorizonCtx;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * floorf/ceilf clamped into [lo,hi].  Every float -> int conversion in the
 * sweep goes through these: a sweep-line range is derived by dividing by the
 * shear slope, which can be arbitrarily small, so the quotient can be inf and
 * a bare cast would be undefined.  NaN collapses to lo.
 */
static int jce_horizon_floor_clamp(float t, int lo, int hi)
{
    float f = floorf(t);
    if (!(f > (float)lo)) return lo;
    if (f >= (float)hi)   return hi;
    return (int)f;
}

static int jce_horizon_ceil_clamp(float t, int lo, int hi)
{
    float f = ceilf(t);
    if (!(f > (float)lo)) return lo;
    if (f >= (float)hi)   return hi;
    return (int)f;
}

/*
 * Minor-axis offset of a sweep line at major index a, as an integer shear.
 * Round-half-up is monotone in a, and monotone shear is exactly what makes the
 * line family a PARTITION of the grid: for a fixed a the map line -> minor
 * index is a bijection, so every cell lies on exactly one line and is visited
 * exactly once per direction.  Accumulating without a per-cell visit counter
 * (there is no scratch buffer to hold one) depends on that property.
 */
static int jce_horizon_shear(float slope, int a)
{
    /* |slope| <= 1 and a < JCE_HORIZON_MAX_DIM, so the cast cannot overflow. */
    return (int)floorf(slope * (float)a + 0.5f);
}

/* Elevation slope from a sample at (d0,y0) to a candidate further along the
 * sweep.  dd > 0 holds by construction; the guard also absorbs a NaN height. */
static float jce_horizon_slope(float d0, float y0, const JceHorizonPt *p)
{
    float dd = p->d - d0;
    if (!(dd > 0.0f)) return 0.0f;
    return (p->y - y0) / dd;
}

/*
 * Fold one azimuth slice into the running sums for a cell.
 * horizon_slope may be negative (nothing blocks above the local horizontal),
 * infinite or NaN; everything collapses into [0, JCE_HORIZON_MAX_SLOPE] first
 * so the outputs stay finite for any input.
 */
static void jce_horizon_accumulate(const JceHorizonCtx *c,
                                   float horizon_slope,
                                   float ux, float uz,
                                   size_t cell)
{
    float s = horizon_slope;
    float v;

    if (!(s > 0.0f)) s = 0.0f;                   /* also maps NaN to "open" */
    if (s > JCE_HORIZON_MAX_SLOPE) s = JCE_HORIZON_MAX_SLOPE;

    /* cos^2(atan(s)) in closed form: the cosine-weighted fraction of this
     * slice that stays open.  No atan, so no transcendental in the hot loop. */
    v = 1.0f / (1.0f + s * s);

    if (c->vis) c->vis[cell] += v * c->inv_dirs;

    if (c->bent) {
        /* Centre of the open cone [theta, pi/2] is at mid = theta/2 + pi/4.
         * Half-angle identities from cos(theta) keep this at two sqrts. */
        float co  = sqrtf(v);                     /* cos(theta)   */
        float hc  = sqrtf(0.5f * (1.0f + co));    /* cos(theta/2) */
        float t   = 0.5f * (1.0f - co);
        float hs  = (t > 0.0f) ? sqrtf(t) : 0.0f; /* sin(theta/2) */
        float cm  = JCE_HORIZON_SQRT1_2 * (hc - hs);
        float sm  = JCE_HORIZON_SQRT1_2 * (hc + hs);
        size_t b  = cell * 3u;

        /* Weighted by the slice's own openness: an unweighted sum lets a
         * fully blocked slice vote for its own cone centre with full strength,
         * which is the opposite of what a bent normal is for. */
        c->bent[b + 0] += v * ux * cm;
        c->bent[b + 1] += v * sm;
        c->bent[b + 2] += v * uz * cm;
    }
}

/* ------------------------------------------------------------------ */
/* One azimuth direction                                               */
/* ------------------------------------------------------------------ */

static void jce_horizon_sweep(const JceHorizonCtx *c, float ux, float uz)
{
    /* Index-space direction: advancing one world unit along u moves the X
     * index by ux/cell_size_x, so the dominant GRID axis is not simply the
     * dominant world axis when the cell is not square. */
    float gx = ux / c->csx;
    float gz = uz / c->csz;
    bool  x_major = (fabsf(gx) >= fabsf(gz));

    int   M    = x_major ? c->w : c->h;  /* samples along one sweep line */
    int   Mi   = x_major ? c->h : c->w;  /* unsheared line count         */
    float gmaj = x_major ? gx : gz;
    float gmin = x_major ? gz : gx;
    float umaj = x_major ? ux : uz;
    float slope;
    int   o_end, omin, omax, b, step;

    /* |slope| <= 1 by choice of major axis; forced, not assumed, because the
     * partition argument in jce_horizon_shear() and the marching direction
     * below both break if a denormal divisor lets it drift past 1. */
    slope = gmin / gmaj;
    if (!(slope > -1.0f && slope < 1.0f)) slope = (slope < 0.0f) ? -1.0f : 1.0f;

    /* Shear is monotone, so its extremes sit at the two ends of the line. */
    o_end = jce_horizon_shear(slope, M - 1);
    omin  = (o_end < 0) ? o_end : 0;
    omax  = (o_end > 0) ? o_end : 0;

    /* d grows with the major index exactly when umaj > 0 (the perpendicular
     * jitter of the discrete line always shares that sign), so walking away
     * from the horizon direction means stepping against umaj. */
    step = (umaj > 0.0f) ? -1 : 1;

    for (b = -omax; b <= Mi - 1 - omin; ++b) {
        int a_lo = 0, a_hi = M - 1;
        int a, a_start, a_end, top;

        if (slope != 0.0f) {
            /* |shear(a) - slope*a| <= 0.5, so the cells of this line that fall
             * inside the grid all satisfy slope*a in [L,R].  One extra cell of
             * slack each side covers the rounding of the division itself; the
             * per-cell test below is the exact one, so a superset is enough
             * and an undersized range would silently drop cells. */
            float L  = (float)(-b) - 0.5f;
            float R  = (float)(Mi - 1 - b) + 0.5f;
            float t0 = L / slope;
            float t1 = R / slope;
            if (t0 > t1) { float tmp = t0; t0 = t1; t1 = tmp; }
            a_lo = jce_horizon_floor_clamp(t0 - 1.0f, 0, M - 1);
            a_hi = jce_horizon_ceil_clamp (t1 + 1.0f, 0, M - 1);
        }
        if (a_lo > a_hi) continue;

        a_start = (step < 0) ? a_hi : a_lo;
        a_end   = (step < 0) ? a_lo : a_hi;

        top = 0;
        for (a = a_start; ; a += step) {
            int m = b + jce_horizon_shear(slope, a);
            if (m >= 0 && m < Mi) {
                int    x = x_major ? a : m;
                int    z = x_major ? m : a;
                size_t cell = (size_t)z * (size_t)c->w + (size_t)x;
                float  y = c->heights[cell];
                /* Absolute projection rather than an accumulated step: no
                 * drift, and identical for a cell however it is reached. */
                float  d = (float)x * c->csx * ux + (float)z * c->csz * uz;
                float  hs;

                /* The hull is concave, so the elevation seen from this sample
                 * rises then falls along it: pop while the next vertex down is
                 * at least as high and the top is the maximum.  That same test
                 * is the convexity test for inserting this sample, so one pop
                 * loop serves both and each sample is pushed/popped once. */
                while (top >= 2) {
                    float s1 = jce_horizon_slope(d, y, &c->stack[top - 1]);
                    float s2 = jce_horizon_slope(d, y, &c->stack[top - 2]);
                    if (s1 <= s2) --top; else break;
                }

                hs = (top >= 1) ? jce_horizon_slope(d, y, &c->stack[top - 1])
                                : 0.0f; /* nothing ahead: open to the horizon */
                jce_horizon_accumulate(c, hs, ux, uz, cell);

                /* A line holds at most M <= JCE_HORIZON_MAX_DIM samples, so
                 * the bound below is unreachable; it is kept so a future
                 * change to the line generator cannot turn into a smash. */
                if (top < (int)JCE_HORIZON_MAX_DIM) {
                    c->stack[top].d = d;
                    c->stack[top].y = y;
                    ++top;
                }
            }
            if (a == a_end) break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

bool jce_horizon_bake(const JceHorizonDesc *desc,
                      float *out_visibility,
                      float *out_bent_normals)
{
    JceHorizonCtx ctx;
    /* ~32 KB.  Fixed because the module allocates nothing and the desc carries
     * no scratch; the dimension cap in the header is the price of that. */
    JceHorizonPt  stack[JCE_HORIZON_MAX_DIM];
    uint32_t      ndirs, di;
    size_t        cells, i;

    if (!desc || !desc->heights) return false;
    if (!out_visibility && !out_bent_normals) return false;
    if (desc->w == 0u || desc->h == 0u) return false;
    /* Validated before the clears below: an over-large extent would otherwise
     * be written into caller buffers that were sized for the real one. */
    if (desc->w > JCE_HORIZON_MAX_DIM || desc->h > JCE_HORIZON_MAX_DIM) return false;
    if (!(desc->cell_size_x > 0.0f) || !(desc->cell_size_x < JCE_HORIZON_MAX_CELL)) return false;
    if (!(desc->cell_size_z > 0.0f) || !(desc->cell_size_z < JCE_HORIZON_MAX_CELL)) return false;

    ndirs = desc->directions ? desc->directions : JCE_HORIZON_DEFAULT_DIRECTIONS;
    if (ndirs < JCE_HORIZON_MIN_DIRECTIONS) ndirs = JCE_HORIZON_MIN_DIRECTIONS;
    if (ndirs > JCE_HORIZON_MAX_DIRECTIONS) ndirs = JCE_HORIZON_MAX_DIRECTIONS;

    cells = (size_t)desc->w * (size_t)desc->h;
    if (out_visibility)   memset(out_visibility,   0, cells * sizeof(float));
    if (out_bent_normals) memset(out_bent_normals, 0, cells * 3u * sizeof(float));

    ctx.heights  = desc->heights;
    ctx.w        = (int)desc->w;
    ctx.h        = (int)desc->h;
    ctx.csx      = desc->cell_size_x;
    ctx.csz      = desc->cell_size_z;
    ctx.inv_dirs = 1.0f / (float)ndirs;
    ctx.vis      = out_visibility;
    ctx.bent     = out_bent_normals;
    ctx.stack    = stack;

    for (di = 0; di < ndirs; ++di) {
        /* Angle in double so the direction set is symmetric to the last bit
         * it can be; the sweep itself is float. */
        double phi = (JCE_HORIZON_TWO_PI * (double)di) / (double)ndirs;
        jce_horizon_sweep(&ctx, (float)cos(phi), (float)sin(phi));
    }

    if (out_visibility) {
        for (i = 0; i < cells; ++i) {
            float v = out_visibility[i];
            if (!(v >= 0.0f))     v = 0.0f;  /* NaN-safe */
            else if (v > 1.0f)    v = 1.0f;  /* summation round-off only */
            out_visibility[i] = v;
        }
    }

    if (out_bent_normals) {
        for (i = 0; i < cells; ++i) {
            float *n = &out_bent_normals[i * 3u];
            float len2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
            /* y is a positive weighted sum of sin(mid) >= sin(pi/4), so this
             * only fires if every slice underflowed to zero weight. */
            if (len2 > 1.0e-20f) {
                float inv = 1.0f / sqrtf(len2);
                n[0] *= inv; n[1] *= inv; n[2] *= inv;
            } else {
                n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f;
            }
        }
    }

    return true;
}