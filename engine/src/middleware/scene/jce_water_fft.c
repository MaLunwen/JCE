/*
 * jce_water_fft.c -- Tessendorf statistical FFT ocean (deep-water spectrum).
 *
 * See jce_water_fft.h for the exact equations (the single source of truth the
 * headless test and the water vertex shader's FFT branch mirror).  Pure math:
 * depends only on the C math library + the engine allocator (no bgfx, flecs, or
 * globals), so a given (N, patch_size, wind, amplitude, seed) is fully
 * reproducible across runs and platforms.
 *
 * ── FFT convention ──────────────────────────────────────────────────────
 * The hand-rolled radix-2 transform (fft_radix2) takes a `sign` argument:
 *   sign = -1  : FORWARD transform,  X[k] = sum_n x[n] * exp(-2*PI*i * k*n / N)
 *   sign = +1  : INVERSE transform,  x[n] = sum_k X[k] * exp(+2*PI*i * k*n / N)
 * NEITHER direction normalizes; the caller divides by N (1D) / (N*N) (2D) for a
 * true inverse.  The 2D transform is separable: transform every row, then every
 * column, with the same sign.  Internally double precision is used for the
 * spectrum + FFT (numerical headroom for large N); the output fields are float
 * (the shader is float and never sees a double in the hot path).
 */

#include <jce/middleware/scene/jce_water_fft.h>

#include "jce_ocean_spectrum.h"

#include "os/core/jce_memory.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define WFFT_TWO_PI 6.28318530717958647692
#define WFFT_GRAVITY 9.81

/* A double-precision complex sample (re, im).  Plain struct (no C99 _Complex) so
 * the math is explicit and bit-identical across compilers/backends. */
typedef struct { double re, im; } WfftCpx;

struct JceWaterFft {
    int          N;             /* grid resolution (power of two)               */
    float        patch_size;    /* world-space tile side length L               */
    float        amplitude;     /* Phillips energy scale                        */
    float        wind_speed;    /* m/s                                          */
    float        wind_dir[2];   /* normalized wind direction                    */

    /* Immutable initial spectrum, indexed [m*N + n] (m,n in [0,N)).            */
    WfftCpx     *h0;            /* h0(k)                                         */
    WfftCpx     *h0_conj;       /* conj(h0(-k))                                  */
    double      *omega;         /* dispersion w(k) = sqrt(g*|k|)                 */

    /* Per-evolve frequency-domain field + its IFFT scratch (N*N each).         */
    WfftCpx     *hkt;          /* h(k,t)                                        */
    /* HORIZONTAL DISPLACEMENT, not slope.  These were named slope_x/slope_z,
     * which is what they look like and is not what they are: the field built
     * below is -i*(k/|k|)*h, Tessendorf's choppy displacement vector D, and it
     * is packed straight into disp_x/disp_z.  The gradient of h is a DIFFERENT
     * transform (i*k*h, eq. 37).  A reader who needed slopes and reached for
     * the field called "slope" would have got a plausible, wrong answer. */
    WfftCpx     *dispf_x;      /* D_x(k) frequency field then spatial           */
    WfftCpx     *dispf_z;      /* D_z(k)                                        */
    /* TRUE gradient of h (Tessendorf eq. 37): i*k*h.  Optional, because it
     * costs two more inverse transforms per evolve -- a two-thirds increase in
     * FFT work -- and a caller that only needs heights should not pay it.
     * NULL unless requested.  Central differences of the height field are a
     * cheap substitute but they are wrong exactly where it shows: at a sharp
     * crest, where the finite difference straddles the peak and flattens it. */
    WfftCpx     *slopef_x;
    WfftCpx     *slopef_z;
    float       *slope_x;      /* spatial dh/dx, NULL when not requested        */
    float       *slope_z;
    int          want_slopes;
    /* Jacobian of the horizontal map x -> x + D(x).  Below 1 the surface is
     * being COMPRESSED; below 0 it has folded over itself, which is what a
     * breaking crest physically is -- so this is the foam mask, derived rather
     * than painted.  NULL unless requested. */
    float       *foam;
    int          want_foam;
    float        last_time;    /* time of the most recent evolve               */
    WfftCpx     *fft_row;      /* 1D scratch (length N) reused per row/column   */
    WfftCpx     *twiddle;      /* N-1 precomputed factors; see wfft_twiddle_build */

    /* Spatial-domain output fields, row-major index = z*N + x (float).         */
    float       *height;
    float       *disp_x;
    float       *disp_z;

    int          evolved;       /* fields populated at least once               */

    /* Modern spectrum (JONSWAP / PM / TMA + Donelan-Banner + swell).  Off by
     * default so existing scenes keep their exact surface until someone opts
     * in and rebaselines. */
    int                    use_modern_spectrum;
    JceOceanSpectrumParams spectrum;
};

/* ── Deterministic PRNG (PCG32) + Box-Muller Gaussian ───────────────────
 * A small, self-contained, seedable generator so the spectrum is reproducible
 * with no dependency on platform rand().  PCG32 (O'Neill 2014): good statistical
 * quality, deterministic, no globals. */
typedef struct { uint64_t state, inc; } WfftRng;

static void wfft_rng_seed(WfftRng *r, uint64_t seed, uint64_t seq)
{
    r->state = 0u;
    r->inc   = (seq << 1u) | 1u;
    /* advance once with the seed mixed in (canonical PCG seeding) */
    r->state = r->state * 6364136223846793005ULL + r->inc;
    r->state += seed;
    r->state = r->state * 6364136223846793005ULL + r->inc;
}

static uint32_t wfft_rng_u32(WfftRng *r)
{
    uint64_t old = r->state;
    r->state = old * 6364136223846793005ULL + r->inc;
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-(int)rot) & 31));
}

/* Uniform double in (0,1] — never returns exactly 0 (safe for log()). */
static double wfft_rng_unit(WfftRng *r)
{
    /* 53-bit mantissa from two draws, then nudge off zero. */
    uint64_t hi = (uint64_t)wfft_rng_u32(r) >> 5;   /* 27 bits */
    uint64_t lo = (uint64_t)wfft_rng_u32(r) >> 6;   /* 26 bits */
    double u = (double)((hi << 26) | lo) / 9007199254740992.0; /* /2^53 */
    if (u <= 0.0) u = 1.0 / 9007199254740992.0;
    return u;
}

/* Box-Muller: two independent N(0,1) Gaussians per call. */
static void wfft_gaussian2(WfftRng *r, double *out0, double *out1)
{
    double u1 = wfft_rng_unit(r);
    double u2 = wfft_rng_unit(r);
    double mag = sqrt(-2.0 * log(u1));
    *out0 = mag * cos(WFFT_TWO_PI * u2);
    *out1 = mag * sin(WFFT_TWO_PI * u2);
}

/* ── Power-of-two check (N>0 and a single set bit) ──────────────────────── */
static int wfft_is_pow2(int n)
{
    return n > 0 && (n & (n - 1)) == 0;
}

/* ── Hand-rolled radix-2 in-place complex FFT (Cooley-Tukey, DIT) ────────
 * `sign` selects direction (see file header).  NOT normalized.  `n` MUST be a
 * power of two (callers guarantee it).  Bit-reversal permutation followed by
 * log2(n) butterfly stages. */
/* Twiddle table for one transform size.
 *
 * The butterfly loop used to advance its twiddle by complex multiplication,
 * cw *= w, which is both slower and progressively WRONG: the rounding error of
 * every multiply accumulates along the stage.  A table is faster and more
 * accurate at once.
 *
 * WHAT WAS ACTUALLY MEASURED, and what was not.  In an -O2 microbenchmark of
 * the transform alone: round-trip error at N=256 fell from 6.2e-15 to 5.6e-16
 * (11x) and the transform ran ~45% faster, repeatably.  An end-to-end A/B of
 * jce_water_fft_evolve in a DEBUG build showed no difference at all -- evolve
 * there is dominated by the unoptimised spectrum update, so that measurement
 * says nothing either way and is not claimed as a win.  The ACCURACY gain is
 * unconditional and is on its own sufficient reason to prefer the table.
 *
 * This was measured before it was written, after a blocked-transpose
 * experiment that looked obviously right, produced bit-identical output, and
 * turned out to be slightly SLOWER.
 *
 * Layout: stages concatenated, `len/2` entries for len = 2,4,...,n, so the
 * whole table is exactly n-1 entries.  Stored for the POSITIVE angle; the
 * inverse negates the imaginary part, since w(-a) = conj(w(a)). */
#define WFFT_MAX_N 4096   /* bounds the stack table in the fft_radix2 wrapper */

static void wfft_twiddle_build(WfftCpx *tw, int n)
{
    int off = 0;
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len / 2;
        for (int k = 0; k < half; ++k) {
            const double ang = WFFT_TWO_PI * (double)k / (double)len;
            tw[off + k].re = cos(ang);
            tw[off + k].im = sin(ang);
        }
        off += half;
    }
}

/* In-place radix-2 FFT with a precomputed twiddle table.  `n` must be a power
 * of two (callers guarantee it) and `tw` must hold n-1 entries built by
 * wfft_twiddle_build for the same n. */
static void fft_radix2_tw(WfftCpx *a, int n, int sign, const WfftCpx *tw)
{
    /* --- bit-reversal permutation --- */
    int j = 0;
    for (int i = 1; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            WfftCpx tmp = a[i];
            a[i] = a[j];
            a[j] = tmp;
        }
    }

    /* --- butterflies --- */
    int off = 0;
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len / 2;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; ++k) {
                const double cwr = tw[off + k].re;
                const double cwi = (sign > 0) ? tw[off + k].im
                                              : -tw[off + k].im;
                WfftCpx u = a[i + k];
                WfftCpx v;
                v.re = a[i + k + half].re * cwr - a[i + k + half].im * cwi;
                v.im = a[i + k + half].re * cwi + a[i + k + half].im * cwr;
                a[i + k].re        = u.re + v.re;
                a[i + k].im        = u.im + v.im;
                a[i + k + half].re = u.re - v.re;
                a[i + k + half].im = u.im - v.im;
            }
        }
        off += half;
    }
}

/* Convenience wrapper that builds a table on the stack.  Used by tests and any
 * one-off transform; the hot path passes the field's cached table instead. */
static void fft_radix2(WfftCpx *a, int n, int sign)
{
    WfftCpx tw[WFFT_MAX_N];
    if (n < 2 || n > WFFT_MAX_N) return;   /* never index off the stack table */
    wfft_twiddle_build(tw, n);
    fft_radix2_tw(a, n, sign, tw);
}

/* Separable inverse 2D FFT of `field` (N*N, row-major [row*N + col]) in place.
 * Transforms each row then each column with sign=+1 and applies the 1/(N*N)
 * normalization so the result is a true inverse.  `scratch` is a length-N
 * complex buffer the caller owns (reused).
 *
 * MEASURED, NOT ASSUMED: the per-column gather/scatter below looks like the
 * classic cache disaster -- at N=256 a column strides 4 KB per element -- so a
 * blocked in-place transpose was written to replace it.  It produced
 * bit-identical output and was 2-4% SLOWER at N=256, with run-to-run variance
 * at N=64/128 too large to call a win either way.  `scratch` is only N complex
 * values (4 KB at N=256), so it stays hot in L1 and the strided reads prefetch
 * acceptably, while two full transposes add a complete extra pass of
 * pointer-chasing swaps.  The transpose was reverted rather than shipped with a
 * rationale its own measurement contradicts.
 *
 * The real remaining win is Hermitian symmetry -- the output field is REAL, so
 * half the complex transform is redundant -- a numerical change, not a
 * memory-layout one.
 */
static void ifft2d(WfftCpx *field, int N, WfftCpx *scratch, const WfftCpx *tw)
{
    /* rows */
    for (int row = 0; row < N; ++row)
        fft_radix2_tw(&field[row * N], N, +1, tw);

    /* columns (gather into scratch, transform, scatter back) */
    for (int col = 0; col < N; ++col) {
        for (int row = 0; row < N; ++row)
            scratch[row] = field[row * N + col];
        fft_radix2_tw(scratch, N, +1, tw);
        for (int row = 0; row < N; ++row)
            field[row * N + col] = scratch[row];
    }

    /* normalization for the inverse transform */
    double inv = 1.0 / ((double)N * (double)N);
    for (int i = 0; i < N * N; ++i) {
        field[i].re *= inv;
        field[i].im *= inv;
    }
}

static void wfft_build_h0(JceWaterFft *f, unsigned int seed);

/* Phillips spectrum energy at wavevector (kx, kz).  Returns 0 for k==0. */
static double wfft_phillips(const JceWaterFft *f, double kx, double kz)
{
    double k2 = kx * kx + kz * kz;
    if (k2 < 1e-12) return 0.0;
    double k = sqrt(k2);

    double L_w = (double)f->wind_speed * (double)f->wind_speed / WFFT_GRAVITY;
    if (L_w < 1e-9) return 0.0;

    double khx = kx / k, khz = kz / k;             /* unit wavevector */
    double wd = khx * (double)f->wind_dir[0] + khz * (double)f->wind_dir[1];
    double dir_factor = wd * wd;                    /* |dot(khat,windhat)|^2 */

    double kL = k * L_w;
    double ph = (double)f->amplitude * exp(-1.0 / (kL * kL)) / (k2 * k2) * dir_factor;

    /* small-wave damping: suppress capillary-scale waves (l = L_w / 1000) */
    double l = L_w * 1e-3;
    ph *= exp(-k2 * l * l);

    return ph;
}

/* Map grid index i in [0,N) to the signed mode m in [-N/2, N/2). */
static int wfft_mode(int i, int N)
{
    return i - N / 2;
}

JceWaterFft *JCE_CALL
jce_water_fft_create(int N, float patch_size,
                     float wind_speed, float wind_dir_x, float wind_dir_z,
                     float amplitude, unsigned int seed)
{
    if (!wfft_is_pow2(N))      return NULL;
    if (patch_size <= 0.0f)    return NULL;
    if (amplitude <= 0.0f)     return NULL;
    if (wind_speed <= 0.0f)    return NULL;

    JceWaterFft *f = (JceWaterFft *)JCE_CALLOC(1, sizeof(JceWaterFft));
    if (!f) return NULL;

    f->N          = N;
    f->patch_size = patch_size;
    f->amplitude  = amplitude;
    f->wind_speed = wind_speed;

    /* normalize wind direction (zero-length -> +X so the spectrum is defined) */
    {
        double wl = (double)wind_dir_x * wind_dir_x + (double)wind_dir_z * wind_dir_z;
        if (wl <= 1e-12) {
            f->wind_dir[0] = 1.0f;
            f->wind_dir[1] = 0.0f;
        } else {
            double inv = 1.0 / sqrt(wl);
            f->wind_dir[0] = (float)(wind_dir_x * inv);
            f->wind_dir[1] = (float)(wind_dir_z * inv);
        }
    }

    size_t cells = (size_t)N * (size_t)N;
    f->h0      = (WfftCpx *)JCE_MALLOC(cells * sizeof(WfftCpx));
    f->h0_conj = (WfftCpx *)JCE_MALLOC(cells * sizeof(WfftCpx));
    f->omega   = (double  *)JCE_MALLOC(cells * sizeof(double));
    f->hkt     = (WfftCpx *)JCE_MALLOC(cells * sizeof(WfftCpx));
    f->dispf_x = (WfftCpx *)JCE_MALLOC(cells * sizeof(WfftCpx));
    f->dispf_z = (WfftCpx *)JCE_MALLOC(cells * sizeof(WfftCpx));
    f->fft_row = (WfftCpx *)JCE_MALLOC((size_t)N * sizeof(WfftCpx));
    /* n-1 entries exactly: n/2 + n/4 + ... + 1.  Allocated at N so a future
     * radix change cannot silently overrun it. */
    f->twiddle = (WfftCpx *)JCE_MALLOC((size_t)N * sizeof(WfftCpx));
    f->height  = (float   *)JCE_MALLOC(cells * sizeof(float));
    f->disp_x  = (float   *)JCE_MALLOC(cells * sizeof(float));
    f->disp_z  = (float   *)JCE_MALLOC(cells * sizeof(float));

    if (!f->h0 || !f->h0_conj || !f->omega || !f->hkt || !f->dispf_x ||
        !f->dispf_z || !f->fft_row || !f->twiddle || !f->height ||
        !f->disp_x || !f->disp_z) {
        jce_water_fft_destroy(f);
        return NULL;
    }

    /* Build the twiddle table once.  Every transform reads it, so leaving it
     * uninitialised would feed garbage into all three fields. */
    wfft_twiddle_build(f->twiddle, N);

    /* Zero outputs so an accessor read before the first evolve is well-defined. */
    memset(f->height, 0, cells * sizeof(float));
    memset(f->disp_x, 0, cells * sizeof(float));
    memset(f->disp_z, 0, cells * sizeof(float));

    wfft_build_h0(f, seed);
    return f;
}

/* Build h0(k), conj(h0(-k)) and the dispersion omega(k).
 *
 * Factored out of create() so the spectrum can be switched after construction
 * and the field rebuilt in place.  The Gaussian draws walk the grid in a FIXED
 * [m][n] order, so the seed alone determines the spectrum -- that is what makes
 * the surface reproducible across runs and platforms, and it must not be
 * reordered. */
static void wfft_build_h0(JceWaterFft *f, unsigned int seed)
{
    const int N = f->N;
    WfftRng rng;
    wfft_rng_seed(&rng, (uint64_t)seed, 0x9E3779B97F4A7C15ULL);

    const double Lp = (double)f->patch_size;

    /* First pass: draw h0(k) and omega(k) for every cell. */
    for (int m = 0; m < N; ++m) {
        for (int n = 0; n < N; ++n) {
            int idx = m * N + n;
            int km = wfft_mode(m, N);
            int kn = wfft_mode(n, N);
            double kx = WFFT_TWO_PI * (double)km / Lp;
            double kz = WFFT_TWO_PI * (double)kn / Lp;

            /* Spectrum selection.
             *
             * Phillips has no fetch parameter, no peak-frequency control and
             * poor high-k convergence -- which is why this file already bolts
             * on an ad-hoc exp(-k^2 l^2) damping patch.  Nobody ships it any
             * more.  JONSWAP with a fetch, Donelan-Banner directional
             * spreading and a swell parameter is what production uses, and it
             * costs NOTHING at runtime: this loop runs once at creation, never
             * per frame.
             *
             * Phillips stays reachable because switching the spectrum changes
             * every height value, so any golden-image or hash baseline over
             * the water field is a deliberate rebaseline, not a side effect of
             * a build. */
            double root;
            if (f->use_modern_spectrum) {
                const float amp = jce_ocean_spectrum_amplitude(
                    &f->spectrum, (float)kx, (float)kz,
                    f->wind_dir[0], f->wind_dir[1]);
                /* The module deliberately omits the 1/sqrt(L) patch factor:
                 * patch size is a property of this FFT grid, not of the sea
                 * state.  Applying it here keeps the module reusable. */
                root = ((double)amp / sqrt(Lp)) * 0.70710678118654752440;
            } else {
                double ph = wfft_phillips(f, kx, kz);
                root = sqrt(ph) * 0.70710678118654752440; /* 1/sqrt(2) */
            }

            double xi_r, xi_i;
            wfft_gaussian2(&rng, &xi_r, &xi_i);
            f->h0[idx].re = root * xi_r;
            f->h0[idx].im = root * xi_i;

            double kk = sqrt(kx * kx + kz * kz);
            f->omega[idx] = sqrt(WFFT_GRAVITY * kk);
        }
    }

    /* Second pass: conj(h0(-k)).  The wavevector -k is the DFT mirror of the
     * array index, (N-m)%N / (N-n)%N (NOT (N-1-m)): with this exact mirror the
     * dispersion omega(k)=omega(-k) is symmetric under the SAME index map, so
     * h(k,t) is exactly Hermitian-symmetric and the inverse FFT output is real
     * to round-off (no Nyquist-row imaginary leak).  See the realness proof in
     * the module notes / test_jce_water_fft. */
    for (int m = 0; m < N; ++m) {
        for (int n = 0; n < N; ++n) {
            int idx  = m * N + n;
            int mm   = (N - m) % N;
            int nn   = (N - n) % N;
            int midx = mm * N + nn;
            f->h0_conj[idx].re =  f->h0[midx].re;
            f->h0_conj[idx].im = -f->h0[midx].im;   /* conjugate */
        }
    }
}

bool JCE_CALL jce_water_fft_use_jonswap(JceWaterFft *f, float wind_speed,
                                        float fetch, float swell,
                                        unsigned int seed)
{
    /* Refuse after the first evolve: h0 is the field's identity, and swapping
     * it mid-flight would teleport every wave.  Callers configure once. */
    if (!f || f->evolved) return false;

    jce_ocean_spectrum_params_default(&f->spectrum);
    f->spectrum.wind_speed = wind_speed;
    f->spectrum.fetch      = fetch;
    f->spectrum.swell      = swell;
    f->use_modern_spectrum = 1;

    /* The FFT keeps its own wind speed for the Phillips path; keep the two in
     * step so a later read of either agrees. */
    f->wind_speed = wind_speed;

    wfft_build_h0(f, seed);
    return true;
}

void JCE_CALL jce_water_fft_destroy(JceWaterFft *f)
{
    if (!f) return;
    JCE_FREE(f->h0);
    JCE_FREE(f->h0_conj);
    JCE_FREE(f->omega);
    JCE_FREE(f->hkt);
    JCE_FREE(f->foam);
    JCE_FREE(f->slopef_x);
    JCE_FREE(f->slopef_z);
    JCE_FREE(f->slope_x);
    JCE_FREE(f->slope_z);
    JCE_FREE(f->dispf_x);
    JCE_FREE(f->dispf_z);
    JCE_FREE(f->fft_row);
    JCE_FREE(f->twiddle);
    JCE_FREE(f->height);
    JCE_FREE(f->disp_x);
    JCE_FREE(f->disp_z);
    JCE_FREE(f);
}

void JCE_CALL jce_water_fft_evolve(JceWaterFft *f, float t)
{
    if (!f) return;
    const int N = f->N;
    const double Lp = (double)f->patch_size;
    const double td = (double)t;

    /* --- Build the time-evolved frequency fields h(k,t) and D_{x,z}(k). --- */
    for (int m = 0; m < N; ++m) {
        for (int n = 0; n < N; ++n) {
            int idx = m * N + n;

            /* h(k,t) = h0(k)*exp(i*w*t) + conj(h0(-k))*exp(-i*w*t) */
            double w = f->omega[idx];
            double cw = cos(w * td);
            double sw = sin(w * td);

            /* exp(+i*w*t) = (cw, sw); exp(-i*w*t) = (cw, -sw) */
            WfftCpx a = f->h0[idx];
            WfftCpx b = f->h0_conj[idx];
            double re = a.re * cw - a.im * sw   /* h0 * exp(+iwt) */
                      + b.re * cw + b.im * sw;  /* h0c * exp(-iwt) */
            double im = a.re * sw + a.im * cw
                      - b.re * sw + b.im * cw;
            f->hkt[idx].re = re;
            f->hkt[idx].im = im;

            /* Horizontal displacement: D(k) = -i * (k/|k|) * h(k,t).
             * -i*(c) for complex c=(re,im) is (im, -re); scale by k/|k|. */
            int km = wfft_mode(m, N);
            int kn = wfft_mode(n, N);
            double kx = WFFT_TWO_PI * (double)km / Lp;
            double kz = WFFT_TWO_PI * (double)kn / Lp;
            double klen = sqrt(kx * kx + kz * kz);

            if (klen < 1e-9) {
                f->dispf_x[idx].re = 0.0; f->dispf_x[idx].im = 0.0;
                f->dispf_z[idx].re = 0.0; f->dispf_z[idx].im = 0.0;
            } else {
                double khx = kx / klen, khz = kz / klen;
                /* -i*h = (im, -re) */
                double mix = im, mre = -re;
                f->dispf_x[idx].re = khx * mix; f->dispf_x[idx].im = khx * mre;
                f->dispf_z[idx].re = khz * mix; f->dispf_z[idx].im = khz * mre;
            }

            /* Gradient (eq. 37): i*k*h.  i*(re,im) = (-im, re), scaled by the
             * FULL wavenumber -- not the unit vector the displacement uses.
             * That distinction is the whole difference between the two fields
             * and is why one cannot be derived from the other. */
            if (f->want_slopes) {
                f->slopef_x[idx].re = -kx * im; f->slopef_x[idx].im = kx * re;
                f->slopef_z[idx].re = -kz * im; f->slopef_z[idx].im = kz * re;
            }
        }
    }

    /* --- Inverse 2D FFTs to the spatial domain. --- */
    ifft2d(f->hkt,     N, f->fft_row, f->twiddle);
    ifft2d(f->dispf_x, N, f->fft_row, f->twiddle);
    ifft2d(f->dispf_z, N, f->fft_row, f->twiddle);
    if (f->want_slopes) {
        ifft2d(f->slopef_x, N, f->fft_row, f->twiddle);
        ifft2d(f->slopef_z, N, f->fft_row, f->twiddle);
    }

    /* --- Pack real parts into the float output fields with the (-1)^(x+z)
     * sign flip (fftshift) so the patch is centred and tiles seamlessly. --- */
    for (int z = 0; z < N; ++z) {
        for (int x = 0; x < N; ++x) {
            int idx = z * N + x;
            double sgn = ((x + z) & 1) ? -1.0 : 1.0;
            f->height[idx] = (float)(f->hkt[idx].re     * sgn);
            f->disp_x[idx] = (float)(f->dispf_x[idx].re * sgn);
            f->disp_z[idx] = (float)(f->dispf_z[idx].re * sgn);
            if (f->want_slopes) {
                f->slope_x[idx] = (float)(f->slopef_x[idx].re * sgn);
                f->slope_z[idx] = (float)(f->slopef_z[idx].re * sgn);
            }
        }
    }

    /* Foam last: it is a function of the packed displacement fields, so it has
     * to run after them, not inside the same loop. */
    if (f->want_foam) {
        for (int z = 0; z < N; ++z) {
            for (int x = 0; x < N; ++x) {
                /* Wrapped neighbours -- the patch tiles, so the derivative at
                 * the seam is a real derivative, not a clamped edge. */
                const int xm = (x - 1 + N) % N, xp = (x + 1) % N;
                const int zm = (z - 1 + N) % N, zp = (z + 1) % N;

                const float dDxdx = (f->disp_x[z * N + xp] -
                                     f->disp_x[z * N + xm]) * 0.5f;
                const float dDzdz = (f->disp_z[zp * N + x] -
                                     f->disp_z[zm * N + x]) * 0.5f;
                const float dDxdz = (f->disp_x[zp * N + x] -
                                     f->disp_x[zm * N + x]) * 0.5f;
                const float dDzdx = (f->disp_z[z * N + xp] -
                                     f->disp_z[z * N + xm]) * 0.5f;

                /* The derivatives above are per-CELL, which is what the
                 * Jacobian of the grid->world map wants: the displacement and
                 * the spacing are both in world units, so the cell size
                 * cancels and no patch_size factor belongs here. */
                f->foam[z * N + x] =
                    (1.0f + dDxdx) * (1.0f + dDzdz) - dDxdz * dDzdx;
            }
        }
    }

    f->evolved   = 1;
    f->last_time = t;
}

float JCE_CALL
jce_water_fft_sample_height(const JceWaterFft *f, float world_x, float world_z)
{
    if (!f || !f->evolved) return 0.0f;
    const int N = f->N;
    const float Lp = f->patch_size;

    /* Wrap world coords into [0, patch_size) then to a [0,N) grid coordinate. */
    float u = fmodf(world_x, Lp);
    float v = fmodf(world_z, Lp);
    if (u < 0.0f) u += Lp;
    if (v < 0.0f) v += Lp;
    float gx = u / Lp * (float)N;
    float gz = v / Lp * (float)N;

    int x0 = (int)floorf(gx);
    int z0 = (int)floorf(gz);
    float fx = gx - (float)x0;
    float fz = gz - (float)z0;

    int x0w = ((x0 % N) + N) % N;
    int z0w = ((z0 % N) + N) % N;
    int x1w = (x0w + 1) % N;
    int z1w = (z0w + 1) % N;

    float h00 = f->height[z0w * N + x0w];
    float h10 = f->height[z0w * N + x1w];
    float h01 = f->height[z1w * N + x0w];
    float h11 = f->height[z1w * N + x1w];

    float h0 = h00 + (h10 - h00) * fx;
    float h1 = h01 + (h11 - h01) * fx;
    return h0 + (h1 - h0) * fz;
}

int JCE_CALL jce_water_fft_resolution(const JceWaterFft *f)
{
    return f ? f->N : 0;
}

float JCE_CALL jce_water_fft_patch_size(const JceWaterFft *f)
{
    return f ? f->patch_size : 0.0f;
}

const float *JCE_CALL jce_water_fft_height_data(const JceWaterFft *f)
{
    return f ? f->height : NULL;
}

const float *JCE_CALL jce_water_fft_disp_x_data(const JceWaterFft *f)
{
    return f ? f->disp_x : NULL;
}

const float *JCE_CALL jce_water_fft_disp_z_data(const JceWaterFft *f)
{
    return f ? f->disp_z : NULL;
}

/* ── Inverse-displacement surface query ─────────────────────────────
 *
 * The FFT surface is PARAMETRIC, not a height map.  vs_water.sc draws the
 * vertex authored at world (x, z) at
 *
 *     (x + D_x(x,z),  base_y + h(x,z),  z + D_z(x,z))
 *
 * so "the surface height at world position p" is NOT h(p).  Sampling h(p)
 * directly is wrong by exactly the horizontal chop, and it is worst at
 * crests -- which is precisely where a floating body needs it to be right.
 *
 * Recovering the authored point requires solving  x + D(x) = p  for x.  D is
 * small relative to the wavelength, so a fixed-point iteration converges
 * quickly:
 *
 *     W <- p ;  repeat N times:  W <- p - D(W) ;  return h(W)
 *
 * Four iterations is the production norm.  Each iteration is one bilinear
 * fetch, so the whole query is tens of flops -- cheap enough that gameplay
 * can call it per buoyancy probe per tick. */

static float wfft_bilinear(const JceWaterFft *f, const float *grid,
                           float world_x, float world_z)
{
    const int   N  = f->N;
    const float Lp = f->patch_size;

    float u = fmodf(world_x, Lp);
    float v = fmodf(world_z, Lp);
    if (u < 0.0f) u += Lp;
    if (v < 0.0f) v += Lp;

    const float gx = u / Lp * (float)N;
    const float gz = v / Lp * (float)N;

    const int x0 = (int)floorf(gx);
    const int z0 = (int)floorf(gz);
    const float fx = gx - (float)x0;
    const float fz = gz - (float)z0;

    const int x0w = ((x0 % N) + N) % N;
    const int z0w = ((z0 % N) + N) % N;
    const int x1w = (x0w + 1) % N;
    const int z1w = (z0w + 1) % N;

    const float v00 = grid[z0w * N + x0w];
    const float v10 = grid[z0w * N + x1w];
    const float v01 = grid[z1w * N + x0w];
    const float v11 = grid[z1w * N + x1w];

    const float a = v00 + (v10 - v00) * fx;
    const float b = v01 + (v11 - v01) * fx;
    return a + (b - a) * fz;
}

void JCE_CALL jce_water_fft_sample_surface(const JceWaterFft *f,
                                           float world_x, float world_z,
                                           int iterations,
                                           JceWaterFftSample *out)
{
    if (!out) return;
    out->height = 0.0f;
    out->grid_x = world_x;
    out->grid_z = world_z;
    out->disp_x = 0.0f;
    out->disp_z = 0.0f;
    out->slope_x = 0.0f;
    out->slope_z = 0.0f;
    out->jacobian = 1.0f;      /* undisturbed, not "folded" */
    if (!f || !f->evolved) return;

    if (iterations < 1) iterations = 1;
    if (iterations > 16) iterations = 16;

    /* Solve x + D(x) = p.  Starting from p itself converges for the chop
     * magnitudes a stable Tessendorf surface produces. */
    float wx = world_x;
    float wz = world_z;
    for (int i = 0; i < iterations; i++) {
        const float dx = wfft_bilinear(f, f->disp_x, wx, wz);
        const float dz = wfft_bilinear(f, f->disp_z, wx, wz);
        wx = world_x - dx;
        wz = world_z - dz;
    }

    out->grid_x = wx;
    out->grid_z = wz;
    out->disp_x = wfft_bilinear(f, f->disp_x, wx, wz);
    out->disp_z = wfft_bilinear(f, f->disp_z, wx, wz);
    out->height = wfft_bilinear(f, f->height, wx, wz);

    /* The solve above already located the authored grid point; reading the
     * auxiliary fields there is four more bilinear fetches and no extra
     * iteration.  A caller that wanted a normal used to pay for FOUR more
     * complete inverse solves to central-difference one. */
    if (f->want_slopes) {
        out->slope_x = wfft_bilinear(f, f->slope_x, wx, wz);
        out->slope_z = wfft_bilinear(f, f->slope_z, wx, wz);
    }
    if (f->want_foam)
        out->jacobian = wfft_bilinear(f, f->foam, wx, wz);
}

float JCE_CALL jce_water_fft_sample_height_displaced(const JceWaterFft *f,
                                                     float world_x,
                                                     float world_z,
                                                     int iterations)
{
    JceWaterFftSample s;
    jce_water_fft_sample_surface(f, world_x, world_z, iterations, &s);
    return s.height;
}

/* ── Exact surface gradient (Tessendorf eq. 37) ────────────────────────── */

bool JCE_CALL jce_water_fft_enable_slopes(JceWaterFft *f)
{
    if (!f) return false;
    if (f->want_slopes) return true;

    const size_t cells = (size_t)f->N * (size_t)f->N;
    WfftCpx *sx = (WfftCpx *)JCE_MALLOC(cells * sizeof(WfftCpx));
    WfftCpx *sz = (WfftCpx *)JCE_MALLOC(cells * sizeof(WfftCpx));
    float   *ox = (float   *)JCE_MALLOC(cells * sizeof(float));
    float   *oz = (float   *)JCE_MALLOC(cells * sizeof(float));
    if (!sx || !sz || !ox || !oz) {
        /* All or nothing: a half-enabled field would evolve into buffers that
         * do not exist. */
        JCE_FREE(sx); JCE_FREE(sz); JCE_FREE(ox); JCE_FREE(oz);
        return false;
    }
    memset(ox, 0, cells * sizeof(float));
    memset(oz, 0, cells * sizeof(float));

    f->slopef_x = sx; f->slopef_z = sz;
    f->slope_x  = ox; f->slope_z  = oz;
    f->want_slopes = 1;

    /* Re-evolve at the current time so the slope fields are immediately valid
     * rather than zero until whenever the next evolve happens to arrive. */
    if (f->evolved) jce_water_fft_evolve(f, f->last_time);
    return true;
}

const float *JCE_CALL jce_water_fft_slope_x_data(const JceWaterFft *f)
{
    return (f && f->want_slopes) ? f->slope_x : NULL;
}
const float *JCE_CALL jce_water_fft_slope_z_data(const JceWaterFft *f)
{
    return (f && f->want_slopes) ? f->slope_z : NULL;
}

/* ── Jacobian foam ─────────────────────────────────────────────────────── */

bool JCE_CALL jce_water_fft_enable_foam(JceWaterFft *f)
{
    if (!f) return false;
    if (f->want_foam) return true;

    const size_t cells = (size_t)f->N * (size_t)f->N;
    float *j = (float *)JCE_MALLOC(cells * sizeof(float));
    if (!j) return false;
    /* 1 == unstretched.  Zeroing would read as "folded everywhere", i.e. an
     * ocean that is entirely foam until the first evolve. */
    for (size_t i = 0; i < cells; i++) j[i] = 1.0f;

    f->foam = j;
    f->want_foam = 1;
    if (f->evolved) jce_water_fft_evolve(f, f->last_time);
    return true;
}

const float *JCE_CALL jce_water_fft_foam_data(const JceWaterFft *f)
{
    return (f && f->want_foam) ? f->foam : NULL;
}
