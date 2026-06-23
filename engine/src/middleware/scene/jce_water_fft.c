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

#include "os/core/jce_memory.h"

#include <math.h>
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
    WfftCpx     *slope_x;      /* D_x(k) frequency field then spatial           */
    WfftCpx     *slope_z;      /* D_z(k)                                        */
    WfftCpx     *fft_row;      /* 1D scratch (length N) reused per row/column   */

    /* Spatial-domain output fields, row-major index = z*N + x (float).         */
    float       *height;
    float       *disp_x;
    float       *disp_z;

    int          evolved;       /* fields populated at least once               */
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
static void fft_radix2(WfftCpx *a, int n, int sign)
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
    for (int len = 2; len <= n; len <<= 1) {
        double ang = (double)sign * WFFT_TWO_PI / (double)len;
        double wr = cos(ang);
        double wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cwr = 1.0, cwi = 0.0;   /* running twiddle w^k */
            for (int k = 0; k < len / 2; ++k) {
                WfftCpx u = a[i + k];
                WfftCpx v;
                /* v = a[i+k+len/2] * w^k */
                v.re = a[i + k + len / 2].re * cwr - a[i + k + len / 2].im * cwi;
                v.im = a[i + k + len / 2].re * cwi + a[i + k + len / 2].im * cwr;
                a[i + k].re             = u.re + v.re;
                a[i + k].im             = u.im + v.im;
                a[i + k + len / 2].re   = u.re - v.re;
                a[i + k + len / 2].im   = u.im - v.im;
                /* advance running twiddle: cw *= w */
                double nwr = cwr * wr - cwi * wi;
                double nwi = cwr * wi + cwi * wr;
                cwr = nwr;
                cwi = nwi;
            }
        }
    }
}

/* Separable inverse 2D FFT of `field` (N*N, row-major [row*N + col]) in place.
 * Transforms each row then each column with sign=+1 and applies the 1/(N*N)
 * normalization so the result is a true inverse.  `scratch` is a length-N
 * complex buffer the caller owns (reused). */
static void ifft2d(WfftCpx *field, int N, WfftCpx *scratch)
{
    /* rows */
    for (int row = 0; row < N; ++row)
        fft_radix2(&field[row * N], N, +1);

    /* columns (gather into scratch, transform, scatter back) */
    for (int col = 0; col < N; ++col) {
        for (int row = 0; row < N; ++row)
            scratch[row] = field[row * N + col];
        fft_radix2(scratch, N, +1);
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
    f->slope_x = (WfftCpx *)JCE_MALLOC(cells * sizeof(WfftCpx));
    f->slope_z = (WfftCpx *)JCE_MALLOC(cells * sizeof(WfftCpx));
    f->fft_row = (WfftCpx *)JCE_MALLOC((size_t)N * sizeof(WfftCpx));
    f->height  = (float   *)JCE_MALLOC(cells * sizeof(float));
    f->disp_x  = (float   *)JCE_MALLOC(cells * sizeof(float));
    f->disp_z  = (float   *)JCE_MALLOC(cells * sizeof(float));

    if (!f->h0 || !f->h0_conj || !f->omega || !f->hkt || !f->slope_x ||
        !f->slope_z || !f->fft_row || !f->height || !f->disp_x || !f->disp_z) {
        jce_water_fft_destroy(f);
        return NULL;
    }

    /* Zero outputs so an accessor read before the first evolve is well-defined. */
    memset(f->height, 0, cells * sizeof(float));
    memset(f->disp_x, 0, cells * sizeof(float));
    memset(f->disp_z, 0, cells * sizeof(float));

    /* --- Precompute h0(k), conj(h0(-k)), and the dispersion omega(k). ---
     * The Gaussian draws walk the grid in a FIXED [m][n] order so the seed
     * fully determines the spectrum (deterministic across runs/platforms). */
    WfftRng rng;
    wfft_rng_seed(&rng, (uint64_t)seed, 0x9E3779B97F4A7C15ULL);

    const double Lp = (double)patch_size;

    /* First pass: draw h0(k) and omega(k) for every cell. */
    for (int m = 0; m < N; ++m) {
        for (int n = 0; n < N; ++n) {
            int idx = m * N + n;
            int km = wfft_mode(m, N);
            int kn = wfft_mode(n, N);
            double kx = WFFT_TWO_PI * (double)km / Lp;
            double kz = WFFT_TWO_PI * (double)kn / Lp;

            double ph = wfft_phillips(f, kx, kz);
            double root = sqrt(ph) * 0.70710678118654752440; /* 1/sqrt(2) */

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

    return f;
}

void JCE_CALL jce_water_fft_destroy(JceWaterFft *f)
{
    if (!f) return;
    JCE_FREE(f->h0);
    JCE_FREE(f->h0_conj);
    JCE_FREE(f->omega);
    JCE_FREE(f->hkt);
    JCE_FREE(f->slope_x);
    JCE_FREE(f->slope_z);
    JCE_FREE(f->fft_row);
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
                f->slope_x[idx].re = 0.0; f->slope_x[idx].im = 0.0;
                f->slope_z[idx].re = 0.0; f->slope_z[idx].im = 0.0;
            } else {
                double khx = kx / klen, khz = kz / klen;
                /* -i*h = (im, -re) */
                double mix = im, mre = -re;
                f->slope_x[idx].re = khx * mix; f->slope_x[idx].im = khx * mre;
                f->slope_z[idx].re = khz * mix; f->slope_z[idx].im = khz * mre;
            }
        }
    }

    /* --- Inverse 2D FFTs to the spatial domain. --- */
    ifft2d(f->hkt,     N, f->fft_row);
    ifft2d(f->slope_x, N, f->fft_row);
    ifft2d(f->slope_z, N, f->fft_row);

    /* --- Pack real parts into the float output fields with the (-1)^(x+z)
     * sign flip (fftshift) so the patch is centred and tiles seamlessly. --- */
    for (int z = 0; z < N; ++z) {
        for (int x = 0; x < N; ++x) {
            int idx = z * N + x;
            double sgn = ((x + z) & 1) ? -1.0 : 1.0;
            f->height[idx] = (float)(f->hkt[idx].re     * sgn);
            f->disp_x[idx] = (float)(f->slope_x[idx].re * sgn);
            f->disp_z[idx] = (float)(f->slope_z[idx].re * sgn);
        }
    }

    f->evolved = 1;
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
