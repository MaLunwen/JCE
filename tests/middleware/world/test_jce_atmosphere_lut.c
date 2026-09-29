/*
 * test_jce_atmosphere_lut.c
 *
 * The tables exist so the GPU sky and the CPU lighting authority can read ONE
 * atmosphere.  The property that makes that true is not "the table looks
 * plausible" -- it is that sampling the table AGREES with evaluating the
 * integral it was baked from.  Everything else here is physics sanity that
 * would catch a table baked with its axes swapped, which is the mistake that
 * produces a beautiful and completely wrong sky.
 */

#include <jce/middleware/world/jce_atmosphere.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define TR_CELLS (JCE_ATMOSPHERE_LUT_ALTITUDE * JCE_ATMOSPHERE_LUT_COSINE)
#define MS_CELLS (JCE_ATMOSPHERE_MS_LUT_DIM * JCE_ATMOSPHERE_MS_LUT_DIM)

static float *g_tr;
static float *g_ms;
static JceAtmosphereParams g_p;

static void bake(void)
{
    g_p  = jce_atmosphere_default_params();
    g_tr = (float *)malloc(sizeof(float) * TR_CELLS * 3);
    g_ms = (float *)malloc(sizeof(float) * MS_CELLS * 3);
    TEST_ASSERT_NOT_NULL(g_tr);
    TEST_ASSERT_NOT_NULL(g_ms);
    TEST_ASSERT_TRUE(jce_atmosphere_bake_transmittance_lut(&g_p, g_tr));
    TEST_ASSERT_TRUE(jce_atmosphere_bake_multiscatter_lut(&g_p, g_ms));
}

static void unbake(void) { free(g_tr); g_tr = NULL; free(g_ms); g_ms = NULL; }

/* ── 1. THE POINT: the table agrees with the integral it came from ─────
 *
 * If this fails, the sky the player sees and the light the authority applies
 * come from two different atmospheres -- which is exactly the class of bug
 * this whole line of work exists to eliminate. */

static void test_lut_agrees_with_the_integral(void)
{
    bake();
    const jce_vec3 up = { 0.0f, 1.0f, 0.0f };

    double worst = 0.0;
    for (int i = 0; i <= 20; i++) {
        const float mu = -1.0f + 2.0f * ((float)i / 20.0f);
        for (int a = 0; a <= 4; a++) {
            const float alt = ((float)a / 4.0f) * g_p.atmosphere_height_km;

            const float s = sqrtf(1.0f - mu * mu);
            const jce_vec3 dir = { s, mu, 0.0f };
            const jce_vec3 truth =
                jce_atmosphere_transmittance(&g_p, alt, dir, up);
            const jce_vec3 got = jce_atmosphere_sample_transmittance_lut(
                &g_p, g_tr, alt, mu);

            const double dx = fabs((double)truth.x - got.x);
            const double dy = fabs((double)truth.y - got.y);
            const double dz = fabs((double)truth.z - got.z);
            if (dx > worst) worst = dx;
            if (dy > worst) worst = dy;
            if (dz > worst) worst = dz;
        }
    }
    /* Bilinear over a 256x64 grid: the residual is interpolation error, not
     * disagreement.  A swapped axis or a wrong mapping blows straight past
     * this by orders of magnitude. */
    TEST_ASSERT_TRUE(worst < 0.02);
    unbake();
}

/* ── 2. Transmittance is physical everywhere in the table ──────────────  */

static void test_transmittance_is_bounded_and_finite(void)
{
    bake();
    for (uint32_t i = 0; i < TR_CELLS * 3u; i++) {
        TEST_ASSERT_FALSE(isnan(g_tr[i]));
        TEST_ASSERT_FALSE(isinf(g_tr[i]));
        TEST_ASSERT_TRUE(g_tr[i] >= 0.0f && g_tr[i] <= 1.0f);
    }
    unbake();
}

/* ── 3. Looking up is brighter than looking along the horizon ──────────
 *
 * The single most basic property of an atmosphere, and the one an axis swap
 * destroys: a vertical path crosses far less air than a grazing one. */

static void test_zenith_beats_horizon(void)
{
    bake();
    const jce_vec3 zen = jce_atmosphere_sample_transmittance_lut(&g_p, g_tr, 0.0f,  1.0f);
    const jce_vec3 hor = jce_atmosphere_sample_transmittance_lut(&g_p, g_tr, 0.0f,  0.0f);
    const jce_vec3 dwn = jce_atmosphere_sample_transmittance_lut(&g_p, g_tr, 0.0f, -1.0f);

    TEST_ASSERT_TRUE(zen.y > hor.y);
    TEST_ASSERT_TRUE(hor.y > dwn.y);

    /* And blue is extinguished more than red -- this is why sunsets are red.
     * If this inverts, the Rayleigh channels have been transposed. */
    TEST_ASSERT_TRUE(hor.x > hor.z);
    unbake();
}

/* ── 4. Higher up is thinner ───────────────────────────────────────────  */

static void test_altitude_increases_transmittance(void)
{
    bake();
    float prev = -1.0f;
    for (int a = 0; a <= 8; a++) {
        const float alt = ((float)a / 8.0f) * g_p.atmosphere_height_km;
        const jce_vec3 t =
            jce_atmosphere_sample_transmittance_lut(&g_p, g_tr, alt, 0.0f);
        TEST_ASSERT_TRUE(t.y >= prev - 1e-4f);
        prev = t.y;
    }
    unbake();
}

/* ── 5. Multiple scattering is positive, finite, and altitude-ordered ──
 *
 * The series 1/(1-f) diverges if the albedo is ever allowed to reach 1, which
 * would paint an infinitely bright sky; the clamp must hold across the whole
 * table, not merely at the sample the author happened to try. */

static void test_multiscatter_is_finite_everywhere(void)
{
    bake();
    for (uint32_t i = 0; i < MS_CELLS * 3u; i++) {
        TEST_ASSERT_FALSE(isnan(g_ms[i]));
        TEST_ASSERT_FALSE(isinf(g_ms[i]));
        TEST_ASSERT_TRUE(g_ms[i] >= 0.0f);
        TEST_ASSERT_TRUE(g_ms[i] < 1.0f);   /* generous; it is a coefficient */
    }

    /* Dense air scatters more than thin air. */
    const jce_vec3 low  = jce_atmosphere_sample_multiscatter_lut(&g_p, g_ms, 0.0f,  1.0f);
    const jce_vec3 high = jce_atmosphere_sample_multiscatter_lut(
        &g_p, g_ms, g_p.atmosphere_height_km, 1.0f);
    TEST_ASSERT_TRUE(low.z > high.z);

    /* And it is blue-dominant, like the sky it explains. */
    TEST_ASSERT_TRUE(low.z > low.x);
    unbake();
}

/* ── 5b. The multiple-scattering table agrees with an INDEPENDENT integral ──
 *
 * Test 5 above cannot fail on a wrong transfer factor and did not: it bounds
 * the table by 1.0, calling that "generous; it is a coefficient", and an 82x
 * amplification of a small second-order term still lands under 1.  Its own
 * header says the series "diverges if the ALBEDO is ever allowed to reach 1"
 * -- which is precisely the misconception the implementation carried.  A test
 * written from the same model as the code will certify the code's mistake.
 *
 * So this one does not reason about the model at all.  It re-derives the
 * quantity from the density profile DOCUMENTED IN THE HEADER, using a
 * different direction set (a latitude/longitude grid, not the implementation's
 * spherical Fibonacci) and a different step count, and requires the two to
 * agree.  Two discretisations of the same integral converge to the same
 * number; a path integral and a per-collision probability do not.
 *
 * The distinction under test:
 *
 *   f_ms = (1/4pi) INT_sphere [ INT sigma_s(s) T(x,s) ds ] dw    <- a PATH
 *   albedo = sigma_s / sigma_t                                    <- a COLLISION
 *
 * Rayleigh does not absorb, so its albedo is ~0.99 and 1/(1-albedo) is ~82.
 * The path integral is bounded by how much air there actually is above you.
 * These differ by a factor of 40 or more, so the tolerance here can be loose
 * and still be decisive. */

/* Extinction from the model documented in jce_atmosphere.h: exponential
 * Rayleigh and Mie profiles plus an ozone tent.  Written out longhand here on
 * purpose -- sharing the implementation's helper would make this a test of
 * whether the code equals itself. */
static void ref_coeffs(const JceAtmosphereParams *p, float h,
                       double sc[3], double ex[3])
{
    if (h < 0.0f) h = 0.0f;
    const double rh = exp(-(double)h / (double)p->rayleigh_scale_height_km);
    const double mh = exp(-(double)h / (double)p->mie_scale_height_km);
    double oz = 1.0 - fabs((double)h - (double)p->ozone_center_km)
                      / (double)p->ozone_width_km;
    if (oz < 0.0) oz = 0.0;

    const double ray[3] = { p->rayleigh_scattering.x,
                            p->rayleigh_scattering.y,
                            p->rayleigh_scattering.z };
    const double ozo[3] = { p->ozone_absorption.x,
                            p->ozone_absorption.y,
                            p->ozone_absorption.z };
    for (int i = 0; i < 3; i++) {
        sc[i] = ray[i] * rh + (double)p->mie_scattering * mh;
        ex[i] = ray[i] * rh + (double)p->mie_extinction * mh + ozo[i] * oz;
    }
}

/* The reference integral at one (altitude, sun-cosine) cell.  Returns the
 * boosted second-order radiance -- the same thing the table stores -- and
 * reports f_ms so the test can assert on it directly. */
static void ref_multiscatter(const JceAtmosphereParams *p,
                             float alt_km, float mu,
                             double out_psi[3], double out_fms[3],
                             double out_l2[3])
{
    const double Rp = p->planet_radius_km;
    const double Ra = Rp + p->atmosphere_height_km;
    const double r0 = Rp + alt_km;
    const double iso = 1.0 / (4.0 * 3.14159265358979323846);

    /* Sun direction in the same frame the module uses: up is +Y, the sun lies
     * in the XY plane at cos(zenith) = mu. */
    const double smu = mu;
    const double ssin = sqrt(fmax(0.0, 1.0 - mu * mu));
    const double sun[3] = { ssin, smu, 0.0 };

    /* A latitude/longitude direction grid -- deliberately NOT the golden-angle
     * spiral the implementation uses.  Cosine-uniform in the polar axis so the
     * samples carry equal solid angle and a plain mean is the sphere average. */
    const int NT = 16, NP = 8, NS = 48;
    double L2[3] = { 0, 0, 0 }, F[3] = { 0, 0, 0 };
    int ndir = 0;

    for (int a = 0; a < NT; a++) {
        const double wy = 1.0 - (2.0 * a + 1.0) / NT;      /* cos, uniform */
        const double wr = sqrt(fmax(0.0, 1.0 - wy * wy));
        for (int b = 0; b < NP; b++) {
            const double ph = (2.0 * 3.14159265358979323846 * (b + 0.5)) / NP;
            const double w[3] = { wr * cos(ph), wy, wr * sin(ph) };
            ndir++;

            /* Distance to the outer shell, or to the ground if that is nearer. */
            double disc = r0 * r0 * (wy * wy - 1.0) + Ra * Ra;
            if (disc < 0.0) continue;
            double tmax = -r0 * wy + sqrt(disc);
            /* A downward ray is blocked unless it clears the horizon, and
             * clearing the horizon is exactly dg < 0. Testing the sign of wy
             * rather than the sign of the root matters at altitude 0, where
             * the root is exactly 0 and a `tg > 0` guard declines -- sending
             * the ray through the planet, along 12000 km of air that the
             * altitude clamp holds at sea-level density. */
            const double dg = r0 * r0 * (wy * wy - 1.0) + Rp * Rp;
            if (wy < 0.0 && dg >= 0.0) {
                const double tg = -r0 * wy - sqrt(dg);
                if (tg < tmax) tmax = tg;
            }
            if (tmax <= 1e-6) continue;

            const double dt = tmax / NS;
            double T[3] = { 1.0, 1.0, 1.0 };
            for (int k = 0; k < NS; k++) {
                const double tt = (k + 0.5) * dt;
                const double ri = sqrt(r0 * r0 + tt * tt + 2.0 * r0 * tt * wy);
                const double h  = ri - Rp;

                double sc[3], ex[3];
                ref_coeffs(p, (float)h, sc, ex);

                /* Sun cosine at this point, and the transmittance toward it
                 * from the PUBLIC entry point -- reusing that is fine, it is
                 * a separately tested quantity (tests 1-4 above). */
                const double dp = sun[0] * (tt * w[0])
                                + sun[1] * (r0 + tt * w[1])
                                + sun[2] * (tt * w[2]);
                double mus = (ri > 1e-6) ? dp / ri : 1.0;
                if (mus >  1.0) mus =  1.0;
                if (mus < -1.0) mus = -1.0;

                jce_vec3 sd, sup;
                const double sm = sqrt(fmax(0.0, 1.0 - mus * mus));
                sd  = jce_v3((float)sm, (float)mus, 0.0f);
                sup = jce_v3(0.0f, 1.0f, 0.0f);
                const jce_vec3 tsv =
                    jce_atmosphere_transmittance(p, (float)h, sd, sup);
                const double ts[3] = { tsv.x, tsv.y, tsv.z };

                for (int i = 0; i < 3; i++) {
                    const double st = exp(-ex[i] * dt);
                    const double S  = sc[i] * iso * ts[i];
                    const double Si = (ex[i] > 1e-12) ? (S - S * st) / ex[i]
                                                      : S * dt;
                    L2[i] += T[i] * Si;
                    const double Fi = (ex[i] > 1e-12)
                                    ? (sc[i] - sc[i] * st) / ex[i] : sc[i] * dt;
                    F[i] += T[i] * Fi;
                    T[i] *= st;
                }
            }
        }
    }

    for (int i = 0; i < 3; i++) {
        const double l2 = L2[i] / ndir;
        double f = F[i] / ndir;
        if (f > 0.95) f = 0.95;
        out_fms[i]  = f;
        out_l2[i]   = l2;
        out_psi[i]  = l2 / (1.0 - f);
    }
}

static void test_multiscatter_matches_independent_integral(void)
{
    bake();

    /* Cell centres of the 32x32 table, so the bilinear fetch returns a baked
     * value rather than a blend of two. */
    const uint32_t D = JCE_ATMOSPHERE_MS_LUT_DIM;
    const struct { uint32_t r, c; } cells[] = {
        {  0u, D - 1u },     /* ground, sun at the zenith  */
        {  0u, (D - 1u) / 2u },  /* ground, sun near the horizon */
        {  4u, D - 1u },     /* aloft, sun at the zenith   */
    };

    for (size_t n = 0; n < sizeof cells / sizeof cells[0]; n++) {
        const float alt = ((float)cells[n].r / (float)(D - 1u))
                        * g_p.atmosphere_height_km;
        const float mu  = ((float)cells[n].c / (float)(D - 1u)) * 2.0f - 1.0f;

        double psi[3], fms[3], l2ref[3];
        ref_multiscatter(&g_p, alt, mu, psi, fms, l2ref);

        const jce_vec3 got =
            jce_atmosphere_sample_multiscatter_lut(&g_p, g_ms, alt, mu);
        const double g[3] = { got.x, got.y, got.z };

        for (int i = 0; i < 3; i++) {
            /* f_ms is a fraction of an isotropic field that comes back. For an
             * atmosphere 60 km deep on a 6360 km planet it is a long way below
             * 1 -- the albedo, which the implementation used to use here, is
             * 0.99 for blue. Asserting the ceiling directly is what makes the
             * two impossible to confuse. */
            TEST_ASSERT_TRUE(fms[i] >= 0.0);
            TEST_ASSERT_TRUE(fms[i] < 0.50);

            /* Two discretisations of one integral. 25% covers the difference
             * between a 128-direction lat/long grid at 48 steps and a
             * 64-direction spiral at 24; it does not cover a factor of 40. */
            const double lo = psi[i] * 0.75, hi = psi[i] * 1.25;
            TEST_ASSERT_TRUE(g[i] >= lo);
            TEST_ASSERT_TRUE(g[i] <= hi);
        }
    }
    unbake();
}

/* ── 5c. The geometric series is a CORRECTION, not the sky ─────────────
 *
 * 1/(1-f_ms) is a boost on the second-order term. For an atmosphere as thin as
 * Earth's it should be a few percent to a few tens of percent: most light that
 * leaves a parcel of air never comes back, so there is not much to sum.
 *
 * The shipped bug made this factor 18.7 / 24.2 / 82.0 by using the
 * single-scatter albedo, and NOTHING in the suite noticed -- the table stayed
 * finite, stayed positive, stayed blue-dominant and stayed altitude-ordered,
 * which is every property the old tests checked. Bounding the boost directly
 * is what turns "the sky looks too bright" into a failing assertion.
 *
 * The earlier version of this test compared the table against
 * rayleigh_scattering * iso_phase. That is a coefficient per kilometre and the
 * table holds a path integral; the ratio carried units of km, so no bound on
 * it meant anything. */
static void test_multiscatter_boost_is_a_correction(void)
{
    bake();
    const uint32_t D = JCE_ATMOSPHERE_MS_LUT_DIM;
    const float mu = 1.0f;                     /* sun at the zenith */

    double psi[3], fms[3], l2[3];
    ref_multiscatter(&g_p, 0.0f, mu, psi, fms, l2);

    const jce_vec3 got =
        jce_atmosphere_sample_multiscatter_lut(&g_p, g_ms, 0.0f, mu);
    const double g[3] = { got.x, got.y, got.z };
    (void)D;

    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(l2[i] > 0.0);
        const double boost = 1.0 / (1.0 - fms[i]);
        /* 2.0 is already generous: the physical answer at sea level is about
         * 1.03 for red and 1.25 for blue. It is nowhere near 82. */
        TEST_ASSERT_TRUE(boost > 1.0);
        TEST_ASSERT_TRUE(boost < 2.0);

        /* And the table must show the same restraint: it may exceed the raw
         * second-order term only by that same modest factor. */
        TEST_ASSERT_TRUE(g[i] > 0.0);
        TEST_ASSERT_TRUE(g[i] < l2[i] * 2.0 * 1.35);
    }
    unbake();
}


/* ── 6. Determinism: same params, bit-identical table ──────────────────
 *
 * The CPU bakes this and the GPU samples the upload.  If the bake were not
 * deterministic the two could differ between runs and nothing would report it. */

static void test_bake_is_deterministic(void)
{
    JceAtmosphereParams p = jce_atmosphere_default_params();
    float *a = (float *)malloc(sizeof(float) * TR_CELLS * 3);
    float *b = (float *)malloc(sizeof(float) * TR_CELLS * 3);
    TEST_ASSERT_TRUE(jce_atmosphere_bake_transmittance_lut(&p, a));
    TEST_ASSERT_TRUE(jce_atmosphere_bake_transmittance_lut(&p, b));
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(float) * TR_CELLS * 3);
    free(a); free(b);
}

/* ── 7. Out-of-range sampling clamps rather than wrapping ──────────────
 *
 * Wrapping would fabricate a transmittance from the opposite side of the sky
 * for an altitude above the atmosphere -- a plausible number for an impossible
 * query. */

static void test_sampling_clamps(void)
{
    bake();
    const jce_vec3 top =
        jce_atmosphere_sample_transmittance_lut(&g_p, g_tr, g_p.atmosphere_height_km, 1.0f);
    const jce_vec3 above =
        jce_atmosphere_sample_transmittance_lut(&g_p, g_tr, 1.0e6f, 1.0f);
    TEST_ASSERT_EQUAL_FLOAT(top.y, above.y);

    const jce_vec3 up1 = jce_atmosphere_sample_transmittance_lut(&g_p, g_tr, 0.0f,  1.0f);
    const jce_vec3 up9 = jce_atmosphere_sample_transmittance_lut(&g_p, g_tr, 0.0f,  9.0f);
    TEST_ASSERT_EQUAL_FLOAT(up1.y, up9.y);
    unbake();
}

/* ── 8. NULL safety ────────────────────────────────────────────────────  */

static void test_null_safety(void)
{
    JceAtmosphereParams p = jce_atmosphere_default_params();
    float one[3];
    TEST_ASSERT_FALSE(jce_atmosphere_bake_transmittance_lut(NULL, one));
    TEST_ASSERT_FALSE(jce_atmosphere_bake_transmittance_lut(&p, NULL));
    TEST_ASSERT_FALSE(jce_atmosphere_bake_multiscatter_lut(NULL, one));
    TEST_ASSERT_FALSE(jce_atmosphere_bake_multiscatter_lut(&p, NULL));

    /* A NULL table reads as "nothing absorbed" -- it can only fail to darken,
     * never invent extinction that is not there. */
    const jce_vec3 t = jce_atmosphere_sample_transmittance_lut(&p, NULL, 0.0f, 1.0f);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, t.x);
}

/* ── 9. THE GPU PATH READS THE SAME ATMOSPHERE ─────────────────────────
 *
 * The sky pass will sample an uploaded texture with a normalised (u,v) while
 * the lighting authority samples the CPU table with (altitude, mu).  If those
 * two addressings disagree the sky is smoothly, plausibly wrong -- exactly the
 * failure nobody catches by looking at it.
 *
 * So this walks the packed data the way a shader will and requires it to match
 * the authority.  It is the same shape of test as "sampling the LUT equals
 * evaluating the integral": agreement between the two paths, not plausibility
 * of either. */

static void test_packed_texture_matches_the_cpu_authority(void)
{
    bake();
    const uint32_t W = JCE_ATMOSPHERE_LUT_COSINE;
    const uint32_t H = JCE_ATMOSPHERE_LUT_ALTITUDE;

    float *rgba = (float *)malloc(sizeof(float) * W * H * 4u);
    TEST_ASSERT_NOT_NULL(rgba);
    jce_atmosphere_pack_lut_rgba32f(g_tr, W * H, rgba);

    /* Alpha must be 1 everywhere: a zero alpha would make the texture read as
     * fully transparent on any path that happens to respect it. */
    for (uint32_t i = 0; i < W * H; i++)
        TEST_ASSERT_EQUAL_FLOAT(1.0f, rgba[i * 4u + 3u]);

    double worst = 0.0;
    for (int a = 0; a <= 6; a++) {
        const float alt = ((float)a / 6.0f) * g_p.atmosphere_height_km;
        for (int m = 0; m <= 24; m++) {
            const float mu = -1.0f + 2.0f * ((float)m / 24.0f);

            float u, v;
            jce_atmosphere_lut_uv(&g_p, alt, mu, &u, &v);
            const jce_vec3 gpu =
                jce_atmosphere_sample_packed_rgba32f(rgba, W, H, u, v);
            const jce_vec3 cpu =
                jce_atmosphere_sample_transmittance_lut(&g_p, g_tr, alt, mu);

            const double dx = fabs((double)gpu.x - cpu.x);
            const double dy = fabs((double)gpu.y - cpu.y);
            const double dz = fabs((double)gpu.z - cpu.z);
            if (dx > worst) worst = dx;
            if (dy > worst) worst = dy;
            if (dz > worst) worst = dz;
        }
    }
    /* Both do bilinear over the same grid, so this is float noise, not a
     * modelling difference. */
    TEST_ASSERT_TRUE(worst < 1e-5);

    /* The UV mapping must hit texel CENTRES: the first entry sits at 0.5/N,
     * not 0.  Landing on 0 shifts the whole table half a texel, and the
     * horizon -- where transmittance changes fastest -- is where that shows. */
    float u0, v0, u1, v1;
    jce_atmosphere_lut_uv(&g_p, 0.0f, -1.0f, &u0, &v0);
    jce_atmosphere_lut_uv(&g_p, g_p.atmosphere_height_km, 1.0f, &u1, &v1);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f / (float)W, u0);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f / (float)H, v0);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f - 0.5f / (float)W, u1);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f - 0.5f / (float)H, v1);

    free(rgba);
    unbake();
}

/* Out-of-range UV clamps rather than wrapping -- a wrapped fetch would return
 * the opposite horizon for a ray pointing straight down. */
static void test_packed_sampling_clamps(void)
{
    bake();
    const uint32_t W = JCE_ATMOSPHERE_LUT_COSINE, H = JCE_ATMOSPHERE_LUT_ALTITUDE;
    float *rgba = (float *)malloc(sizeof(float) * W * H * 4u);
    jce_atmosphere_pack_lut_rgba32f(g_tr, W * H, rgba);

    const jce_vec3 lo = jce_atmosphere_sample_packed_rgba32f(rgba, W, H, 0.0f, 0.0f);
    const jce_vec3 under = jce_atmosphere_sample_packed_rgba32f(rgba, W, H, -3.0f, -3.0f);
    TEST_ASSERT_EQUAL_FLOAT(lo.y, under.y);

    const jce_vec3 hi = jce_atmosphere_sample_packed_rgba32f(rgba, W, H, 1.0f, 1.0f);
    const jce_vec3 over = jce_atmosphere_sample_packed_rgba32f(rgba, W, H, 9.0f, 9.0f);
    TEST_ASSERT_EQUAL_FLOAT(hi.y, over.y);

    /* NULL is "nothing absorbed" -- it can only fail to darken. */
    const jce_vec3 n = jce_atmosphere_sample_packed_rgba32f(NULL, W, H, 0.5f, 0.5f);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, n.x);

    free(rgba);
    unbake();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lut_agrees_with_the_integral);
    RUN_TEST(test_transmittance_is_bounded_and_finite);
    RUN_TEST(test_zenith_beats_horizon);
    RUN_TEST(test_altitude_increases_transmittance);
    RUN_TEST(test_multiscatter_is_finite_everywhere);
    RUN_TEST(test_multiscatter_matches_independent_integral);
    RUN_TEST(test_multiscatter_boost_is_a_correction);
    RUN_TEST(test_bake_is_deterministic);
    RUN_TEST(test_sampling_clamps);
    RUN_TEST(test_null_safety);
    RUN_TEST(test_packed_texture_matches_the_cpu_authority);
    RUN_TEST(test_packed_sampling_clamps);
    return UNITY_END();
}
