/*
 * jce_sky.c -- analytic Preetham daylight model (pure CPU).
 *
 * Closed-form implementation of Preetham et al. "A Practical Analytic
 * Model for Daylight" (SIGGRAPH 1999).  No fitted dataset: every term is
 * a polynomial in turbidity T and the sun-zenith angle theta_s, so the
 * model is fully testable on the CPU with only <math.h>.
 *
 * fs_sky.sc evaluates the SAME math on the GPU.  When editing the Perez
 * formula, the xyY->RGB matrix, or the horizon clamps here, mirror the
 * change in the shader (and vice versa) so the two stay bit-aligned.
 *
 * ------------------------------------------------------------------
 * The shared formula (mirrored exactly in fs_sky.sc):
 *
 *   Perez F(theta, gamma; A..E) =
 *       (1 + A * exp(B / cos(theta)))
 *     * (1 + C * exp(D * gamma) + E * cos(gamma)^2)
 *
 *   For each channel Q in {Y, x, y}:
 *       Q = Q_zenith * F(theta, gamma) / F(0, theta_s)
 *
 *   theta  = view-zenith angle   (acos(view.up), clamped < pi/2)
 *   gamma  = view-sun angle      (acos(dot(view, sun)))
 *   theta_s= sun-zenith angle    (acos(sun.up),  clamped < pi/2)
 *
 *   Reassemble xyY, then xyY -> XYZ -> linear sRGB.
 * ------------------------------------------------------------------
 */

#include <jce/middleware/world/jce_sky.h>

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */

JceSkyConfig jce_sky_config_default(void)
{
    JceSkyConfig c;
    c.turbidity = 2.5f;   /* clear temperate day */
    c.exposure  = 1.0f;
    c.normalize = 0;
    return c;
}

/* ------------------------------------------------------------------ */

static float sky_clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float sky_dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Perez distribution function.  c is [A, B, C, D, E].
 * cos_theta in [eps, 1]; gamma in [0, pi]. */
static float sky_perez(const float c[5], float cos_theta, float gamma)
{
    float cg = cosf(gamma);
    return (1.0f + c[0] * expf(c[1] / cos_theta))
         * (1.0f + c[2] * expf(c[3] * gamma) + c[4] * cg * cg);
}

/* ------------------------------------------------------------------ */

JceSkyState jce_sky_evaluate(const JceSkyConfig *cfg_in, const float sun_dir[3])
{
    JceSkyConfig cfg = cfg_in ? *cfg_in : jce_sky_config_default();
    float T = sky_clampf(cfg.turbidity, 1.0f, 10.0f);

    JceSkyState st;

    /* Sun direction (normalized, defaulting to straight up). */
    float sx = 0.0f, sy = 1.0f, sz = 0.0f;
    if (sun_dir) {
        float len = sqrtf(sun_dir[0] * sun_dir[0] +
                          sun_dir[1] * sun_dir[1] +
                          sun_dir[2] * sun_dir[2]);
        if (len > 1e-6f) {
            sx = sun_dir[0] / len;
            sy = sun_dir[1] / len;
            sz = sun_dir[2] / len;
        }
    }
    st.sun_dir[0] = sx;
    st.sun_dir[1] = sy;
    st.sun_dir[2] = sz;

    /* Sun-zenith angle theta_s.  Clamp the up component just above 0 so
     * the sun never sits exactly on / below the horizon (keeps the
     * zenith luminance and the F(0,theta_s) normalizer finite). */
    float cos_ts = sky_clampf(sy, 0.01f, 1.0f);
    float theta_s = acosf(cos_ts);

    /* ---- Perez coefficients A..E (standard Preetham polynomials) ---- */
    st.perezY[0] =  0.1787f * T - 1.4630f;
    st.perezY[1] = -0.3554f * T + 0.4275f;
    st.perezY[2] = -0.0227f * T + 5.3251f;
    st.perezY[3] =  0.1206f * T - 2.5771f;
    st.perezY[4] = -0.0670f * T + 0.3703f;

    st.perezx[0] = -0.0193f * T - 0.2592f;
    st.perezx[1] = -0.0665f * T + 0.0008f;
    st.perezx[2] = -0.0004f * T + 0.2125f;
    st.perezx[3] = -0.0641f * T - 0.8989f;
    st.perezx[4] = -0.0033f * T + 0.0452f;

    st.perezy[0] = -0.0167f * T - 0.2608f;
    st.perezy[1] = -0.0950f * T + 0.0092f;
    st.perezy[2] = -0.0079f * T + 0.2102f;
    st.perezy[3] = -0.0441f * T - 1.6537f;
    st.perezy[4] = -0.0109f * T + 0.0529f;

    /* ---- Zenith luminance Yz (kcd/m^2), Preetham eq. ---------------- */
    float chi = (4.0f / 9.0f - T / 120.0f) * ((float)M_PI - 2.0f * theta_s);
    st.Yz = (4.0453f * T - 4.9710f) * tanf(chi) - 0.2155f * T + 2.4192f;
    if (st.Yz < 0.0f) st.Yz = 0.0f;

    /* ---- Zenith chromaticity xz, yz (polynomials in T, theta_s) ----- */
    float t1 = theta_s;
    float t2 = t1 * t1;
    float t3 = t2 * t1;
    float T2 = T * T;

    st.xz = ( 0.00166f * t3 - 0.00375f * t2 + 0.00209f * t1 + 0.0f)      * T2
          + (-0.02903f * t3 + 0.06377f * t2 - 0.03202f * t1 + 0.00394f)  * T
          + ( 0.11693f * t3 - 0.21196f * t2 + 0.06052f * t1 + 0.25886f);

    st.yz = ( 0.00275f * t3 - 0.00610f * t2 + 0.00317f * t1 + 0.0f)      * T2
          + (-0.04214f * t3 + 0.08970f * t2 - 0.04153f * t1 + 0.00516f)  * T
          + ( 0.15346f * t3 - 0.26756f * t2 + 0.06670f * t1 + 0.26688f);

    st.exposure  = cfg.exposure;
    st.normalize = cfg.normalize ? 1.0f : 0.0f;
    return st;
}

/* ------------------------------------------------------------------ */

void jce_sky_radiance(const JceSkyState *st, const float view_dir[3],
                      float out_rgb[3])
{
    if (!out_rgb) return;
    out_rgb[0] = out_rgb[1] = out_rgb[2] = 0.0f;
    if (!st || !view_dir) return;

    /* Normalize the view direction. */
    float vx = view_dir[0], vy = view_dir[1], vz = view_dir[2];
    float vlen = sqrtf(vx * vx + vy * vy + vz * vz);
    if (vlen > 1e-6f) { vx /= vlen; vy /= vlen; vz /= vlen; }

    /* theta = view-zenith angle.  Clamp the up component just above 0 so
     * cos(theta) never reaches 0 (the exp(B/cos(theta)) horizon term
     * would otherwise blow up).  Sky hemisphere only. */
    float cos_theta = sky_clampf(vy, 0.01f, 1.0f);

    /* gamma = angle between view and sun. */
    float view[3] = { vx, vy, vz };
    float cos_gamma = sky_clampf(sky_dot3(view, st->sun_dir), -1.0f, 1.0f);
    float gamma = acosf(cos_gamma);

    /* Sun-zenith reference angle theta_s. */
    float cos_ts = sky_clampf(st->sun_dir[1], 0.01f, 1.0f);
    float theta_s = acosf(cos_ts);

    /* F(0, theta_s): cos(0)=1, gamma=theta_s. */
    float fY0 = sky_perez(st->perezY, 1.0f, theta_s);
    float fx0 = sky_perez(st->perezx, 1.0f, theta_s);
    float fy0 = sky_perez(st->perezy, 1.0f, theta_s);
    if (fY0 == 0.0f) fY0 = 1e-6f;
    if (fx0 == 0.0f) fx0 = 1e-6f;
    if (fy0 == 0.0f) fy0 = 1e-6f;

    float Y = st->Yz * sky_perez(st->perezY, cos_theta, gamma) / fY0;
    float x = st->xz * sky_perez(st->perezx, cos_theta, gamma) / fx0;
    float y = st->yz * sky_perez(st->perezy, cos_theta, gamma) / fy0;

    if (Y < 0.0f) Y = 0.0f;

    /* Optional normalization: divide luminance by the zenith value so the
     * output scale is roughly turbidity-independent. */
    if (st->normalize != 0.0f) {
        float Yz = (st->Yz > 1e-6f) ? st->Yz : 1e-6f;
        Y /= Yz;
    }

    /* xyY -> XYZ.  Guard the y denominator. */
    if (y < 1e-4f) y = 1e-4f;
    float X = (x / y) * Y;
    float Z = ((1.0f - x - y) / y) * Y;

    /* XYZ -> linear sRGB (D65).  Mirrored exactly in fs_sky.sc. */
    float r =  3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z;
    float g = -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z;
    float b =  0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z;

    r *= st->exposure;
    g *= st->exposure;
    b *= st->exposure;

    out_rgb[0] = (r > 0.0f) ? r : 0.0f;
    out_rgb[1] = (g > 0.0f) ? g : 0.0f;
    out_rgb[2] = (b > 0.0f) ? b : 0.0f;
}

/* ── Sky-derived ambient (spherical harmonics) ─────────────────────── */

/* Real SH basis, bands 0..2, in the canonical order used by
 * jce_sky_project_sh9 / jce_sky_irradiance_sh9.  Index 2 is the "up" (+Y)
 * linear term, which is why an all-sky signal has a strong positive value
 * there and a downward normal receives much less. */
static void sky_sh9_basis(const float d[3], float y[9])
{
    const float x = d[0], u = d[1], z = d[2];
    y[0] = 0.282095f;                       /* l=0            */
    y[1] = 0.488603f * x;                   /* l=1, m=-1 (x)  */
    y[2] = 0.488603f * u;                   /* l=1, m= 0 (up) */
    y[3] = 0.488603f * z;                   /* l=1, m=+1 (z)  */
    y[4] = 1.092548f * x * u;               /* l=2            */
    y[5] = 1.092548f * u * z;
    y[6] = 0.315392f * (3.0f * u * u - 1.0f);
    y[7] = 1.092548f * x * z;
    y[8] = 0.546274f * (x * x - z * z);
}

void jce_sky_project_sh9(const JceSkyState *st, float out_sh9[9][3])
{
    if (!out_sh9) return;
    for (int i = 0; i < 9; i++)
        out_sh9[i][0] = out_sh9[i][1] = out_sh9[i][2] = 0.0f;
    if (!st) return;

    /* Fibonacci sphere: even coverage with no pole clustering and no RNG, so
     * the projection is deterministic by construction rather than by seeding.
     * Only the upper hemisphere carries sky radiance (see the header). */
    enum { SAMPLES = 512 };
    const float golden = 3.14159265358979f * (3.0f - 1.7320508f); /* pi*(3-sqrt5) */

    int used = 0;
    for (int i = 0; i < SAMPLES; i++) {
        /* u goes +1 -> -1 over the sphere; keep the upper half. */
        const float u = 1.0f - (2.0f * (float)i + 1.0f) / (float)SAMPLES;
        if (u <= 0.0f) continue;

        const float r = sqrtf(1.0f - u * u);
        const float a = golden * (float)i;
        const float d[3] = { cosf(a) * r, u, sinf(a) * r };

        float rad[3];
        jce_sky_radiance(st, d, rad);

        float basis[9];
        sky_sh9_basis(d, basis);
        for (int k = 0; k < 9; k++) {
            out_sh9[k][0] += rad[0] * basis[k];
            out_sh9[k][1] += rad[1] * basis[k];
            out_sh9[k][2] += rad[2] * basis[k];
        }
        used++;
    }
    if (used == 0) return;

    /* Monte-Carlo weight for a uniformly sampled hemisphere. */
    const float w = 6.28318530717959f / (float)used;   /* 2*pi / N */

    /* Per-band cosine-convolution constants (Ramamoorthi & Hanrahan), then
     * divide by pi so the result is the radiance a white Lambertian surface
     * emits rather than raw irradiance. */
    const float inv_pi = 1.0f / 3.14159265358979f;
    const float conv[9] = {
        3.14159265f * inv_pi,                                  /* l=0: pi   */
        2.09439510f * inv_pi, 2.09439510f * inv_pi,
        2.09439510f * inv_pi,                                  /* l=1: 2pi/3*/
        0.78539816f * inv_pi, 0.78539816f * inv_pi,
        0.78539816f * inv_pi, 0.78539816f * inv_pi,
        0.78539816f * inv_pi,                                  /* l=2: pi/4 */
    };

    for (int k = 0; k < 9; k++) {
        const float s = w * conv[k];
        out_sh9[k][0] *= s;
        out_sh9[k][1] *= s;
        out_sh9[k][2] *= s;
    }
}

void jce_sky_irradiance_sh9(const float sh9[9][3], const float n[3],
                            float out_rgb[3])
{
    if (!out_rgb) return;
    out_rgb[0] = out_rgb[1] = out_rgb[2] = 0.0f;
    if (!sh9 || !n) return;

    float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len < 1e-8f) len = 1.0f;
    const float d[3] = { n[0] / len, n[1] / len, n[2] / len };

    float basis[9];
    sky_sh9_basis(d, basis);

    float acc[3] = { 0.0f, 0.0f, 0.0f };
    for (int k = 0; k < 9; k++) {
        acc[0] += sh9[k][0] * basis[k];
        acc[1] += sh9[k][1] * basis[k];
        acc[2] += sh9[k][2] * basis[k];
    }

    /* An L2 fit can ring slightly negative near a sharp horizon.  Negative
     * light is never correct, so clamp rather than propagate it. */
    out_rgb[0] = acc[0] > 0.0f ? acc[0] : 0.0f;
    out_rgb[1] = acc[1] > 0.0f ? acc[1] : 0.0f;
    out_rgb[2] = acc[2] > 0.0f ? acc[2] : 0.0f;
}
