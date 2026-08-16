/*
 * jce_atmosphere.c -- Physically based atmospheric transmittance.
 *
 * See jce_atmosphere.h for the contract and for why the sun colour is derived
 * rather than authored.
 *
 * The model is the standard three-component one: Rayleigh scattering off air
 * molecules (strongly wavelength dependent, which is what makes the sky blue
 * and the low sun red), Mie scattering off aerosols (nearly grey, which is
 * what makes haze white), and ozone absorption (which is what keeps a clear
 * zenith sky from going cyan).
 *
 * Transmittance along a ray is Beer-Lambert over the optical depth:
 *
 *     T(a -> b) = exp( -integral_a^b sigma_e(s) ds )
 *
 * with sigma_e the sum of the three extinctions at the sample altitude.
 * Rayleigh and Mie densities fall off exponentially with their own scale
 * heights; ozone is a tent function centred in the stratosphere.
 */

#include <jce/middleware/world/jce_atmosphere.h>

#include <math.h>

/* ── Parameters ────────────────────────────────────────────────────── */

JceAtmosphereParams JCE_CALL jce_atmosphere_default_params(void)
{
    JceAtmosphereParams p;

    p.planet_radius_km         = 6360.0f;
    p.atmosphere_height_km     = 60.0f;

    /* Per-kilometre scattering at sea level.  The 1:2.3:5.7 ratio across
     * R:G:B is the ~1/lambda^4 Rayleigh dependence, and it is the entire
     * reason a long path reddens. */
    p.rayleigh_scattering      = jce_v3(5.802e-3f, 13.558e-3f, 33.100e-3f);
    p.rayleigh_scale_height_km = 8.0f;

    /* Aerosols are nearly grey; extinction exceeds scattering because
     * aerosols absorb as well as scatter. */
    p.mie_scattering           = 3.996e-3f;
    p.mie_extinction           = 4.440e-3f;
    p.mie_scale_height_km      = 1.2f;

    /* Ozone absorbs in the Chappuis band -- strongest in green, which is what
     * stops a deep zenith sky drifting toward cyan. */
    p.ozone_absorption         = jce_v3(0.650e-3f, 1.881e-3f, 0.085e-3f);
    p.ozone_center_km          = 25.0f;
    p.ozone_width_km           = 30.0f;

    p.sun_illuminance_top      = 128000.0f;
    p.min_sun_elevation_deg    = -2.0f;
    p.steps                    = 40;

    return p;
}

/* ── Helpers ───────────────────────────────────────────────────────── */

static float atm_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static jce_vec3 atm_normalize(jce_vec3 v)
{
    float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len <= 1e-8f) return jce_v3(0.0f, 1.0f, 0.0f);
    return jce_v3(v.x / len, v.y / len, v.z / len);
}

/* Extinction coefficient (per km, per channel) at a given altitude. */
static jce_vec3 atm_extinction(const JceAtmosphereParams *p, float h_km)
{
    if (h_km < 0.0f) h_km = 0.0f;

    const float rayleigh_density = expf(-h_km / p->rayleigh_scale_height_km);
    const float mie_density      = expf(-h_km / p->mie_scale_height_km);

    /* Tent centred on ozone_center_km, reaching zero at +/- ozone_width_km. */
    float ozone_density = 1.0f - fabsf(h_km - p->ozone_center_km) /
                                 p->ozone_width_km;
    if (ozone_density < 0.0f) ozone_density = 0.0f;

    jce_vec3 e;
    e.x = p->rayleigh_scattering.x * rayleigh_density
        + p->mie_extinction        * mie_density
        + p->ozone_absorption.x    * ozone_density;
    e.y = p->rayleigh_scattering.y * rayleigh_density
        + p->mie_extinction        * mie_density
        + p->ozone_absorption.y    * ozone_density;
    e.z = p->rayleigh_scattering.z * rayleigh_density
        + p->mie_extinction        * mie_density
        + p->ozone_absorption.z    * ozone_density;
    return e;
}

/* Distance from a point at radius `r` travelling with cos(zenith) = `mu`
 * to the outer atmosphere shell of radius `R`.  Positive root of
 * d^2 + 2*r*mu*d + r^2 - R^2 = 0. */
static float atm_distance_to_top(float r, float mu, float R)
{
    const float disc = r * r * (mu * mu - 1.0f) + R * R;
    if (disc < 0.0f) return 0.0f;
    const float d = -r * mu + sqrtf(disc);
    return d > 0.0f ? d : 0.0f;
}

/* ── Transmittance ─────────────────────────────────────────────────── */

jce_vec3 JCE_CALL jce_atmosphere_transmittance(
    const JceAtmosphereParams *p, float altitude_km,
    jce_vec3 dir, jce_vec3 up)
{
    if (!p) return jce_v3(1.0f, 1.0f, 1.0f);

    const jce_vec3 d = atm_normalize(dir);
    const jce_vec3 u = atm_normalize(up);

    /* cos(zenith angle) of the ray. */
    const float mu = atm_clampf(d.x * u.x + d.y * u.y + d.z * u.z, -1.0f, 1.0f);

    const float Rp = p->planet_radius_km;
    const float Ra = Rp + p->atmosphere_height_km;
    const float r  = Rp + (altitude_km < 0.0f ? 0.0f : altitude_km);

    const float len = atm_distance_to_top(r, mu, Ra);
    if (len <= 0.0f) return jce_v3(1.0f, 1.0f, 1.0f);

    const int steps = p->steps > 0 ? p->steps : 40;
    const float dt  = len / (float)steps;

    /* Trapezoid-free midpoint accumulation: sampling at segment centres is
     * both cheaper and better behaved than endpoint sampling for an
     * exponential integrand. */
    jce_vec3 optical_depth = jce_v3(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < steps; i++) {
        const float t = ((float)i + 0.5f) * dt;
        /* Altitude at distance t along the ray, by the law of cosines. */
        const float ri = sqrtf(r * r + t * t + 2.0f * r * t * mu);
        const jce_vec3 e = atm_extinction(p, ri - Rp);
        optical_depth.x += e.x * dt;
        optical_depth.y += e.y * dt;
        optical_depth.z += e.z * dt;
    }

    return jce_v3(expf(-optical_depth.x),
                  expf(-optical_depth.y),
                  expf(-optical_depth.z));
}

/* ── Sun illuminance ───────────────────────────────────────────────── */

jce_vec3 JCE_CALL jce_atmosphere_sun_illuminance(
    const JceAtmosphereParams *p, float altitude_km,
    jce_vec3 sun_dir, jce_vec3 up)
{
    if (!p) return jce_v3(0.0f, 0.0f, 0.0f);

    const jce_vec3 s = atm_normalize(sun_dir);
    const jce_vec3 u = atm_normalize(up);
    const float sin_elev = atm_clampf(s.x * u.x + s.y * u.y + s.z * u.z,
                                      -1.0f, 1.0f);
    const float elev_deg = asinf(sin_elev) * 180.0f / 3.14159265358979f;

    /* Clamp the elevation used for the transmittance lookup.  Without this the
     * integral degenerates as the ray grazes the planet and the sun snaps to
     * black at the terminator; production sky systems ship the same control. */
    const float lookup_deg = elev_deg < p->min_sun_elevation_deg
                           ? p->min_sun_elevation_deg : elev_deg;

    jce_vec3 lookup_dir = s;
    if (lookup_deg != elev_deg) {
        /* Rebuild a direction at the clamped elevation, keeping azimuth: take
         * the horizontal part of s and re-elevate it. */
        jce_vec3 horiz = jce_v3(s.x - u.x * sin_elev,
                                s.y - u.y * sin_elev,
                                s.z - u.z * sin_elev);
        horiz = atm_normalize(horiz);
        const float rad = lookup_deg * 3.14159265358979f / 180.0f;
        const float ch = cosf(rad), sh = sinf(rad);
        lookup_dir = atm_normalize(jce_v3(horiz.x * ch + u.x * sh,
                                          horiz.y * ch + u.y * sh,
                                          horiz.z * ch + u.z * sh));
    }

    const jce_vec3 T =
        jce_atmosphere_transmittance(p, altitude_km, lookup_dir, up);

    /* Below the horizon the direct sun is occluded by the planet.  Fade over a
     * few degrees rather than cutting, so the terminator is a dusk and not a
     * light switch.  This is a visibility factor, separate from transmittance. */
    float visibility = 1.0f;
    if (elev_deg < 0.0f) {
        const float fade_deg = 6.0f;
        visibility = 1.0f + elev_deg / fade_deg;   /* 1 at 0deg, 0 at -6deg */
        visibility = atm_clampf(visibility, 0.0f, 1.0f);
        visibility *= visibility;                  /* ease out */
    }

    const float E = p->sun_illuminance_top * visibility;
    return jce_v3(T.x * E, T.y * E, T.z * E);
}

/* ── Precomputed tables ────────────────────────────────────────────────
 * See jce_atmosphere.h.  Both bakes are pure functions of the parameters:
 * same params in, bit-identical table out, on every platform. */

/* Build the (dir, up) pair a given cos(zenith) corresponds to.  `up` is +Y and
 * the direction is tilted in the XY plane; azimuth is irrelevant because the
 * atmosphere model is spherically symmetric about the vertical. */
static void atmo_dir_from_mu(float mu, jce_vec3 *out_dir, jce_vec3 *out_up)
{
    if (mu >  1.0f) mu =  1.0f;
    if (mu < -1.0f) mu = -1.0f;
    const float s = sqrtf(1.0f - mu * mu);   /* sin(zenith) >= 0 */
    *out_dir = jce_v3(s, mu, 0.0f);
    *out_up  = jce_v3(0.0f, 1.0f, 0.0f);
}

bool JCE_CALL jce_atmosphere_bake_transmittance_lut(
    const JceAtmosphereParams *p, float *out_rgb)
{
    if (!p || !out_rgb) return false;

    const uint32_t H = JCE_ATMOSPHERE_LUT_ALTITUDE;
    const uint32_t W = JCE_ATMOSPHERE_LUT_COSINE;

    for (uint32_t r = 0; r < H; r++) {
        const float alt = (H > 1u)
            ? ((float)r / (float)(H - 1u)) * p->atmosphere_height_km
            : 0.0f;
        for (uint32_t c = 0; c < W; c++) {
            const float mu = (W > 1u)
                ? ((float)c / (float)(W - 1u)) * 2.0f - 1.0f
                : 1.0f;
            jce_vec3 dir, up;
            atmo_dir_from_mu(mu, &dir, &up);
            const jce_vec3 t = jce_atmosphere_transmittance(p, alt, dir, up);
            float *dst = &out_rgb[((size_t)r * W + c) * 3u];
            dst[0] = t.x; dst[1] = t.y; dst[2] = t.z;
        }
    }
    return true;
}

/* Shared bilinear fetch for both tables (they share a parameterisation). */
static jce_vec3 atmo_sample_lut(const JceAtmosphereParams *p, const float *lut,
                                uint32_t W, uint32_t H,
                                float altitude_km, float mu)
{
    if (!p || !lut) return jce_v3(1.0f, 1.0f, 1.0f);

    const float top = (p->atmosphere_height_km > 0.0f)
                    ? p->atmosphere_height_km : 1.0f;
    float fy = (altitude_km / top) * (float)(H - 1u);
    float fx = ((mu * 0.5f) + 0.5f) * (float)(W - 1u);
    /* Clamp rather than wrap: an altitude above the atmosphere is vacuum and a
     * |mu| past 1 is not a direction.  Wrapping either would fabricate a
     * transmittance from the opposite side of the sky. */
    if (fy < 0.0f) fy = 0.0f;
    if (fx < 0.0f) fx = 0.0f;
    if (fy > (float)(H - 1u)) fy = (float)(H - 1u);
    if (fx > (float)(W - 1u)) fx = (float)(W - 1u);

    const uint32_t y0 = (uint32_t)fy, x0 = (uint32_t)fx;
    const uint32_t y1 = (y0 + 1u < H) ? y0 + 1u : y0;
    const uint32_t x1 = (x0 + 1u < W) ? x0 + 1u : x0;
    const float ty = fy - (float)y0, tx = fx - (float)x0;

    const float *p00 = &lut[((size_t)y0 * W + x0) * 3u];
    const float *p10 = &lut[((size_t)y0 * W + x1) * 3u];
    const float *p01 = &lut[((size_t)y1 * W + x0) * 3u];
    const float *p11 = &lut[((size_t)y1 * W + x1) * 3u];

    jce_vec3 o;
    float *d = &o.x;
    for (int i = 0; i < 3; i++) {
        const float a = p00[i] + (p10[i] - p00[i]) * tx;
        const float b = p01[i] + (p11[i] - p01[i]) * tx;
        d[i] = a + (b - a) * ty;
    }
    return o;
}

jce_vec3 JCE_CALL jce_atmosphere_sample_transmittance_lut(
    const JceAtmosphereParams *p, const float *lut,
    float altitude_km, float mu)
{
    return atmo_sample_lut(p, lut, JCE_ATMOSPHERE_LUT_COSINE,
                           JCE_ATMOSPHERE_LUT_ALTITUDE, altitude_km, mu);
}

/* Multiple scattering, Hillaire 2020 s.4.
 *
 * The key move is that after the first bounce the light field is treated as
 * ISOTROPIC, so each further order is the previous one times a single scalar
 * transfer factor f_ms.  The whole infinite series then collapses to the
 * geometric sum 1/(1-f_ms), which is why this costs one small table instead of
 * an unbounded number of scattering passes.
 *
 * WHAT f_ms IS, AND WHAT IT IS NOT.
 *
 * This function previously used the SINGLE-SCATTER ALBEDO sigma_s/sigma_t as
 * f_ms.  Those are different quantities and the difference is not subtle:
 *
 *   albedo  is PER COLLISION -- given that a photon hits something, the
 *           probability that it scatters rather than being absorbed.
 *   f_ms    is PER PATH -- of a unit isotropic radiance field surrounding the
 *           point, the fraction that gets scattered back TO the point and
 *           survives the trip there.
 *
 * Rayleigh scattering does not absorb, so its albedo is ~1 by construction.
 * At ground level the old code computed (0.947, 0.959, 0.988) and divided by
 * (1 - albedo), amplifying by (18.7, 24.2, 82.0).  Blue got the worst of it
 * because blue scatters most and therefore absorbs least -- the physics that
 * makes the sky blue is exactly the physics that made this term explode.
 *
 * Using the albedo asserts that the medium is infinitely thick in every
 * direction: every photon eventually comes back.  A 60 km shell of air is not
 * that.  Most of the light leaves for space and never returns, which is what
 * the transmittance inside the path integral accounts for and what a
 * per-collision probability has no way to express.
 *
 * f_ms is therefore a path integral, and it needs the sphere:
 *
 *   f_ms(x) = (1/4pi) * INT_sphere [ INT_0^tmax sigma_s(s) T(x,s) ds ] dw
 *
 * The 1/4pi is the isotropic phase, and pairing it with the solid angle turns
 * the outer integral into a plain MEAN over uniformly distributed directions
 * -- which is why the direction loop below divides by the sample count and
 * applies no phase factor of its own.  The result is bounded above by the
 * albedo and is far below it for a thin atmosphere, so 1/(1-f_ms) becomes a
 * modest boost rather than a two-order-of-magnitude one.
 *
 * The second-order radiance L2 gets the same treatment for the same reason: it
 * was a per-unit-length quantity (sigma_s * phase * T_sun) that had never been
 * integrated along anything, while the comment above it claimed both terms
 * were "integrated over the sphere with a small uniform direction set".  No
 * such integration existed anywhere in the function.  Prose describing an
 * algorithm the code does not implement is worse than no prose: it is what let
 * a 40x error read as intentional for as long as it did.
 *
 * Segment integration uses the energy-conserving analytic form
 * (S - S*exp(-sigma_t*dt)) / sigma_t rather than S*dt, so the answer stops
 * depending on the step count once the medium is optically thick.
 *
 * NOT MODELLED: light that bounces off the ground and back into the air.
 * Hillaire includes it via a ground albedo; this atmosphere carries no such
 * parameter, so rays that hit the planet simply terminate.  That leaves the
 * horizon slightly darker than a full model would make it, which is the safe
 * direction to be wrong in, and it is recorded here so that it stays a known
 * omission rather than a discrepancy someone rediscovers from an image. */

/* Direction and step counts for the multiple-scattering integral.  This is a
 * bake, not a frame cost, so they are chosen for accuracy: 64 directions is
 * what Hillaire's reference implementation uses. */
#define JCE_ATMO_MS_DIRS  64
#define JCE_ATMO_MS_STEPS 24

/* Coarse transmittance table private to this bake.
 *
 * The march needs transmittance toward the sun at every sample point --
 * 32*32*64*24 = 1.6M of them.  Running the full 40-step integral at each would
 * be 63M inner iterations to fill a table of 1024 texels.  A coarse local table
 * costs 2048 integrals once and is sampled from there.
 *
 * Deliberately NOT the shipped JCE_ATMOSPHERE_LUT_* table: that one is 64x256
 * (192 KB), and this module allocates nothing.  Keeping the scratch small
 * enough to be a plain static array preserves the "pure math, no allocator"
 * property the file was written to have. */
#define JCE_ATMO_MS_TR_H 32
#define JCE_ATMO_MS_TR_W 64

static void atmo_ms_build_tr(const JceAtmosphereParams *p, float *tr)
{
    for (int r = 0; r < JCE_ATMO_MS_TR_H; r++) {
        const float alt = ((float)r / (float)(JCE_ATMO_MS_TR_H - 1))
                        * p->atmosphere_height_km;
        for (int c = 0; c < JCE_ATMO_MS_TR_W; c++) {
            const float mu = ((float)c / (float)(JCE_ATMO_MS_TR_W - 1))
                           * 2.0f - 1.0f;
            jce_vec3 dir, up;
            atmo_dir_from_mu(mu, &dir, &up);
            const jce_vec3 v = jce_atmosphere_transmittance(p, alt, dir, up);
            float *d = &tr[((size_t)r * JCE_ATMO_MS_TR_W + (size_t)c) * 3u];
            d[0] = v.x; d[1] = v.y; d[2] = v.z;
        }
    }
}

static void atmo_ms_tr_fetch(const JceAtmosphereParams *p, const float *tr,
                             float alt_km, float mu, float out[3])
{
    const float top = (p->atmosphere_height_km > 0.0f)
                    ? p->atmosphere_height_km : 1.0f;
    const float fy = atm_clampf(alt_km / top, 0.0f, 1.0f)
                   * (float)(JCE_ATMO_MS_TR_H - 1);
    const float fx = atm_clampf(mu * 0.5f + 0.5f, 0.0f, 1.0f)
                   * (float)(JCE_ATMO_MS_TR_W - 1);
    const int y0 = (int)fy, x0 = (int)fx;
    const int y1 = (y0 + 1 < JCE_ATMO_MS_TR_H) ? y0 + 1 : y0;
    const int x1 = (x0 + 1 < JCE_ATMO_MS_TR_W) ? x0 + 1 : x0;
    const float ty = fy - (float)y0, tx = fx - (float)x0;
    const float *q00 = &tr[((size_t)y0 * JCE_ATMO_MS_TR_W + (size_t)x0) * 3u];
    const float *q10 = &tr[((size_t)y0 * JCE_ATMO_MS_TR_W + (size_t)x1) * 3u];
    const float *q01 = &tr[((size_t)y1 * JCE_ATMO_MS_TR_W + (size_t)x0) * 3u];
    const float *q11 = &tr[((size_t)y1 * JCE_ATMO_MS_TR_W + (size_t)x1) * 3u];
    for (int i = 0; i < 3; i++) {
        const float a = q00[i] + (q10[i] - q00[i]) * tx;
        const float b = q01[i] + (q11[i] - q01[i]) * tx;
        out[i] = a + (b - a) * ty;
    }
}

/* Uniformly distributed directions on the sphere, with world up as the polar
 * axis so that omega.y is directly the cosine this atmosphere is parameterised
 * by.  Spherical Fibonacci: no clustering at the poles, and deterministic -- a
 * bake that moved with a random seed would make every golden image a moving
 * target. */
static jce_vec3 atmo_ms_dir(int i, int n)
{
    const float ga = 2.39996322972865332f;            /* golden angle */
    const float y  = 1.0f - (2.0f * (float)i + 1.0f) / (float)n;
    const float rr = sqrtf(atm_clampf(1.0f - y * y, 0.0f, 1.0f));
    const float ph = ga * (float)i;
    return jce_v3(rr * cosf(ph), y, rr * sinf(ph));
}

bool JCE_CALL jce_atmosphere_bake_multiscatter_lut(
    const JceAtmosphereParams *p, float *out_rgb)
{
    if (!p || !out_rgb) return false;

    const uint32_t D = JCE_ATMOSPHERE_MS_LUT_DIM;
    /* Isotropic phase: 1/(4*pi) for both Rayleigh and Mie at this order, which
     * is exactly the approximation that makes the series collapse.  Applied to
     * the SUNLIT term only -- see the header comment for why f_ms carries no
     * phase of its own. */
    const float iso_phase = 1.0f / (4.0f * 3.14159265358979323846f);
    const float Rp = p->planet_radius_km;
    const float Ra = Rp + p->atmosphere_height_km;

    static float tr[JCE_ATMO_MS_TR_H * JCE_ATMO_MS_TR_W * 3];
    atmo_ms_build_tr(p, tr);

    for (uint32_t r = 0; r < D; r++) {
        const float alt = (D > 1u)
            ? ((float)r / (float)(D - 1u)) * p->atmosphere_height_km
            : 0.0f;
        const float r0 = Rp + alt;

        for (uint32_t c = 0; c < D; c++) {
            const float mu = (D > 1u)
                ? ((float)c / (float)(D - 1u)) * 2.0f - 1.0f
                : 1.0f;

            jce_vec3 sun_dir, up;
            atmo_dir_from_mu(mu, &sun_dir, &up);      /* up is (0,1,0) */
            (void)up;

            double L2[3]  = { 0.0, 0.0, 0.0 };
            double fms[3] = { 0.0, 0.0, 0.0 };

            for (int j = 0; j < JCE_ATMO_MS_DIRS; j++) {
                const jce_vec3 w = atmo_ms_dir(j, JCE_ATMO_MS_DIRS);
                const float mu_w = w.y;               /* dot(w, up) */

                /* How far this ray travels before leaving the atmosphere, or
                 * hitting the planet, whichever comes first.  A downward ray
                 * that still clears the horizon must be marched all the way to
                 * the far shell, so the ground test uses the NEAR root and
                 * only counts when it lies in front of us. */
                float t_max = atm_distance_to_top(r0, mu_w, Ra);
                const float disc_g = r0 * r0 * (mu_w * mu_w - 1.0f) + Rp * Rp;
                /* A ray pointing DOWN hits the planet unless it clears the
                 * horizon, and clearing the horizon is exactly disc_g < 0.
                 * The test is on mu_w rather than on the root being positive
                 * because at alt = 0 that root is exactly 0: a downward ray
                 * standing on the ground is blocked immediately, and a
                 * `tg > 0` guard declares it unblocked and marches it THROUGH
                 * the planet.  Samples then sit at negative altitude, where
                 * exp(-h/H) is the exponential of a large POSITIVE number --
                 * while atm_extinction clamps h to 0 and stays finite.  One
                 * side infinite and the other not is how a scattering
                 * coefficient becomes inf and their ratio becomes NaN. */
                if (mu_w < 0.0f && disc_g >= 0.0f) {
                    const float tg = -r0 * mu_w - sqrtf(disc_g);
                    if (tg < t_max) t_max = tg;
                }
                if (t_max <= 1e-6f) continue;

                const float dt = t_max / (float)JCE_ATMO_MS_STEPS;
                float T[3] = { 1.0f, 1.0f, 1.0f };

                for (int k = 0; k < JCE_ATMO_MS_STEPS; k++) {
                    const float tt = ((float)k + 0.5f) * dt;
                    /* Radius at this step, by the law of cosines. */
                    const float ri = sqrtf(r0 * r0 + tt * tt
                                           + 2.0f * r0 * tt * mu_w);
                    const float h = ri - Rp;

                    const jce_vec3 exv = atm_extinction(p, h);
                    const float ex[3] = { exv.x, exv.y, exv.z };

                    const float rh = (p->rayleigh_scale_height_km > 0.0f)
                        ? expf(-h / p->rayleigh_scale_height_km) : 0.0f;
                    const float mh = (p->mie_scale_height_km > 0.0f)
                        ? expf(-h / p->mie_scale_height_km) : 0.0f;
                    const float sc[3] = {
                        p->rayleigh_scattering.x * rh + p->mie_scattering * mh,
                        p->rayleigh_scattering.y * rh + p->mie_scattering * mh,
                        p->rayleigh_scattering.z * rh + p->mie_scattering * mh,
                    };

                    /* Sun cosine at THIS point, not at the table cell: the up
                     * axis rotates along the ray, and over a 60 km shell on a
                     * 6360 km planet that rotation is small -- but it is the
                     * only thing that puts the far end of a long horizontal
                     * ray on the other side of the terminator. */
                    const float dotsp = sun_dir.x * (tt * w.x)
                                      + sun_dir.y * (r0 + tt * w.y)
                                      + sun_dir.z * (tt * w.z);
                    const float mu_s = (ri > 1e-6f)
                                     ? atm_clampf(dotsp / ri, -1.0f, 1.0f)
                                     : 1.0f;

                    float tsun[3];
                    atmo_ms_tr_fetch(p, tr, h, mu_s, tsun);

                    for (int i = 0; i < 3; i++) {
                        const float step_T = expf(-ex[i] * dt);
                        const float S  = sc[i] * iso_phase * tsun[i];
                        const float Si = (ex[i] > 1e-9f)
                                       ? (S - S * step_T) / ex[i] : S * dt;
                        L2[i] += (double)(T[i] * Si);
                        const float Fi = (ex[i] > 1e-9f)
                                       ? (sc[i] - sc[i] * step_T) / ex[i]
                                       : sc[i] * dt;
                        fms[i] += (double)(T[i] * Fi);
                        T[i] *= step_T;
                    }
                }
            }

            float *dst = &out_rgb[((size_t)r * D + c) * 3u];
            for (int i = 0; i < 3; i++) {
                const double inv_n = 1.0 / (double)JCE_ATMO_MS_DIRS;
                const double l2 = L2[i] * inv_n;
                double f = fms[i] * inv_n;
                /* f_ms is a fraction by construction and cannot reach 1 for a
                 * finite atmosphere.  This clamp exists for a degenerate
                 * parameter set (an absurd scattering coefficient, a zero
                 * extinction), so that a bad input yields a bright sky rather
                 * than an infinite one. */
                if (f > 0.95) f = 0.95;
                if (f < 0.0)  f = 0.0;
                dst[i] = (float)(l2 / (1.0 - f));
            }
        }
    }
    return true;
}

jce_vec3 JCE_CALL jce_atmosphere_sample_multiscatter_lut(
    const JceAtmosphereParams *p, const float *lut,
    float altitude_km, float mu)
{
    return atmo_sample_lut(p, lut, JCE_ATMOSPHERE_MS_LUT_DIM,
                           JCE_ATMOSPHERE_MS_LUT_DIM, altitude_km, mu);
}

/* ── GPU upload ─────────────────────────────────────────────────────────
 * See jce_atmosphere.h. */

void JCE_CALL jce_atmosphere_pack_lut_rgba32f(const float *lut, uint32_t cells,
                                              float *out_rgba)
{
    if (!lut || !out_rgba) return;
    for (uint32_t i = 0; i < cells; i++) {
        out_rgba[i * 4u + 0u] = lut[i * 3u + 0u];
        out_rgba[i * 4u + 1u] = lut[i * 3u + 1u];
        out_rgba[i * 4u + 2u] = lut[i * 3u + 2u];
        out_rgba[i * 4u + 3u] = 1.0f;
    }
}

void JCE_CALL jce_atmosphere_lut_uv(const JceAtmosphereParams *p,
                                    float altitude_km, float mu,
                                    float *out_u, float *out_v)
{
    const float top = (p && p->atmosphere_height_km > 0.0f)
                    ? p->atmosphere_height_km : 1.0f;

    float fy = altitude_km / top;
    float fx = mu * 0.5f + 0.5f;
    if (fy < 0.0f) fy = 0.0f; else if (fy > 1.0f) fy = 1.0f;
    if (fx < 0.0f) fx = 0.0f; else if (fx > 1.0f) fx = 1.0f;

    /* Texel CENTRES: entry i of N lives at (i + 0.5)/N, so the normalised
     * coordinate of the FIRST entry is 0.5/N and of the last is 1 - 0.5/N. */
    const float w = (float)JCE_ATMOSPHERE_LUT_COSINE;
    const float h = (float)JCE_ATMOSPHERE_LUT_ALTITUDE;
    if (out_u) *out_u = (fx * (w - 1.0f) + 0.5f) / w;
    if (out_v) *out_v = (fy * (h - 1.0f) + 0.5f) / h;
}

jce_vec3 JCE_CALL jce_atmosphere_sample_packed_rgba32f(
    const float *rgba, uint32_t width, uint32_t height, float u, float v)
{
    if (!rgba || width == 0u || height == 0u) return jce_v3(1.0f, 1.0f, 1.0f);

    /* Undo the texel-centre mapping, exactly as a GPU sampler does. */
    float fx = u * (float)width  - 0.5f;
    float fy = v * (float)height - 0.5f;
    if (fx < 0.0f) fx = 0.0f;
    if (fy < 0.0f) fy = 0.0f;
    if (fx > (float)(width  - 1u)) fx = (float)(width  - 1u);
    if (fy > (float)(height - 1u)) fy = (float)(height - 1u);

    const uint32_t x0 = (uint32_t)fx, y0 = (uint32_t)fy;
    const uint32_t x1 = (x0 + 1u < width)  ? x0 + 1u : x0;
    const uint32_t y1 = (y0 + 1u < height) ? y0 + 1u : y0;
    const float tx = fx - (float)x0, ty = fy - (float)y0;

    const float *p00 = &rgba[((size_t)y0 * width + x0) * 4u];
    const float *p10 = &rgba[((size_t)y0 * width + x1) * 4u];
    const float *p01 = &rgba[((size_t)y1 * width + x0) * 4u];
    const float *p11 = &rgba[((size_t)y1 * width + x1) * 4u];

    jce_vec3 o;
    float *d = &o.x;
    for (int i = 0; i < 3; i++) {
        const float a = p00[i] + (p10[i] - p00[i]) * tx;
        const float b = p01[i] + (p11[i] - p01[i]) * tx;
        d[i] = a + (b - a) * ty;
    }
    return o;
}
