/*
 * jce_cloud_noise.c  Volumetric cloud density field (CPU, deterministic).
 *
 * ---------------------------------------------------------------------------
 * PERIOD -- how seamless tiling is enforced
 * ---------------------------------------------------------------------------
 * The sky is unbounded, so the field must repeat without a visible seam. Two
 * mechanisms together guarantee that, exactly, not approximately:
 *
 *   (a) World space is mapped to TILE space up front:
 *           u = frac(x / period_x), v = frac(y / period_y), w = frac(z / period_z)
 *       so every sample lands in [0,1)^3. Folding here (rather than at the
 *       lattice) also keeps the lattice coordinates small enough that float32
 *       still has sub-cell precision arbitrarily far from the origin.
 *
 *   (b) Every octave's frequency is rounded to an INTEGER number of cells per
 *       period (see cloud_freq_cells). The lattice coordinate is then u*F with
 *       F integral, and every lattice index is taken modulo F before it is
 *       hashed. Index F therefore aliases index 0: the gradient vectors (Perlin)
 *       and the feature points (Worley) on the two sides of the seam are the
 *       same values, so the interpolation across the seam is continuous by
 *       construction. Rounding the frequency is the load-bearing step -- a
 *       fractional cells-per-period would put a half cell at the seam and no
 *       amount of modulo would close it.
 *
 * The only residual difference between x and x+period is float32 rounding in
 * (x + period)/period, on the order of 1e-5 in the final density.
 *
 * Y is wrapped the same way, but period_y is the full thickness of the cloud
 * layer and the layer is sampled over exactly one period, so the vertical
 * repeat is never visible; wrapping it only serves to bound the hash input.
 * ---------------------------------------------------------------------------
 */

#include "jce_cloud_noise.h"

#include <math.h>
#include <string.h>

/* Per-channel hash salts. Distinct constants keep the base shape, the billows,
 * the erosion detail and each weather channel decorrelated from one seed. */
#define CLOUD_SALT_PERLIN  0x1b873593u
#define CLOUD_SALT_WORLEY  0x85ebca6bu
#define CLOUD_SALT_DETAIL  0xc2b2ae35u
#define CLOUD_SALT_WCOV    0x27d4eb2fu
#define CLOUD_SALT_WTYPE   0x165667b1u
#define CLOUD_SALT_WPRECIP 0x9e3779b1u

/* Above this shape value a voxel counts as cloud CORE and detail erosion is
 * switched fully off. Eroding the core uniformly hollows the cloud out. */
#define CLOUD_CORE_ONSET 0.25f

/* ── scalar helpers ─────────────────────────────────────────────────────── */

static float cloud_clamp01(float v)
{
    /* Written as a positive test so NaN (which fails every comparison) falls
     * through to 0 rather than propagating. */
    if (v > 0.0f) {
        return (v < 1.0f) ? v : 1.0f;
    }
    return 0.0f;
}

static float cloud_lerp(float a, float b, float t) { return a + (b - a) * t; }

static float cloud_fade(float t)
{
    /* Quintic fade: first and second derivatives vanish at the cell boundary,
     * so octave seams do not show up as creases in the lit result. */
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float cloud_smoothstep(float e0, float e1, float x)
{
    const float d = e1 - e0;
    if (!(fabsf(d) > 1e-6f)) {
        return (x >= e1) ? 1.0f : 0.0f;
    }
    {
        const float t = cloud_clamp01((x - e0) / d);
        return t * t * (3.0f - 2.0f * t);
    }
}

float jce_cloud_remap(float v, float lo, float hi, float nlo, float nhi)
{
    const float d = hi - lo;
    if (!(fabsf(d) > 1e-6f)) {
        return nlo; /* degenerate window: collapse rather than divide by zero */
    }
    return nlo + (v - lo) / d * (nhi - nlo);
}

/* ── integer hash ───────────────────────────────────────────────────────── */

static uint32_t cloud_hash_u32(uint32_t v)
{
    /* xorshift-multiply finaliser (PCG/murmur family). Integer-only on purpose:
     * frac(sin(x)*k) depends on the transcendental precision of whoever
     * evaluates it and diverges between CPU bakes and GPU sampling. */
    v ^= v >> 17;
    v *= 0xed5ad4bbu;
    v ^= v >> 11;
    v *= 0xac4c1b51u;
    v ^= v >> 15;
    v *= 0x31848babu;
    v ^= v >> 14;
    return v;
}

static uint32_t cloud_hash3(uint32_t x, uint32_t y, uint32_t z, uint32_t seed)
{
    uint32_t h = cloud_hash_u32(seed ^ 0x9e3779b9u);
    h = cloud_hash_u32(h ^ (x * 0x85ebca6bu));
    h = cloud_hash_u32(h ^ (y * 0xc2b2ae35u));
    h = cloud_hash_u32(h ^ (z * 0x27d4eb2fu));
    return h;
}

/* Wrap a lattice index into [0,F). F is guaranteed >= 1 by cloud_freq_cells. */
static uint32_t cloud_wrapi(int32_t i, uint32_t period)
{
    const int32_t m = (int32_t)period;
    int32_t r = i % m;
    if (r < 0) {
        r += m;
    }
    return (uint32_t)r;
}

/* Cells per period, rounded to an integer -- see the PERIOD block above.
 * The upper clamp keeps u*F inside the range where float32 still resolves
 * individual cells. */
static uint32_t cloud_freq_cells(float f)
{
    float r;
    if (!isfinite(f)) {
        return 1u;
    }
    r = floorf(f + 0.5f);
    if (!(r >= 1.0f)) {
        return 1u;
    }
    if (r > 1024.0f) {
        r = 1024.0f;
    }
    return (uint32_t)r;
}

/* World -> tile space, folded into [0,1). */
static float cloud_tile_coord(float v, float period)
{
    float t;
    if (!isfinite(v)) {
        return 0.0f;
    }
    t = v / period; /* period is sanitised > 0 before this is ever called */
    t -= floorf(t);
    if (!(t >= 0.0f && t < 1.0f)) {
        /* frac() of a value just below zero rounds up to exactly 1.0; by
         * periodicity that is the same point as 0, so this is a fold, not a
         * discontinuity. Also catches non-finite quotients. */
        t = 0.0f;
    }
    return t;
}

/* ── Perlin gradient noise on a wrapped lattice ─────────────────────────── */

/* Perlin's 12 edge gradients, padded to 16 so the index is a mask, not a
 * modulo. The 4 repeats are the classic ones and keep the distribution even. */
static const float k_cloud_grad3[16][3] = {
    { 1.0f,  1.0f,  0.0f }, { -1.0f,  1.0f,  0.0f },
    { 1.0f, -1.0f,  0.0f }, { -1.0f, -1.0f,  0.0f },
    { 1.0f,  0.0f,  1.0f }, { -1.0f,  0.0f,  1.0f },
    { 1.0f,  0.0f, -1.0f }, { -1.0f,  0.0f, -1.0f },
    { 0.0f,  1.0f,  1.0f }, {  0.0f, -1.0f,  1.0f },
    { 0.0f,  1.0f, -1.0f }, {  0.0f, -1.0f, -1.0f },
    { 1.0f,  1.0f,  0.0f }, {  0.0f, -1.0f,  1.0f },
    { -1.0f, 1.0f,  0.0f }, {  0.0f, -1.0f, -1.0f }
};

static float cloud_perlin3(float gx, float gy, float gz, uint32_t F, uint32_t seed)
{
    const int32_t xi = (int32_t)floorf(gx);
    const int32_t yi = (int32_t)floorf(gy);
    const int32_t zi = (int32_t)floorf(gz);
    const float xf = gx - (float)xi;
    const float yf = gy - (float)yi;
    const float zf = gz - (float)zi;
    const float u = cloud_fade(xf);
    const float v = cloud_fade(yf);
    const float w = cloud_fade(zf);
    float n[8];
    int c;

    for (c = 0; c < 8; ++c) {
        const int32_t dx = c & 1;
        const int32_t dy = (c >> 1) & 1;
        const int32_t dz = (c >> 2) & 1;
        const uint32_t h = cloud_hash3(cloud_wrapi(xi + dx, F),
                                       cloud_wrapi(yi + dy, F),
                                       cloud_wrapi(zi + dz, F), seed);
        const float *g = k_cloud_grad3[h & 15u];
        n[c] = g[0] * (xf - (float)dx) + g[1] * (yf - (float)dy) +
               g[2] * (zf - (float)dz);
    }

    {
        const float x0 = cloud_lerp(n[0], n[1], u);
        const float x1 = cloud_lerp(n[2], n[3], u);
        const float x2 = cloud_lerp(n[4], n[5], u);
        const float x3 = cloud_lerp(n[6], n[7], u);
        const float y0 = cloud_lerp(x0, x1, v);
        const float y1 = cloud_lerp(x2, x3, v);
        /* sqrt(3)/2 is the theoretical bound of 3-D Perlin with these edge
         * gradients; scaling by its reciprocal makes full use of [-1,1]. */
        float r = cloud_lerp(y0, y1, w) * 1.1547005f;
        if (r > 1.0f)  r = 1.0f;
        if (r < -1.0f) r = -1.0f;
        return r;
    }
}

/* ── Worley (cellular) F1 on a wrapped lattice ──────────────────────────── */

static float cloud_worley3(float gx, float gy, float gz, uint32_t F, uint32_t seed)
{
    const int32_t xi = (int32_t)floorf(gx);
    const int32_t yi = (int32_t)floorf(gy);
    const int32_t zi = (int32_t)floorf(gz);
    float best2 = 4.0f; /* > any distance reachable in the 3x3x3 neighbourhood */
    int32_t dz;

    for (dz = -1; dz <= 1; ++dz) {
        int32_t dy;
        for (dy = -1; dy <= 1; ++dy) {
            int32_t dx;
            for (dx = -1; dx <= 1; ++dx) {
                const int32_t cx = xi + dx;
                const int32_t cy = yi + dy;
                const int32_t cz = zi + dz;
                /* Position uses the UNWRAPPED cell, the hash uses the wrapped
                 * one: that is the whole tiling trick for cellular noise. */
                const uint32_t h = cloud_hash3(cloud_wrapi(cx, F),
                                               cloud_wrapi(cy, F),
                                               cloud_wrapi(cz, F), seed);
                const float ox = (float)(h & 1023u) * (1.0f / 1024.0f);
                const float oy = (float)((h >> 10) & 1023u) * (1.0f / 1024.0f);
                const float oz = (float)((h >> 20) & 1023u) * (1.0f / 1024.0f);
                const float px = ((float)cx + ox) - gx;
                const float py = ((float)cy + oy) - gy;
                const float pz = ((float)cz + oz) - gz;
                const float d2 = px * px + py * py + pz * pz;
                if (d2 < best2) {
                    best2 = d2;
                }
            }
        }
    }

    if (!(best2 > 0.0f)) {
        return 0.0f; /* exact coincidence, or NaN -- keeps sqrtf well defined */
    }
    {
        const float d = sqrtf(best2);
        return (d < 1.0f) ? d : 1.0f;
    }
}

/* ── fBm ────────────────────────────────────────────────────────────────── */

static float cloud_perlin_fbm01(const JceCloudNoiseParams *sp,
                                float u, float v, float w,
                                float f0, int32_t octaves, uint32_t salt)
{
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, f = f0;
    int32_t o;

    for (o = 0; o < octaves; ++o) {
        const uint32_t F = cloud_freq_cells(f);
        const float ff = (float)F;
        sum += amp * cloud_perlin3(u * ff, v * ff, w * ff, F,
                                   sp->seed ^ salt ^ ((uint32_t)o * 0x68bc21ebu));
        norm += amp;
        amp *= sp->gain;
        f *= sp->lacunarity;
    }
    if (!(norm > 1e-6f)) {
        return 0.5f;
    }
    return cloud_clamp01(0.5f + 0.5f * (sum / norm));
}

static float cloud_worley_fbm01(const JceCloudNoiseParams *sp,
                                float u, float v, float w,
                                float f0, int32_t octaves, uint32_t salt)
{
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, f = f0;
    int32_t o;

    for (o = 0; o < octaves; ++o) {
        const uint32_t F = cloud_freq_cells(f);
        const float ff = (float)F;
        sum += amp * cloud_worley3(u * ff, v * ff, w * ff, F,
                                   sp->seed ^ salt ^ ((uint32_t)o * 0x9e3779b1u));
        norm += amp;
        amp *= sp->gain;
        f *= sp->lacunarity;
    }
    if (!(norm > 1e-6f)) {
        return 0.5f;
    }
    return cloud_clamp01(sum / norm);
}

/* Perlin remapped by the inverted Worley: keeps Perlin's connectivity, gains
 * Worley's billows. `billow - 1` lies in [-1,0], so the remap window is at
 * least 1 wide and can never collapse -- the guard in jce_cloud_remap is a
 * belt-and-braces, not the mechanism. */
static float cloud_perlin_worley_uvw(const JceCloudNoiseParams *sp,
                                     float u, float v, float w)
{
    const float perlin = cloud_perlin_fbm01(sp, u, v, w, sp->base_freq,
                                            sp->base_octaves, CLOUD_SALT_PERLIN);
    const float worley = cloud_worley_fbm01(sp, u, v, w, sp->base_freq,
                                            sp->base_octaves, CLOUD_SALT_WORLEY);
    const float billow = 1.0f - worley;
    return cloud_clamp01(jce_cloud_remap(perlin, billow - 1.0f, 1.0f, 0.0f, 1.0f));
}

/* ── parameter sanitising ───────────────────────────────────────────────── */

void jce_cloud_noise_params_default(JceCloudNoiseParams *p)
{
    if (!p) {
        return;
    }
    memset(p, 0, sizeof(*p));
    p->seed = 0x5eed1234u;

    /* A 4 km sky tile over a 1 km thick layer: large enough that the repeat is
     * beyond the horizon at normal camera altitudes. */
    p->period_x = 4096.0f;
    p->period_y = 1000.0f;
    p->period_z = 4096.0f;

    p->base_freq     = 4.0f; /* cells per period, not per world unit */
    p->base_octaves  = 4;
    p->lacunarity    = 2.0f;
    p->gain          = 0.5f;

    p->detail_freq     = 24.0f;
    p->detail_octaves  = 3;
    p->detail_strength = 0.45f;

    p->weather_freq          = 3.0f;
    p->weather_coverage_bias = 0.0f;

    p->density_scale = 1.0f;
    p->anvil_bias    = 0.5f;

    p->weather_fn   = NULL;
    p->weather_user = NULL;
}

static float cloud_sanf(float v, float lo, float hi, float fallback)
{
    if (!isfinite(v)) {
        return fallback;
    }
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float cloud_san_period(float v, float fallback)
{
    /* A tiny or negative period would turn into an absurdly high world-space
     * frequency, so fall back rather than clamp. */
    return (isfinite(v) && v > 1e-3f && v < 1e9f) ? v : fallback;
}

static int32_t cloud_sani(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void cloud_sanitize(const JceCloudNoiseParams *in, JceCloudNoiseParams *out)
{
    JceCloudNoiseParams d;
    jce_cloud_noise_params_default(&d);
    if (!in) {
        *out = d;
        return;
    }
    *out = *in;
    out->period_x = cloud_san_period(in->period_x, d.period_x);
    out->period_y = cloud_san_period(in->period_y, d.period_y);
    out->period_z = cloud_san_period(in->period_z, d.period_z);

    out->base_freq    = cloud_sanf(in->base_freq, 1.0f, 512.0f, d.base_freq);
    out->base_octaves = cloud_sani(in->base_octaves, 1, JCE_CLOUD_MAX_OCTAVES);
    out->lacunarity   = cloud_sanf(in->lacunarity, 1.0f, 8.0f, d.lacunarity);
    out->gain         = cloud_sanf(in->gain, 0.05f, 0.95f, d.gain);

    out->detail_freq     = cloud_sanf(in->detail_freq, 1.0f, 512.0f, d.detail_freq);
    out->detail_octaves  = cloud_sani(in->detail_octaves, 1, JCE_CLOUD_MAX_OCTAVES);
    /* Capped below 1 so the erosion remap window (1 - strength) stays open. */
    out->detail_strength = cloud_sanf(in->detail_strength, 0.0f, 0.95f, 0.0f);

    out->weather_freq          = cloud_sanf(in->weather_freq, 1.0f, 64.0f, d.weather_freq);
    out->weather_coverage_bias = cloud_sanf(in->weather_coverage_bias, -1.0f, 1.0f, 0.0f);

    out->density_scale = cloud_sanf(in->density_scale, 0.0f, 8.0f, d.density_scale);
    out->anvil_bias    = cloud_sanf(in->anvil_bias, 0.0f, 1.0f, d.anvil_bias);
}

/* ── height gradient ────────────────────────────────────────────────────── */

/* Soft-edged altitude band: ramps up over [a0,a1], down over [b0,b1]. */
static float cloud_band(float h, float a0, float a1, float b0, float b1)
{
    return cloud_smoothstep(a0, a1, h) * (1.0f - cloud_smoothstep(b0, b1, h));
}

float jce_cloud_height_gradient(float height01, float cloud_type, float anvil_bias)
{
    const float h = cloud_clamp01(height01);
    const float t = cloud_clamp01(cloud_type);
    const float av = cloud_clamp01(anvil_bias);

    /* Bands are expressed as fractions of the cloud layer, per the classic
     * stratus / cumulus / cumulonimbus split. */
    const float stratus = cloud_band(h, 0.00f, 0.05f, 0.13f, 0.20f);
    const float cumulus = cloud_band(h, 0.10f, 0.24f, 0.44f, 0.60f);
    float cumulonimbus  = cloud_band(h, 0.05f, 0.14f, 0.82f, 1.00f);

    /* The anvil spreads the top out instead of adding to it: scaling by
     * (1 - cb) fills the top-of-band falloff without ever exceeding 1. */
    {
        const float anvil = cloud_band(h, 0.70f, 0.78f, 0.97f, 1.00f);
        cumulonimbus = cumulonimbus + av * anvil * (1.0f - cumulonimbus);
    }

    /* Piecewise-linear blend across the type axis; continuous at t = 0.5,
     * so an animated weather map interpolates instead of popping. */
    if (t < 0.5f) {
        return cloud_clamp01(cloud_lerp(stratus, cumulus, t * 2.0f));
    }
    return cloud_clamp01(cloud_lerp(cumulus, cumulonimbus, (t - 0.5f) * 2.0f));
}

/* ── weather map ────────────────────────────────────────────────────────── */

static void cloud_weather_default(const JceCloudNoiseParams *sp,
                                  float x, float z, JceCloudWeather *out)
{
    /* A constant Y slice of the 3-D lattice is a perfectly good 2-D field and
     * reuses one code path; the channels are decorrelated by salt, not by
     * offset, so they stay independent under any period. */
    const float u = cloud_tile_coord(x, sp->period_x);
    const float w = cloud_tile_coord(z, sp->period_z);
    const float cov = cloud_perlin_fbm01(sp, u, 0.0f, w, sp->weather_freq, 3,
                                         CLOUD_SALT_WCOV);
    const float typ = cloud_perlin_fbm01(sp, u, 0.0f, w, sp->weather_freq * 0.5f, 2,
                                         CLOUD_SALT_WTYPE);
    const float pcp = cloud_perlin_fbm01(sp, u, 0.0f, w, sp->weather_freq * 0.5f, 2,
                                         CLOUD_SALT_WPRECIP);

    out->coverage   = cloud_clamp01(cov + sp->weather_coverage_bias);
    out->cloud_type = cloud_clamp01(typ);
    /* Rain is rarer than cloud: bias the precipitation channel so only the
     * upper part of its range produces any. */
    out->precipitation = cloud_clamp01(pcp * 1.6f - 0.6f);
}

static void cloud_weather_sample_s(const JceCloudNoiseParams *sp,
                                   float x, float z, JceCloudWeather *out)
{
    out->coverage = 0.0f;
    out->cloud_type = 0.0f;
    out->precipitation = 0.0f;

    if (!isfinite(x) || !isfinite(z)) {
        return;
    }
    if (sp->weather_fn) {
        JceCloudWeather w;
        w.coverage = 0.0f;
        w.cloud_type = 0.0f;
        w.precipitation = 0.0f;
        sp->weather_fn(sp->weather_user, x, z, &w);
        /* Caller-supplied maps are untrusted: a NaN texel or an unnormalised
         * channel must not reach the density math. */
        out->coverage      = cloud_clamp01(w.coverage);
        out->cloud_type    = cloud_clamp01(w.cloud_type);
        out->precipitation = cloud_clamp01(w.precipitation);
        return;
    }
    cloud_weather_default(sp, x, z, out);
}

void jce_cloud_weather_sample(const JceCloudNoiseParams *p, float x, float z,
                              JceCloudWeather *out)
{
    JceCloudNoiseParams sp;
    if (!out) {
        return;
    }
    cloud_sanitize(p, &sp);
    cloud_weather_sample_s(&sp, x, z, out);
}

/* ── density ────────────────────────────────────────────────────────────── */

/* `precipitation` is the weather map's rain channel, 0 where it is not
 * raining.  Zero is the identity: a field with no rain bakes bit-for-bit as it
 * did before this parameter existed. */
static float cloud_density_s(const JceCloudNoiseParams *sp,
                             float x, float y, float z,
                             float height01, float coverage, float cloud_type,
                             float precipitation)
{
    float u, v, w, pw, grad, shape;

    if (!isfinite(x) || !isfinite(y) || !isfinite(z) || !isfinite(height01)) {
        return 0.0f;
    }
    coverage = cloud_clamp01(coverage);
    if (coverage <= 0.0f) {
        /* Explicit early out so "no clouds" is exactly zero, not epsilon. */
        return 0.0f;
    }
    height01 = cloud_clamp01(height01);
    precipitation = cloud_clamp01(precipitation);

    /* A raining cell is a DEEPER cell, not merely a wetter one.
     *
     * The type axis runs stratus -> cumulus -> cumulonimbus, and the height
     * gradient it selects is what decides how much vertical extent a column
     * gets. Rain does not fall out of a flat sheet; pushing the type toward
     * cumulonimbus where the weather map says it is raining is the difference
     * between a uniformly grey overcast and a tower with a base.
     *
     * Applied to the TYPE rather than to the density alone because density is
     * clamped to [0,1] and a multiplier there has nowhere to go once a cell is
     * already dense -- which is precisely the cells that rain. */
    cloud_type = cloud_clamp01(cloud_type + precipitation * 0.5f);

    u = cloud_tile_coord(x, sp->period_x);
    v = cloud_tile_coord(y, sp->period_y);
    w = cloud_tile_coord(z, sp->period_z);

    pw = cloud_perlin_worley_uvw(sp, u, v, w);
    grad = jce_cloud_height_gradient(height01, cloud_type, sp->anvil_bias);
    shape = pw * grad;

    /* Coverage carve: only the top `coverage` slice of the shape survives, and
     * the survivors are scaled by coverage so thin weather stays thin instead
     * of turning the few remaining voxels opaque. */
    shape = cloud_clamp01(jce_cloud_remap(shape, 1.0f - coverage, 1.0f, 0.0f, 1.0f));
    shape *= coverage;
    if (shape <= 0.0f) {
        return 0.0f;
    }

    if (sp->detail_strength > 0.0f) {
        /* Erosion is subtractive and edge-weighted: `edge` reaches 0 once the
         * shape passes CLOUD_CORE_ONSET, which makes the remap the identity and
         * leaves the core bit-for-bit untouched. Uniform erosion would eat the
         * interior and dissolve the cloud. */
        const float detail = cloud_worley_fbm01(sp, u, v, w, sp->detail_freq,
                                                sp->detail_octaves, CLOUD_SALT_DETAIL);
        const float wisp = 1.0f - detail;
        const float edge = 1.0f - cloud_clamp01(shape * (1.0f / CLOUD_CORE_ONSET));
        const float erosion = sp->detail_strength * edge * wisp;
        shape = cloud_clamp01(jce_cloud_remap(shape, erosion, 1.0f, 0.0f, 1.0f));
    }

    {
        /* And it absorbs more: the reference darkens rain cells by raising
         * light absorption where they exist. This atlas carries one channel,
         * and the march derives BOTH extinction and self-shadowing from it, so
         * raising density here is how "more absorption" is spelled. The clamp
         * is what bounds it: a cell already at 0.5 saturates, a thin one does
         * not, and the contrast between a raining cell and its neighbours is
         * the thing being bought. */
        const float dens = shape * sp->density_scale
                         * (1.0f + precipitation * 2.0f);
        if (!isfinite(dens)) {
            return 0.0f;
        }
        return cloud_clamp01(dens);
    }
}

void jce_cloud_noise_params_sanitize(const JceCloudNoiseParams *in,
                                     JceCloudNoiseParams *out)
{
    if (!out) return;
    cloud_sanitize(in, out);
}

float jce_cloud_density_prepared(const JceCloudNoiseParams *sanitized,
                                 float x, float y, float z,
                                 float height01, float coverage,
                                 float cloud_type)
{
    if (!sanitized) return 0.0f;
    return cloud_density_s(sanitized, x, y, z, height01, coverage, cloud_type,
                           0.0f);
}

float jce_cloud_density_weather_prepared(const JceCloudNoiseParams *sanitized,
                                         float x, float y, float z,
                                         float height01)
{
    JceCloudWeather wx;
    if (!sanitized) return 0.0f;
    cloud_weather_sample_s(sanitized, x, z, &wx);
    return cloud_density_s(sanitized, x, y, z, height01,
                           wx.coverage, wx.cloud_type, wx.precipitation);
}

float jce_cloud_density(const JceCloudNoiseParams *p,
                        float x, float y, float z,
                        float height01, float coverage, float cloud_type)
{
    JceCloudNoiseParams sp;
    cloud_sanitize(p, &sp);
    return cloud_density_s(&sp, x, y, z, height01, coverage, cloud_type, 0.0f);
}

float jce_cloud_density_weather(const JceCloudNoiseParams *p,
                                float x, float y, float z, float height01)
{
    JceCloudNoiseParams sp;
    JceCloudWeather wx;
    cloud_sanitize(p, &sp);
    cloud_weather_sample_s(&sp, x, z, &wx);
    return cloud_density_s(&sp, x, y, z, height01, wx.coverage, wx.cloud_type,
                           wx.precipitation);
}

/* ── public component accessors ─────────────────────────────────────────── */

float jce_cloud_perlin_fbm(const JceCloudNoiseParams *p, float x, float y, float z)
{
    JceCloudNoiseParams sp;
    cloud_sanitize(p, &sp);
    return cloud_perlin_fbm01(&sp,
                              cloud_tile_coord(x, sp.period_x),
                              cloud_tile_coord(y, sp.period_y),
                              cloud_tile_coord(z, sp.period_z),
                              sp.base_freq, sp.base_octaves, CLOUD_SALT_PERLIN);
}

float jce_cloud_worley_fbm(const JceCloudNoiseParams *p, float x, float y, float z)
{
    JceCloudNoiseParams sp;
    cloud_sanitize(p, &sp);
    return cloud_worley_fbm01(&sp,
                              cloud_tile_coord(x, sp.period_x),
                              cloud_tile_coord(y, sp.period_y),
                              cloud_tile_coord(z, sp.period_z),
                              sp.base_freq, sp.base_octaves, CLOUD_SALT_WORLEY);
}

float jce_cloud_perlin_worley(const JceCloudNoiseParams *p, float x, float y, float z)
{
    JceCloudNoiseParams sp;
    cloud_sanitize(p, &sp);
    return cloud_perlin_worley_uvw(&sp,
                                   cloud_tile_coord(x, sp.period_x),
                                   cloud_tile_coord(y, sp.period_y),
                                   cloud_tile_coord(z, sp.period_z));
}

/* ── bake ───────────────────────────────────────────────────────────────── */

/* Writes one Z slice into `dst`, addressed as dst[j * row_stride + i]. Shared
 * by the volume and the atlas bake so both stay byte-identical. */
static void cloud_bake_slice(const JceCloudNoiseParams *sp, uint32_t k,
                             uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
                             float *dst, size_t row_stride)
{
    const float zz = (((float)k + 0.5f) / (float)dim_z) * sp->period_z;
    uint32_t i;

    for (i = 0; i < dim_x; ++i) {
        const float xx = (((float)i + 0.5f) / (float)dim_x) * sp->period_x;
        JceCloudWeather wx;
        uint32_t j;
        /* The weather field is XZ-only, so it is hoisted out of the column;
         * the resulting strided writes are cheaper than dim_y extra fBm. */
        cloud_weather_sample_s(sp, xx, zz, &wx);
        for (j = 0; j < dim_y; ++j) {
            const float h01 = ((float)j + 0.5f) / (float)dim_y;
            dst[(size_t)j * row_stride + (size_t)i] =
                cloud_density_s(sp, xx, h01 * sp->period_y, zz, h01,
                                wx.coverage, wx.cloud_type, wx.precipitation);
        }
    }
}

bool jce_cloud_noise_bake(const JceCloudNoiseParams *p, float *out,
                          uint32_t dim_x, uint32_t dim_y, uint32_t dim_z)
{
    JceCloudNoiseParams sp;
    uint64_t voxels;
    uint32_t k;

    if (!out || dim_x == 0u || dim_y == 0u || dim_z == 0u) {
        return false;
    }
    voxels = (uint64_t)dim_x * (uint64_t)dim_y * (uint64_t)dim_z;
    if (voxels > (uint64_t)JCE_CLOUD_BAKE_MAX_VOXELS) {
        return false;
    }

    cloud_sanitize(p, &sp);
    for (k = 0; k < dim_z; ++k) {
        float *slice = out + (size_t)k * (size_t)dim_y * (size_t)dim_x;
        cloud_bake_slice(&sp, k, dim_x, dim_y, dim_z, slice, (size_t)dim_x);
    }
    return true;
}

bool jce_cloud_noise_atlas_size(uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
                                uint32_t tiles_x, uint32_t *out_w, uint32_t *out_h)
{
    uint64_t tiles_y, w, h;

    if (!out_w || !out_h) {
        return false;
    }
    *out_w = 0u;
    *out_h = 0u;
    if (dim_x == 0u || dim_y == 0u || dim_z == 0u || tiles_x == 0u) {
        return false;
    }
    tiles_y = ((uint64_t)dim_z + (uint64_t)tiles_x - 1u) / (uint64_t)tiles_x;
    w = (uint64_t)dim_x * (uint64_t)tiles_x;
    h = (uint64_t)dim_y * tiles_y;
    /* 65536 is beyond every shipping max-texture-dimension; past it the atlas
     * could not be uploaded anyway. */
    if (w > 65536u || h > 65536u) {
        return false;
    }
    if (w * h > (uint64_t)JCE_CLOUD_BAKE_MAX_VOXELS) {
        return false;
    }
    *out_w = (uint32_t)w;
    *out_h = (uint32_t)h;
    return true;
}

bool jce_cloud_noise_bake_atlas(const JceCloudNoiseParams *p, float *out,
                                uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
                                uint32_t tiles_x)
{
    JceCloudNoiseParams sp;
    uint32_t w = 0u, h = 0u, k;

    if (!out) {
        return false;
    }
    if (!jce_cloud_noise_atlas_size(dim_x, dim_y, dim_z, tiles_x, &w, &h)) {
        return false;
    }
    /* Clear first so the padding tiles (when dim_z is not a multiple of
     * tiles_x) are defined rather than whatever the caller's buffer held. */
    memset(out, 0, (size_t)w * (size_t)h * sizeof(float));

    cloud_sanitize(p, &sp);
    for (k = 0; k < dim_z; ++k) {
        const uint32_t tx = k % tiles_x;
        const uint32_t ty = k / tiles_x;
        float *slice = out + (size_t)ty * (size_t)dim_y * (size_t)w +
                       (size_t)tx * (size_t)dim_x;
        cloud_bake_slice(&sp, k, dim_x, dim_y, dim_z, slice, (size_t)w);
    }
    return true;
}

/* ── Atlas addressing ──────────────────────────────────────────────────
 * See jce_cloud_noise.h. */

void jce_cloud_atlas_uv(uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
                        uint32_t tiles_x,
                        float x01, float y01, float z01,
                        float *out_u, float *out_v)
{
    if (out_u) *out_u = 0.0f;
    if (out_v) *out_v = 0.0f;
    if (dim_x == 0u || dim_y == 0u || dim_z == 0u || tiles_x == 0u) return;

    uint32_t aw = 0u, ah = 0u;
    if (!jce_cloud_noise_atlas_size(dim_x, dim_y, dim_z, tiles_x, &aw, &ah))
        return;

    if (x01 < 0.0f) x01 = 0.0f; else if (x01 > 1.0f) x01 = 1.0f;
    if (y01 < 0.0f) y01 = 0.0f; else if (y01 > 1.0f) y01 = 1.0f;
    if (z01 < 0.0f) z01 = 0.0f; else if (z01 > 1.0f) z01 = 1.0f;

    /* Slice CENTRES: slice k of N is at (k + 0.5)/N, so the nearest slice to
     * z01 is floor(z01 * N) clamped to the last one. */
    uint32_t slice = (uint32_t)(z01 * (float)dim_z);
    if (slice >= dim_z) slice = dim_z - 1u;

    const uint32_t tile_col = slice % tiles_x;
    const uint32_t tile_row = slice / tiles_x;

    /* Texel centres WITHIN the slice, then offset into its tile. */
    const float px = (float)(tile_col * dim_x) + x01 * (float)(dim_x - 1u) + 0.5f;
    const float py = (float)(tile_row * dim_y) + y01 * (float)(dim_y - 1u) + 0.5f;

    if (out_u) *out_u = px / (float)aw;
    if (out_v) *out_v = py / (float)ah;
}

float jce_cloud_atlas_sample(const float *atlas, uint32_t atlas_w,
                             uint32_t atlas_h, float u, float v)
{
    if (!atlas || atlas_w == 0u || atlas_h == 0u) return 0.0f;

    float fx = u * (float)atlas_w - 0.5f;
    float fy = v * (float)atlas_h - 0.5f;
    if (fx < 0.0f) fx = 0.0f;
    if (fy < 0.0f) fy = 0.0f;
    if (fx > (float)(atlas_w - 1u)) fx = (float)(atlas_w - 1u);
    if (fy > (float)(atlas_h - 1u)) fy = (float)(atlas_h - 1u);

    const uint32_t x0 = (uint32_t)fx, y0 = (uint32_t)fy;
    const uint32_t x1 = (x0 + 1u < atlas_w) ? x0 + 1u : x0;
    const uint32_t y1 = (y0 + 1u < atlas_h) ? y0 + 1u : y0;
    const float tx = fx - (float)x0, ty = fy - (float)y0;

    const float a = atlas[(size_t)y0 * atlas_w + x0];
    const float b = atlas[(size_t)y0 * atlas_w + x1];
    const float c = atlas[(size_t)y1 * atlas_w + x0];
    const float d = atlas[(size_t)y1 * atlas_w + x1];
    const float top = a + (b - a) * tx;
    const float bot = c + (d - c) * tx;
    return top + (bot - top) * ty;
}

/* ── March cost ────────────────────────────────────────────────────────
 * See jce_cloud_noise.h. */

uint32_t jce_cloud_march_steps(int tier)
{
    /* LOW is the charter's minimum profile: integrated GPUs and WebGL2, where
     * the sky can easily be the whole frame budget.  8 steps over a 2.5 km
     * slab is ~300 m per sample -- coarse, but the layer is low-frequency and
     * the alternative on that hardware is no clouds at all. */
    switch (tier) {
    case 3:  /* ULTRA  */
    case 2:  return JCE_CLOUD_MARCH_MAX_STEPS;        /* HIGH   */
    case 1:  return 14u;                              /* MEDIUM */
    default: return 8u;                               /* LOW / unknown */
    }
}
