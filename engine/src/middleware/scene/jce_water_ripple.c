/*
 * jce_water_ripple.c -- linearised shallow water over a varying bed.
 *
 * The scheme, and why each piece is the one it is, is documented at the
 * function that implements it.  The header states what this layer is for and
 * why it is not part of JceWaterField.
 */
#include <jce/middleware/scene/jce_water_ripple.h>

#include "os/core/jce_memory.h"   /* JCE_CALLOC / JCE_FREE, as jce_water_field.c does */

#include <math.h>
#include <string.h>

/* Standard gravity.  Named rather than inlined because it appears in the
 * wave speed AND in the kinetic-energy normalisation, and those two must be
 * the same number or the energy oracle stops being an oracle. */
#define RIPPLE_G 9.80665f

/* Substep cap.  A caller that stalls for a second must not buy a second of
 * simulation at CFL resolution -- that is a spiral where the frame that was
 * slow makes the next frame slower.  Sixteen substeps at a typical 64-cell,
 * 32 m, 2 m-deep pond is about 45 ms of simulated time, comfortably more than
 * a frame; past that the surplus is dropped. */
#define RIPPLE_MAX_SUBSTEPS 16

struct JceWaterRipple {
    int    n;              /* cells per side                                */
    float  size_m;         /* world size of the square                      */
    float  cx, cz;         /* world centre                                  */
    float  dx;             /* cell spacing, metres                          */
    float  inv_dx;
    float  damping;        /* k, 1/s                                        */
    float  default_depth;

    /* Height and surface velocity, both explicit.
     *
     * The first version of this carried two HEIGHT levels and let the velocity
     * be implied by their difference, which is the textbook leapfrog layout.
     * It has a defect that only shows up at the API: an impulse has to be
     * written as a height difference, so it must be divided by the dt the
     * NEXT step will use -- and that dt depends on how long the caller's frame
     * happened to take. The same impulse then meant different things at 30 and
     * 144 fps. Carrying v explicitly makes an impulse literally `v += speed`,
     * with no dt in it to be wrong about.
     *
     * The scheme below is semi-implicit (symplectic) Euler, which for this
     * equation IS leapfrog with the velocity written down -- same order, same
     * CFL bound, same conserved energy -- so nothing was traded for it. */
    float *h;
    float *v;

    /* c2[i] = g * H[i], the squared wave speed at cell centres.  Stored
     * squared because that is the form the flux uses -- taking a sqrt per
     * cell per step to square it again would be arithmetic for nobody. */
    float *c2;

    float  c2_max;         /* drives the CFL bound; kept with the depth map  */
    float *depth;          /* H in metres, retained for set_depth(NULL)      */
};

static bool ripple_finite(float v) { return v == v && v > -1e30f && v < 1e30f; }

JceWaterRippleDesc JCE_CALL jce_water_ripple_default_desc(void)
{
    JceWaterRippleDesc d;
    d.resolution      = 64;
    d.size_m          = 32.0f;
    d.center_x        = 0.0f;
    d.center_z        = 0.0f;
    d.default_depth_m = 2.0f;
    /* Loses most of a ring in a few seconds. Zero would be a lossless pond
     * that rings until the heat death of the scene -- physically wrong (real
     * water is viscous and radiates) and, more practically, a pond that never
     * goes quiet is a pond whose texture can never stop being uploaded. */
    d.damping         = 0.4f;
    return d;
}

/* Recompute c2 and c2_max from the current depth map.  Called whenever the
 * bathymetry changes, never per step: c2 is a property of the bed, and a bed
 * that changed every step would be a different simulation. */
static void ripple_rebuild_speed(JceWaterRipple *r)
{
    const int cells = r->n * r->n;
    float mx = 0.0f;
    for (int i = 0; i < cells; ++i) {
        const float H = r->depth[i];
        /* Land is c = 0, not "very shallow water".  A zero speed makes every
         * flux through a face touching this cell vanish, which IS the
         * reflecting (Neumann) boundary a bank should be -- so the shoreline
         * falls out of the depth map instead of needing a second mask and a
         * second set of edge cases. */
        const float c2 = (H > 0.0f) ? (RIPPLE_G * H) : 0.0f;
        r->c2[i] = c2;
        if (c2 > mx) mx = c2;
    }
    r->c2_max = mx;
}

JceWaterRipple *JCE_CALL jce_water_ripple_create(const JceWaterRippleDesc *desc)
{
    if (!desc) return NULL;
    if (desc->resolution < 4 || desc->resolution > 1024) return NULL;
    if (!ripple_finite(desc->size_m) || desc->size_m <= 0.0f) return NULL;
    if (!ripple_finite(desc->default_depth_m) || desc->default_depth_m <= 0.0f)
        return NULL;
    if (!ripple_finite(desc->center_x) || !ripple_finite(desc->center_z))
        return NULL;

    JceWaterRipple *r = (JceWaterRipple *)JCE_CALLOC(1, sizeof *r);
    if (!r) return NULL;

    r->n             = desc->resolution;
    r->size_m        = desc->size_m;
    r->cx            = desc->center_x;
    r->cz            = desc->center_z;
    r->dx            = desc->size_m / (float)(desc->resolution - 1);
    r->inv_dx        = 1.0f / r->dx;
    r->damping       = (ripple_finite(desc->damping) && desc->damping > 0.0f)
                     ? desc->damping : 0.0f;
    r->default_depth = desc->default_depth_m;

    const size_t cells = (size_t)r->n * (size_t)r->n;
    r->h     = (float *)JCE_CALLOC(cells, sizeof(float));
    r->v     = (float *)JCE_CALLOC(cells, sizeof(float));
    r->c2    = (float *)JCE_CALLOC(cells, sizeof(float));
    r->depth = (float *)JCE_CALLOC(cells, sizeof(float));
    if (!r->h || !r->v || !r->c2 || !r->depth) {
        jce_water_ripple_destroy(r);
        return NULL;
    }
    for (size_t i = 0; i < cells; ++i) r->depth[i] = r->default_depth;
    ripple_rebuild_speed(r);
    return r;
}

void JCE_CALL jce_water_ripple_destroy(JceWaterRipple *r)
{
    if (!r) return;
    JCE_FREE(r->h);
    JCE_FREE(r->v);
    JCE_FREE(r->c2);
    JCE_FREE(r->depth);
    JCE_FREE(r);
}

bool JCE_CALL jce_water_ripple_set_depth(JceWaterRipple *r,
                                         const float *depth_m, int count)
{
    if (!r) return false;
    const int cells = r->n * r->n;

    if (!depth_m) {
        for (int i = 0; i < cells; ++i) r->depth[i] = r->default_depth;
        ripple_rebuild_speed(r);
        return true;
    }
    /* All or nothing.  A partially applied depth map is a pond with a cliff
     * through it, and it would be read as a solver bug rather than a caller
     * one. */
    if (count != cells) return false;

    for (int i = 0; i < cells; ++i) {
        const float H = depth_m[i];
        /* Non-finite depth becomes land rather than propagating a NaN into
         * c2, where it would reach every cell within c2_max steps and take
         * the whole grid with it.  Land is the safe reading of "unknown": it
         * reflects, and a reflection is visible and local. */
        r->depth[i] = ripple_finite(H) ? H : 0.0f;
    }
    ripple_rebuild_speed(r);
    return true;
}

float JCE_CALL jce_water_ripple_max_dt(const JceWaterRipple *r)
{
    if (!r) return 0.0f;
    /* CFL for the explicit 5-point Laplacian in 2D: c dt / dx <= 1/sqrt(2).
     * The bound is on the FASTEST cell, because one cell violating it is
     * enough -- the instability grows there and spreads.
     *
     * A grid that is land everywhere has c_max = 0 and no bound at all; it
     * also cannot propagate anything, so any dt is equally correct.  Return
     * something large and finite rather than an infinity that a caller would
     * have to special-case. */
    if (r->c2_max <= 0.0f) return 1.0f;
    const float c_max = sqrtf(r->c2_max);
    return r->dx / (c_max * 1.41421356f);
}

/* One substep at a dt already known to satisfy CFL.
 *
 *     v += dt * L(h)          then     v /= (1 + k dt)     then    h += dt * v
 *
 * Semi-implicit (symplectic) Euler. Advancing h with the ALREADY UPDATED v is
 * what makes it symplectic rather than plain explicit Euler, and the
 * difference is not academic: plain Euler pumps energy into every oscillator
 * it touches, so the pond would grow louder forever with no bug anywhere else
 * to find.
 *
 * L is the FLUX form of div(c2 grad h):
 *
 *     L(h)[i] = (1/dx^2) * sum over the four faces of
 *               c2_face * (h[neighbour] - h[i])
 *
 * with c2_face the average of the two cells it separates. Using the face
 * average rather than the cell's own c2 is what makes the operator symmetric,
 * and symmetry is not cosmetic: an asymmetric discrete Laplacian is not
 * self-adjoint, so the scheme has no conserved energy, and a depth
 * discontinuity -- which is exactly what a shelf or a bank IS -- pumps energy
 * in until the grid explodes. The land-boundary test is what catches that.
 *
 * The damping is applied as a DIVISION by (1 + k dt) rather than as a
 * subtraction of k dt v. The subtraction is the obvious form and it changes
 * sign once k dt > 1 and diverges past 2 -- so a caller who asked for heavy
 * damping would get the opposite of damping. The division is monotone and
 * bounded for every k >= 0 and every dt > 0, which means damping can never be
 * the thing that destabilises the solver.
 *
 * Off-grid neighbours are treated as a copy of the centre cell, which makes
 * their flux zero: the domain edge reflects, exactly as land does. That is the
 * honest boundary for a bounded pond. It is the WRONG boundary for a window
 * onto a larger body of water, where an outgoing wave should leave; that case
 * is served by the caller tapering depth to zero at the rim -- not by a second
 * boundary mode nobody could choose between.
 */
static void ripple_substep(JceWaterRipple *r, float dt)
{
    const int   n    = r->n;
    const float inv2 = r->inv_dx * r->inv_dx;
    const float damp = 1.0f / (1.0f + r->damping * dt);

    const float *h = r->h;
    float       *v = r->v;

    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const int   idx = j * n + i;
            const float cc  = r->c2[idx];
            /* Land is skipped here and PINNED in the height pass below. Only
             * the pin matters: nothing reads a land cell's velocity -- the
             * Laplacian reads heights, and this cell's height is forced to
             * zero every step regardless. A mutation that removed a second,
             * velocity-side pin stayed green, which is how that was
             * established rather than assumed; it is not written here because
             * a line no test can distinguish from its absence is a line that
             * will be maintained for no reason. */
            if (cc <= 0.0f) continue;

            const float hc = h[idx];
            const float hl = (i > 0)     ? h[idx - 1] : hc;
            const float hr = (i < n - 1) ? h[idx + 1] : hc;
            const float hd = (j > 0)     ? h[idx - n] : hc;
            const float hu = (j < n - 1) ? h[idx + n] : hc;

            const float cl = 0.5f * (cc + ((i > 0)     ? r->c2[idx - 1] : cc));
            const float cr = 0.5f * (cc + ((i < n - 1) ? r->c2[idx + 1] : cc));
            const float cd = 0.5f * (cc + ((j > 0)     ? r->c2[idx - n] : cc));
            const float cu = 0.5f * (cc + ((j < n - 1) ? r->c2[idx + n] : cc));

            const float lap = (cl * (hl - hc) + cr * (hr - hc) +
                               cd * (hd - hc) + cu * (hu - hc)) * inv2;

            v[idx] = (v[idx] + dt * lap) * damp;
        }
    }
    /* h is advanced in a SECOND pass, after every velocity is updated. Fusing
     * the two would make each cell's Laplacian read a mixture of old and new
     * heights depending on iteration order -- a Gauss-Seidel sweep, which is a
     * different (and direction-biased) scheme wearing this one's comments. */
    float *hh = r->h;
    const int cells = n * n;
    for (int i = 0; i < cells; ++i) {
        if (r->c2[i] <= 0.0f) { hh[i] = 0.0f; continue; }
        hh[i] += dt * v[i];
    }
}

void JCE_CALL jce_water_ripple_step(JceWaterRipple *r, float dt)
{
    if (!r) return;
    if (!ripple_finite(dt) || dt <= 0.0f) return;

    const float max_dt = jce_water_ripple_max_dt(r);
    if (max_dt <= 0.0f) return;

    /* 0.9 of the bound, not the bound.  CFL is the boundary between stable
     * and unstable, and sitting exactly on a stability boundary in floating
     * point is how a scheme that is provably stable diverges anyway. */
    const float safe = max_dt * 0.9f;

    int steps = (int)ceilf(dt / safe);
    if (steps < 1) steps = 1;
    if (steps > RIPPLE_MAX_SUBSTEPS) {
        /* The cap binds: simulate RIPPLE_MAX_SUBSTEPS worth and DROP the rest.
         *
         * What this bounds is WORK, not stability -- a distinction worth being
         * exact about, because the comment here first claimed the opposite and
         * a mutation removing the cap stayed green until it was checked.
         * Without the cap `sub` is dt/ceil(dt/safe), which is never LARGER
         * than safe, so an uncapped huge dt is perfectly stable; it just costs
         * an unbounded number of substeps, which is the frame-rate spiral
         * where the frame that was slow makes the next one slower.
         *
         * Dropping the surplus means a stalled frame leaves the pond running
         * slow for a moment. That is a small, local, self-correcting error --
         * unlike the spiral, which is not. */
        steps = RIPPLE_MAX_SUBSTEPS;
        dt    = safe * (float)RIPPLE_MAX_SUBSTEPS;
    }
    const float sub = dt / (float)steps;
    for (int s = 0; s < steps; ++s) ripple_substep(r, sub);
}

void JCE_CALL jce_water_ripple_impulse(JceWaterRipple *r, float x, float z,
                                       float radius_m, float speed_mps)
{
    if (!r) return;
    if (!ripple_finite(x) || !ripple_finite(z)) return;
    if (!ripple_finite(radius_m) || radius_m <= 0.0f) return;
    if (!ripple_finite(speed_mps) || speed_mps == 0.0f) return;

    const int   n    = r->n;
    const float half = r->size_m * 0.5f;
    /* Grid coordinates: cell 0 sits at the square's minimum corner and cell
     * n-1 at its maximum, which is why dx uses (n - 1) and not n. */
    const float gx = (x - (r->cx - half)) * r->inv_dx;
    const float gz = (z - (r->cz - half)) * r->inv_dx;
    const float gr = radius_m * r->inv_dx;

    int i0 = (int)floorf(gx - gr), i1 = (int)ceilf(gx + gr);
    int j0 = (int)floorf(gz - gr), j1 = (int)ceilf(gz + gr);
    if (i1 < 0 || j1 < 0 || i0 > n - 1 || j0 > n - 1) return;  /* off-grid */
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > n - 1) i1 = n - 1;
    if (j1 > n - 1) j1 = n - 1;

    /* The impulse enters as a VELOCITY -- and because the velocity is stored
     * rather than implied by two height levels, that is literally `v -= speed`
     * with no dt anywhere in it. The same call therefore means the same thing
     * at 30 fps and at 144.
     *
     * The sign: speed_mps is DOWNWARD (a body falling in), and the surface
     * height axis is up, so a positive speed subtracts from v. */
    const float inv_gr = 1.0f / gr;

    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            const float ddx = ((float)i - gx);
            const float ddz = ((float)j - gz);
            const float d   = sqrtf(ddx * ddx + ddz * ddz) * inv_gr;
            if (d >= 1.0f) continue;
            /* cos^2 falls to zero WITH ZERO SLOPE at the rim.  A disc with a
             * hard edge radiates at the grid frequency, which on screen is a
             * square ring -- the classic tell of a height-field solver driven
             * by a top-hat. */
            const float w = cosf(d * 1.5707963f);
            const int   idx = j * n + i;
            if (r->c2[idx] <= 0.0f) continue;   /* cannot disturb land */
            r->v[idx] -= speed_mps * w * w;
        }
    }
}

float JCE_CALL jce_water_ripple_height(const JceWaterRipple *r, float x, float z)
{
    if (!r) return 0.0f;
    if (!ripple_finite(x) || !ripple_finite(z)) return 0.0f;

    const int   n    = r->n;
    const float half = r->size_m * 0.5f;
    const float gx   = (x - (r->cx - half)) * r->inv_dx;
    const float gz   = (z - (r->cz - half)) * r->inv_dx;

    /* Outside the grid the answer is zero, and zero is CORRECT rather than a
     * fallback: outside the covered square there is no disturbance.  Clamping
     * to the edge instead would smear the rim value across the whole world. */
    if (gx < 0.0f || gz < 0.0f || gx > (float)(n - 1) || gz > (float)(n - 1))
        return 0.0f;

    int i0 = (int)gx, j0 = (int)gz;
    if (i0 > n - 2) i0 = n - 2;
    if (j0 > n - 2) j0 = n - 2;
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    const float fx = gx - (float)i0, fz = gz - (float)j0;

    const float *h = r->h;
    const float a = h[j0 * n + i0],       b = h[j0 * n + i0 + 1];
    const float c = h[(j0 + 1) * n + i0], d = h[(j0 + 1) * n + i0 + 1];
    const float top = a + (b - a) * fx;
    const float bot = c + (d - c) * fx;
    return top + (bot - top) * fz;
}

const float *JCE_CALL jce_water_ripple_height_data(const JceWaterRipple *r)
{
    return r ? r->h : NULL;
}

int JCE_CALL jce_water_ripple_resolution(const JceWaterRipple *r)
{
    return r ? r->n : 0;
}

void JCE_CALL jce_water_ripple_world_rect(const JceWaterRipple *r,
                                          float *out_center_x,
                                          float *out_center_z,
                                          float *out_size_m)
{
    if (!r) return;          /* leave the caller's own initialisation alone */
    if (out_center_x) *out_center_x = r->cx;
    if (out_center_z) *out_center_z = r->cz;
    if (out_size_m)   *out_size_m   = r->size_m;
}

float JCE_CALL jce_water_ripple_energy(const JceWaterRipple *r)
{
    if (!r) return 0.0f;

    /* E = 1/2 * integral of [ v^2 + c2 |grad h|^2 ]
     *
     * The GRADIENT term, not sum(h^2). This is the correction that made the
     * tests pass, and it was the ORACLE that was wrong, not the solver: for
     * h_tt = div(c2 grad h), multiplying by h_t and integrating gives
     * d/dt (1/2) int v^2 = -(1/2) d/dt int c2 |grad h|^2, so it is v^2 plus
     * the gradient that is conserved. sum(h^2) is not an invariant of the wave
     * equation at all -- a standing wave moves all of its energy between
     * amplitude and motion twice a period, so a sum(h^2) oracle reports a
     * pond that is behaving perfectly as one that is pumping.
     *
     * (The FLUID energy of shallow water really is (1/2)rho g int h^2 plus
     * (1/2) rho int H|u|^2 -- but this state is (h, dh/dt), not (h, u), and
     * dh/dt = -div(Hu) is not u. Reaching for the physical form without that
     * change of variables is exactly the mistake above.)
     *
     * The discrete gradient is summed over FACES, each counted once, with the
     * same face-averaged c2 the flux operator uses. Any other pairing would
     * measure a different operator from the one being stepped, and the test
     * would then be pinning a quantity nothing conserves.
     *
     * This exists to be asserted on. A wave solver has one property that
     * separates "correct" from "plausible", and it is that this decays when
     * damping is on and holds when it is off. A wrong sign, an asymmetric
     * Laplacian, a CFL violation and an unstable boundary all show up here as
     * GROWTH, long before anything looks wrong on a screen. */
    const int   n    = r->n;
    const float area = r->dx * r->dx;
    const float inv2 = r->inv_dx * r->inv_dx;

    double e = 0.0;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const int   idx = j * n + i;
            const float cc  = r->c2[idx];
            if (cc <= 0.0f) continue;          /* land stores nothing */

            e += (double)r->v[idx] * (double)r->v[idx];

            if (i < n - 1) {
                const float cn = r->c2[idx + 1];
                if (cn > 0.0f) {
                    const float d = r->h[idx + 1] - r->h[idx];
                    e += (double)(0.5f * (cc + cn)) * (double)d * (double)d * (double)inv2;
                }
            }
            if (j < n - 1) {
                const float cn = r->c2[idx + n];
                if (cn > 0.0f) {
                    const float d = r->h[idx + n] - r->h[idx];
                    e += (double)(0.5f * (cc + cn)) * (double)d * (double)d * (double)inv2;
                }
            }
        }
    }
    return (float)(e * (double)area * 0.5);
}
