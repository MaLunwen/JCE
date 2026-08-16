/*
 * jce_ocean_spectrum.c -- JONSWAP / Pierson-Moskowitz / TMA + Donelan-Banner.
 *
 * See jce_ocean_spectrum.h for the equations; this file is their guarded
 * evaluation.  Everything is computed in double and narrowed once at the
 * return: the spectrum spans ~30 orders of magnitude across a realistic omega
 * sweep (w^-5 against a quartic exponential), and float loses the tail long
 * before the tail stops mattering to the amplitude sqrt().
 *
 * Two rails make "finite for any bounded input" a property rather than a hope:
 *   1. Every parameter passes through os_san(), which replaces non-finite
 *      values and clamps into a physically meaningful range.  A slider dragged
 *      to 0 or a serialized NaN must not be able to produce NaN geometry.
 *   2. S(w) is assembled in LOG space.  The naive product overflows/underflows
 *      independently in its two factors -- at small w, alpha*g^2/w^5 is +Inf
 *      while exp(-1.25*(wp/w)^4) is 0, and Inf*0 is NaN.  In log space the sum
 *      is -Inf and exp() of it is exactly 0, which is also the correct limit.
 */

#include "jce_ocean_spectrum.h"

#include <math.h>

#define OS_PI      3.14159265358979323846
#define OS_TWO_PI  6.28318530717958647692
#define OS_INV_2PI 0.15915494309189533577 /* isotropic spreading density */

/* Below this the spectrum is defined to be zero.  Any omega this small is far
 * outside the band an FFT patch can represent (w = 1e-6 rad/s is a 72-day
 * wave), and it keeps (wp/w)^4 away from double overflow. */
#define OS_OMEGA_MIN 1.0e-6

/* Hard rails on the log-space assembly.  exp(-700) underflows to 0 in double,
 * and exp(80) = 5.5e34 is still comfortably inside float range -- the upper
 * rail never binds for physical parameters (a 500 m/s wind over 1e9 m of fetch
 * peaks at S ~ 1e9), it exists so that no parameter combination can return Inf. */
#define OS_LN_MIN (-700.0)
#define OS_LN_MAX (80.0)

/* Composite-Simpson interval count for the swell normalisation integral over
 * [0, PI].  The integrand is a sech^2 core (beta <= 2.61, so >= 20 nodes across
 * its half-width) times cos^2s(theta/2) (s <= 16); 64 intervals put the
 * quadrature error far below float epsilon.  Must stay EVEN for Simpson. */
#define OS_QUAD_N 64

typedef struct {
    double u;      /* wind speed  */
    double f;      /* fetch       */
    double h;      /* depth, <= 0 means deep water */
    double g;      /* gravity     */
    double xi;     /* swell       */
    double delta;  /* directional blend */
    double wp;     /* peak angular frequency */
    double alpha;  /* Phillips-equivalent scale */
    int    type;
} OsCfg;

/* Sanitise one parameter: non-finite -> fallback, otherwise clamp.  Clamping
 * (rather than rejecting) is deliberate: an editor slider at its zero stop must
 * still produce a renderable, if degenerate, sea. */
static double os_san(double v, double lo, double hi, double fallback)
{
    if (!isfinite(v)) return fallback;
    if (v < lo)       return lo;
    if (v > hi)       return hi;
    return v;
}

static void os_cfg(const JceOceanSpectrumParams *p, OsCfg *c)
{
    double t;

    c->u     = os_san((double)p->wind_speed, 0.01, 500.0, 10.0);
    c->f     = os_san((double)p->fetch, 1.0, 1.0e9, 100000.0);
    c->g     = os_san((double)p->gravity, 0.01, 1000.0, 9.81);
    c->xi    = os_san((double)p->swell, 0.0, 1.0, 0.0);
    c->delta = os_san((double)p->directional_blend, 0.0, 1.0, 1.0);
    c->type  = (int)p->type;
    if (c->type < 0 || c->type > (int)JCE_OCEAN_SPECTRUM_TMA)
        c->type = (int)JCE_OCEAN_SPECTRUM_JONSWAP;

    /* depth is meaningful only for TMA; <= 0 (or non-finite) selects the
     * deep-water limit, where the Kitaigorodskii factor is identically 1. */
    t = (double)p->depth;
    c->h = (isfinite(t) && t > 0.0) ? (t > 1.0e5 ? 1.0e5 : t) : 0.0;

    /* wp = 22 * (g^2 / (U*F))^(1/3).  The cube root is load-bearing and is what
     * makes the expression dimensionally 1/s: g^2/(U*F) has units 1/s^3.
     * Without it, 10 m/s over 100 km would "peak" at 0.002 rad/s -- a
     * 50-minute period.  With it: 1.006 rad/s, a 6.2 s sea, which is right.
     * Fetch enters as F^(-1/3): quadrupling the fetch drops the peak ~37%. */
    c->wp = 22.0 * cbrt((c->g * c->g) / (c->u * c->f));

    /* alpha is dimensionless: U^2/(F*g) is the inverse dimensionless fetch. */
    c->alpha = 0.076 * pow((c->u * c->u) / (c->f * c->g), 0.22);

    /* Both are strictly positive for the clamped ranges above; the guard exists
     * so a future range change cannot silently divide by zero downstream. */
    if (!(c->wp > 0.0) || !isfinite(c->wp))       c->wp = 1.0;
    if (!(c->alpha > 0.0) || !isfinite(c->alpha)) c->alpha = 0.0081;
}

/* Kitaigorodskii depth attenuation, the TMA factor.  Shallow water cannot hold
 * the long-wave energy a deep-water JONSWAP predicts; phi rolls it off below
 * the depth-dependent cutoff and is exactly 1 above it, so TMA <= JONSWAP for
 * every omega.  wh = w*sqrt(h/g) is the dimensionless depth-frequency. */
static double os_kitaigorodskii(double w, double h, double g)
{
    double wh, r;
    if (!(h > 0.0)) return 1.0; /* deep water */
    wh = w * sqrt(h / g);
    if (wh <= 1.0) return 0.5 * wh * wh;
    if (wh >= 2.0) return 1.0;
    r = 2.0 - wh;
    return 1.0 - 0.5 * r * r;
}

static double os_energy_d(const OsCfg *c, double w)
{
    double sigma, dev, r, gamma, ln_s, s;

    /* Catches NaN as well: every comparison against NaN is false. */
    if (!(w > OS_OMEGA_MIN)) return 0.0;

    gamma = (c->type == (int)JCE_OCEAN_SPECTRUM_PM) ? 1.0 : 3.3;

    /* The sigma step at w == wp is a discontinuity in the *derivative* of r
     * only (r == 1 there for either sigma), so S itself stays smooth. */
    sigma = (w <= c->wp) ? 0.07 : 0.09;
    dev   = (w - c->wp) / (sigma * c->wp);
    r     = exp(-0.5 * dev * dev);

    ln_s = log(c->alpha) + 2.0 * log(c->g) - 5.0 * log(w)
         - 1.25 * pow(c->wp / w, 4.0)
         + r * log(gamma);

    if (!(ln_s > OS_LN_MIN)) return 0.0;   /* underflow, or NaN from a rogue w */
    if (ln_s > OS_LN_MAX) ln_s = OS_LN_MAX;
    s = exp(ln_s);

    if (c->type == (int)JCE_OCEAN_SPECTRUM_TMA)
        s *= os_kitaigorodskii(w, c->h, c->g);

    if (!isfinite(s) || s < 0.0) return 0.0;
    return s;
}

/* sech^2(x) = 1/cosh^2(x).  cosh overflows to +Inf around |x| = 710 and squares
 * to Inf long before, so clamp: at |x| = 40 the true value is 1.6e-35, i.e.
 * zero to every consumer of this module.  Returning 0 keeps the result finite
 * where 1/(Inf*Inf) would be a platform-dependent 0-or-NaN. */
static double os_sech2(double x)
{
    double ch;
    if (!(x > -40.0 && x < 40.0)) return 0.0;
    ch = cosh(x);
    return 1.0 / (ch * ch);
}

/* Donelan-Banner spreading exponent beta(w).  Three regimes around the peak:
 * broad below it, sharpest at it, broadening again in the tail (beta tends to
 * 10^-0.4 = 0.398 as w/wp -> inf, i.e. an almost isotropic capillary range). */
static double os_db_beta(double rw)
{
    double eps;
    if (!(rw > 0.0) || !isfinite(rw)) return 0.0;
    if (rw < 0.95) return 2.61 * pow(rw, 1.3);
    if (rw < 1.6)  return 2.28 * pow(rw, -1.3);
    eps = 0.8393 * exp(-0.567 * log(rw * rw)) - 0.4;
    return pow(10.0, eps);
}

/* Unnormalised directional shape: the Donelan-Banner sech^2 lobe times the
 * swell cos^2s(theta/2) lobe.  Multiplying (rather than blending) the two is
 * what makes swell NARROW the sea: a product of two densities is never wider
 * than either factor. */
static double os_dir_shape(double beta, double s2, double theta)
{
    double d = 0.5 * beta * os_sech2(beta * theta);
    if (s2 > 0.0) {
        double c = fabs(cos(0.5 * theta));
        /* cos is exactly 0 at theta = +-PI; pow(0, positive) is 0, which is the
         * correct "no energy directly upwind" answer. */
        d *= pow(c, s2);
    }
    return d;
}

static double os_spreading_d(const OsCfg *c, double w, double theta)
{
    double rw, beta, s2, dir, z, h, acc;
    int i;

    /* Wrap into [-PI, PI].  remainder() is exact (no cancellation for large
     * theta) and keeps D symmetric bit-for-bit. */
    theta = isfinite(theta) ? remainder(theta, OS_TWO_PI) : 0.0;

    if (!(w > OS_OMEGA_MIN)) return OS_INV_2PI;

    rw   = w / c->wp;
    beta = os_db_beta(rw);
    if (beta > 1.0e3) beta = 1.0e3;

    /* swell exponent 2s, s = 16*tanh(wp/w)*xi^2.  tanh saturates at 1, so s is
     * capped at 16 and the exponent at 32; the argument is clamped only to keep
     * tanh away from a pointless huge input. */
    s2 = 0.0;
    if (c->xi > 0.0) {
        double a = 1.0 / rw;
        if (!(a < 20.0)) a = 20.0;
        s2 = 2.0 * 16.0 * tanh(a) * c->xi * c->xi;
    }

    if (!(beta > 1.0e-4)) {
        /* beta -> 0 limit: 0.5*beta*sech^2(beta*theta) / tanh(beta*PI) tends to
         * 0.5*beta / (beta*PI) = 1/(2*PI).  Taking the limit analytically
         * avoids the 0/0 the literal expression would evaluate. */
        dir = OS_INV_2PI;
    } else if (s2 <= 0.0) {
        /* Pure Donelan-Banner: the normaliser is closed form, because
         * integral of 0.5*beta*sech^2(beta*t) over [-PI,PI] == tanh(beta*PI).
         * Q = 1/tanh(beta*PI) is exactly the paper's normalisation. */
        dir = os_dir_shape(beta, 0.0, theta) / tanh(beta * OS_PI);
    } else {
        /* With the swell lobe multiplied in there is no closed-form integral,
         * so normalise by composite Simpson over [0,PI] and double (the shape
         * is even in theta).  This runs once per wavevector at build time. */
        h   = OS_PI / (double)OS_QUAD_N;
        acc = os_dir_shape(beta, s2, 0.0) + os_dir_shape(beta, s2, OS_PI);
        for (i = 1; i < OS_QUAD_N; ++i) {
            double wgt = (i & 1) ? 4.0 : 2.0;
            acc += wgt * os_dir_shape(beta, s2, (double)i * h);
        }
        z = 2.0 * acc * h / 3.0;
        if (!(z > 1.0e-30) || !isfinite(z))
            dir = OS_INV_2PI; /* degenerate lobe: fall back to isotropic */
        else
            dir = os_dir_shape(beta, s2, theta) / z;
    }

    if (!isfinite(dir) || dir < 0.0) dir = 0.0;

    /* Convex blend of two densities that each integrate to 1, so the result
     * integrates to 1 for any delta -- the invariant the caller relies on when
     * it treats S and D as independent controls. */
    dir = (1.0 - c->delta) * OS_INV_2PI + c->delta * dir;
    if (!isfinite(dir) || dir < 0.0) dir = 0.0;
    return dir;
}

void jce_ocean_spectrum_params_default(JceOceanSpectrumParams *p)
{
    if (!p) return;
    p->type              = JCE_OCEAN_SPECTRUM_JONSWAP;
    p->wind_speed        = 10.0f;
    p->fetch             = 100000.0f;
    p->depth             = 50.0f;
    p->gravity           = 9.81f;
    p->swell             = 0.2f;
    p->directional_blend = 1.0f;
}

float jce_ocean_spectrum_peak_omega(const JceOceanSpectrumParams *p)
{
    OsCfg c;
    if (!p) return 0.0f;
    os_cfg(p, &c);
    return (float)c.wp;
}

float jce_ocean_spectrum_energy(const JceOceanSpectrumParams *p, float omega)
{
    OsCfg c;
    if (!p) return 0.0f;
    os_cfg(p, &c);
    return (float)os_energy_d(&c, (double)omega);
}

float jce_ocean_spectrum_spreading(const JceOceanSpectrumParams *p,
                                   float omega, float theta)
{
    OsCfg c;
    if (!p) return 0.0f;
    os_cfg(p, &c);
    return (float)os_spreading_d(&c, (double)omega, (double)theta);
}

float jce_ocean_spectrum_amplitude(const JceOceanSpectrumParams *p,
                                   float kx, float kz,
                                   float wind_dir_x, float wind_dir_z)
{
    OsCfg  c;
    double dkx, dkz, k, w, dwdk, theta, wlen, a2;

    if (!p) return 0.0f;
    dkx = (double)kx;
    dkz = (double)kz;
    if (!isfinite(dkx) || !isfinite(dkz)) return 0.0f;

    k = sqrt(dkx * dkx + dkz * dkz);
    /* k == 0 is the DC term: a constant height offset, not a wave.  The 1/k
     * factor is singular there and the physical answer is zero energy. */
    if (!(k > OS_OMEGA_MIN)) return 0.0f;

    os_cfg(p, &c);

    /* Deep-water dispersion and its Jacobian.  dw/dk = g/(2*w) = 0.5*sqrt(g/k)
     * converts the frequency-space density S(w) into wavenumber space; omitting
     * it is the classic error that makes the small waves too strong. */
    w    = sqrt(c.g * k);
    dwdk = 0.5 * sqrt(c.g / k);

    wlen = sqrt((double)wind_dir_x * (double)wind_dir_x +
                (double)wind_dir_z * (double)wind_dir_z);
    if (!(wlen > 1.0e-12) || !isfinite(wlen))
        theta = atan2(dkz, dkx);                    /* degenerate wind: use +X */
    else
        theta = atan2(dkz, dkx) - atan2((double)wind_dir_z, (double)wind_dir_x);

    a2 = (4.0 * OS_PI / k) * os_energy_d(&c, w) * os_spreading_d(&c, w, theta)
       * dwdk;

    if (!(a2 > 0.0) || !isfinite(a2)) return 0.0f;
    return (float)sqrt(a2);
}