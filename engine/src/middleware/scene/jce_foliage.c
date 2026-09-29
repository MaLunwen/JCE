/*
 * jce_foliage.c -- Deterministic vegetation scatter implementation.
 *
 * See jce_foliage.h.  Pure math; depends only on jce_terrain (height sampling)
 * and the C math library.  No allocation, no globals, no RNG state outside the
 * locally-seeded xorshift32, so a given (params, terrain, origin) is fully
 * reproducible across runs and platforms.
 */

#include <jce/middleware/scene/jce_foliage.h>
#include <jce/middleware/scene/jce_terrain.h>

#include <string.h>
#include <math.h>

#define FOLIAGE_DEG2RAD 0.01745329251994329577f
#define FOLIAGE_TWO_PI  6.28318530717958647692f

/* Deterministic per-scatter RNG (same pattern used by particles / weapons). */
static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Uniform [0,1) with a clean 24-bit mantissa. */
static float randf01(uint32_t *s)
{
    return (float)(xs32(s) & 0x00FFFFFFu) / (float)0x01000000u;
}

/* Map a scatter candidate to a cell in a 2D field laid over either the scatter
 * rect or a shared world square.
 *
 * Both the density mask and the ridge field need this, and they used to do it
 * with two separate copies of the arithmetic -- which had already drifted: the
 * mask flipped V and the ridge did not, so the same world point addressed two
 * Z-MIRRORED cells.  An artist painting both over one square would have found
 * them disagreeing with no way to tell why.
 *
 * `flip_v` is the difference, and it is a real one rather than a bug to
 * remove.  An artist-painted mask arrives with an IMAGE convention (row 0 at
 * +Z, matching how the reference density map is read), while the ridge channel
 * is produced by the erosion pass row-major in +Z.  Naming it as a parameter
 * keeps one code path and makes the divergence a stated decision. */
static size_t foliage_field_cell(int dim, float world_size,
                                 float ox, float oz, float rx, float rz,
                                 float inv_ax, float inv_az, bool flip_v)
{
    float u, v;
    if (world_size > 0.0f) {
        const float inv_ws = 1.0f / world_size;
        u = (ox + rx) * inv_ws + 0.5f;
        v = (oz + rz) * inv_ws + 0.5f;
    } else {
        u = rx * inv_ax + 0.5f;
        v = rz * inv_az + 0.5f;
    }
    if (flip_v) v = 1.0f - v;

    int mx = (int)(u * (float)dim);
    int mz = (int)(v * (float)dim);
    if (mx < 0) mx = 0; else if (mx >= dim) mx = dim - 1;
    if (mz < 0) mz = 0; else if (mz >= dim) mz = dim - 1;
    return (size_t)mz * (size_t)dim + (size_t)mx;
}

static bool foliage_sample_terrain(void *user, float world_x, float world_z,
                                   bool want_normal, float *out_world_y,
                                   float out_world_normal[3])
{
    const JceTerrain *terrain = (const JceTerrain *)user;
    if (!terrain || !out_world_y) return false;

    *out_world_y = jce_terrain_sample_height(terrain, world_x, world_z);
    if (!want_normal) return true;

    const float e = 0.5f;
    const float hxp = jce_terrain_sample_height(terrain, world_x + e, world_z);
    const float hxm = jce_terrain_sample_height(terrain, world_x - e, world_z);
    const float hzp = jce_terrain_sample_height(terrain, world_x, world_z + e);
    const float hzm = jce_terrain_sample_height(terrain, world_x, world_z - e);
    const float dx = (hxp - hxm) / (2.0f * e);
    const float dz = (hzp - hzm) / (2.0f * e);
    const float inv_len = 1.0f / sqrtf(dx * dx + dz * dz + 1.0f);
    out_world_normal[0] = -dx * inv_len;
    out_world_normal[1] = inv_len;
    out_world_normal[2] = -dz * inv_len;
    return true;
}

static uint32_t foliage_scatter_impl(const JceFoliageScatterParams *p,
                                     JceFoliageSurfaceSampleFn sample_surface,
                                     void *surface_user,
                                     const jce_vec3 *origin,
                                     JceFoliageInstance *out,
                                     uint32_t out_cap)
{
    if (!p || !out || out_cap == 0) return 0;

    const float ax = p->area_x;
    const float az = p->area_z;
    const float density = p->density;
    if (ax <= 0.0f || az <= 0.0f || density <= 0.0f) return 0;

    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    if (origin) { ox = origin->x; oy = origin->y; oz = origin->z; }

    uint32_t target = (uint32_t)(density * ax * az);
    if (target > out_cap)                  target = out_cap;
    if (target > JCE_FOLIAGE_MAX_INSTANCES) target = JCE_FOLIAGE_MAX_INSTANCES;
    if (target == 0) return 0;

    uint32_t state = p->seed ? p->seed : 0x9E3779B9u;

    float smin = (p->scale_min > 0.0f) ? p->scale_min : 1.0f;
    float smax = (p->scale_max >= smin) ? p->scale_max : smin;

    /* Density mask (large-world #8a): when present, each candidate is kept with
     * probability = its mask cell, so brush-painted regions modulate density. */
    const bool have_mask = (p->density_mask != NULL) && (p->mask_dim > 0);
    const float inv_ax = 1.0f / ax, inv_az = 1.0f / az;

    /* A placement is kept when the terrain normal's Y >= cos(max_slope); a
     * limit of >=90° (or no terrain) disables the test. */
    /* Surface rules are pure threshold rejections -- see the header.  They are
     * resolved once here rather than per candidate so the hot loop only pays
     * for the ones actually enabled. */
    const bool height_band = (p->height_min < p->height_max);
    const bool ridge_band  = (p->ridge_field != NULL) && (p->ridge_dim > 0) &&
                             (p->ridge_min < p->ridge_max);
    const bool slope_limit =
        (sample_surface != NULL) && (p->max_slope_deg > 0.0f) &&
        (p->max_slope_deg < 90.0f);
    const float cos_max = slope_limit ? cosf(p->max_slope_deg * FOLIAGE_DEG2RAD) : -1.0f;

    uint32_t n = 0;
    for (uint32_t i = 0; i < target; ++i) {
        /* Always draw the same 4 randoms per candidate so the sequence — and
         * thus the kept set — is identical regardless of slope rejections. */
        const float rx  = (randf01(&state) - 0.5f) * ax;
        const float rz  = (randf01(&state) - 0.5f) * az;
        const float yaw = randf01(&state) * FOLIAGE_TWO_PI;
        const float sc  = smin + (smax - smin) * randf01(&state);

        /* Density-mask rejection (5th RNG draw, only in mask mode so the
         * maskless sequence is unchanged): keep with probability = mask cell. */
        if (have_mask) {
            /* 5th RNG draw, taken ONLY in mask mode so the maskless sequence
             * is unchanged. */
            const float mr = randf01(&state);
            /* flip_v: artist-painted masks arrive in image order.  See
             * foliage_field_cell. */
            const size_t c = foliage_field_cell(p->mask_dim, p->mask_world_size,
                                                ox, oz, rx, rz,
                                                inv_ax, inv_az, true);
            if (mr >= p->density_mask[c])
                continue;                          /* painted-sparse — reject */
        }

        const float wx = ox + rx;
        const float wz = oz + rz;
        float wy = oy;

        float nrm[3] = { 0.0f, 1.0f, 0.0f };
        /* Ridge rule first: it needs no terrain sample at all, so rejecting
         * here avoids the height fetch entirely for a candidate that was never
         * going to survive. */
        if (ridge_band) {
            /* Same mapping as the density mask, WITHOUT the flip: the ridge
             * channel is engine-produced row-major in +Z, not an image. */
            const size_t c = foliage_field_cell(p->ridge_dim, p->mask_world_size,
                                                ox, oz, rx, rz,
                                                inv_ax, inv_az, false);
            const float rv = p->ridge_field[c];
            if (rv < p->ridge_min || rv > p->ridge_max) continue;
        }

        if (sample_surface) {
            const bool need_normal = slope_limit || p->want_normals;
            if (!sample_surface(surface_user, wx, wz, need_normal, &wy, nrm))
                continue;
            if (height_band && (wy < p->height_min || wy > p->height_max))
                continue;                       /* outside the altitude band */
            if (need_normal) {
                const float len_sq = nrm[0] * nrm[0] + nrm[1] * nrm[1] +
                                     nrm[2] * nrm[2];
                if (!(len_sq > 1e-12f) || !isfinite(len_sq)) continue;
                const float inv_len = 1.0f / sqrtf(len_sq);
                nrm[0] *= inv_len;
                nrm[1] *= inv_len;
                nrm[2] *= inv_len;
                if (slope_limit && nrm[1] < cos_max) continue;
                if (!p->want_normals) {
                    nrm[0] = 0.0f;
                    nrm[1] = 1.0f;
                    nrm[2] = 0.0f;
                }
            }
        }

        out[n].normal[0] = nrm[0];
        out[n].normal[1] = nrm[1];
        out[n].normal[2] = nrm[2];
        out[n].pos[0] = wx;
        out[n].pos[1] = wy;
        out[n].pos[2] = wz;
        out[n].rot_y  = yaw;
        out[n].scale  = sc;
        ++n;
    }
    return n;
}

uint32_t jce_foliage_scatter(const JceFoliageScatterParams *p,
                             const JceTerrain *terrain,
                             const jce_vec3 *origin,
                             JceFoliageInstance *out,
                             uint32_t out_cap)
{
    return foliage_scatter_impl(p, terrain ? foliage_sample_terrain : NULL,
                                (void *)terrain, origin, out, out_cap);
}

uint32_t jce_foliage_scatter_on_surface(
    const JceFoliageScatterParams *p,
    JceFoliageSurfaceSampleFn sample_surface,
    void *surface_user,
    const jce_vec3 *origin,
    JceFoliageInstance *out,
    uint32_t out_cap)
{
    return foliage_scatter_impl(p, sample_surface, surface_user, origin, out,
                                out_cap);
}

/* ── Cooked placement ──────────────────────────────────────────────────
 * See jce_foliage.h.  A flat little container: header, then the instance
 * array verbatim, then a hash over both. */

#define FOLIAGE_COOK_MAGIC   0x474C4F46u   /* 'FOLG' little-endian */
#define FOLIAGE_COOK_VERSION 1u
#define FOLIAGE_COOK_HDR     32u

static void fcook_wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t fcook_rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* FNV-1a over everything EXCEPT the hash slot itself.  Deliberately not a
 * cryptographic hash: what a cooked asset actually suffers is truncation and
 * bit rot, both of which this catches.
 *
 * Skipping the slot is not a detail.  A first version hashed [0, size-4),
 * which still covered the slot at offset 28 -- zero while the hash was being
 * computed, non-zero by the time anything read it back, so verification could
 * never succeed and the container silently loaded NOTHING. */
#define FOLIAGE_COOK_HASH_OFF 28u

static uint32_t fcook_hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        if (i >= FOLIAGE_COOK_HASH_OFF && i < FOLIAGE_COOK_HASH_OFF + 4u)
            continue;
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

size_t jce_foliage_cook_size(uint32_t count)
{
    return (size_t)FOLIAGE_COOK_HDR
         + (size_t)count * sizeof(JceFoliageInstance);
}

bool jce_foliage_cook(const JceFoliageInstance *instances, uint32_t count,
                      uint32_t seed, void *dst, size_t dst_size,
                      size_t *out_written)
{
    if (!dst) return false;
    if (count > 0u && !instances) return false;
    const size_t need = jce_foliage_cook_size(count);
    if (dst_size < need) return false;

    uint8_t *p = (uint8_t *)dst;
    memset(p, 0, need);
    fcook_wr_u32(p + 0, FOLIAGE_COOK_MAGIC);
    fcook_wr_u32(p + 4, FOLIAGE_COOK_VERSION);
    fcook_wr_u32(p + 8, count);
    /* The seed is recorded so a consumer can tell WHICH scatter this was baked
     * from.  A cooked list that silently belongs to different parameters is
     * the one failure this container exists to make detectable. */
    fcook_wr_u32(p + 12, seed);
    fcook_wr_u32(p + 16, (uint32_t)sizeof(JceFoliageInstance));

    if (count > 0u)
        memcpy(p + FOLIAGE_COOK_HDR, instances,
               (size_t)count * sizeof(JceFoliageInstance));

    /* Hash covers the header (minus its own slot) and the whole payload. */
    fcook_wr_u32(p + FOLIAGE_COOK_HASH_OFF, fcook_hash(p, need));
    if (out_written) *out_written = need;
    return true;
}

uint32_t jce_foliage_cooked_count(const void *data, size_t size)
{
    if (!data || size < FOLIAGE_COOK_HDR) return 0u;
    const uint8_t *p = (const uint8_t *)data;
    if (fcook_rd_u32(p + 0) != FOLIAGE_COOK_MAGIC) return 0u;
    if (fcook_rd_u32(p + 4) != FOLIAGE_COOK_VERSION) return 0u;
    return fcook_rd_u32(p + 8);
}

uint32_t jce_foliage_cooked_seed(const void *data, size_t size)
{
    if (!data || size < FOLIAGE_COOK_HDR) return 0u;
    const uint8_t *p = (const uint8_t *)data;
    if (fcook_rd_u32(p + 0) != FOLIAGE_COOK_MAGIC) return 0u;
    return fcook_rd_u32(p + 12);
}

bool jce_foliage_cooked_validate(const void *data, size_t size)
{
    if (!data || size < FOLIAGE_COOK_HDR) return false;
    const uint8_t *p = (const uint8_t *)data;
    if (fcook_rd_u32(p + 0) != FOLIAGE_COOK_MAGIC) return false;
    if (fcook_rd_u32(p + 4) != FOLIAGE_COOK_VERSION) return false;
    if (fcook_rd_u32(p + 16) != (uint32_t)sizeof(JceFoliageInstance))
        return false;

    const uint32_t count = fcook_rd_u32(p + 8);
    if (count > JCE_FOLIAGE_MAX_INSTANCES) return false;
    const size_t need = jce_foliage_cook_size(count);
    if (size < need) return false;
    return fcook_hash(p, need) == fcook_rd_u32(p + FOLIAGE_COOK_HASH_OFF);
}

uint32_t jce_foliage_load_cooked(const void *data, size_t size,
                                 JceFoliageInstance *out, uint32_t out_cap)
{
    if (!out || !jce_foliage_cooked_validate(data, size)) return 0u;
    const uint8_t *p = (const uint8_t *)data;
    const uint32_t count = fcook_rd_u32(p + 8);
    const uint32_t n = (count < out_cap) ? count : out_cap;
    if (n > 0u)
        memcpy(out, p + FOLIAGE_COOK_HDR,
               (size_t)n * sizeof(JceFoliageInstance));
    return n;
}
