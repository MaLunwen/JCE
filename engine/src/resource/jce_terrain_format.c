/*
 * jce_terrain_format.c -- cooked terrain v3 binary codec.
 *
 * See jce_terrain_format.h for the contract.  Every field goes through the
 * explicit little-endian helpers below; nothing here casts file bytes to a
 * struct, so the layout is identical on every architecture regardless of
 * packing or alignment rules.
 */

#include "jce_terrain_format.h"

#include <math.h>
#include <string.h>

#include <xxhash.h>

/* ── Little-endian primitives ──────────────────────────────────────── */

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd_u64(const uint8_t *p)
{
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}
static float rd_f32(const uint8_t *p)
{
    /* Via memcpy, not a pointer cast: the cast would be an aliasing violation
     * and can trap on strict-alignment targets. */
    uint32_t bits = rd_u32(p);
    float v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}
static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static void wr_u64(uint8_t *p, uint64_t v)
{
    wr_u32(p, (uint32_t)(v & 0xFFFFFFFFu));
    wr_u32(p + 4, (uint32_t)(v >> 32));
}
static void wr_f32(uint8_t *p, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    wr_u32(p, bits);
}

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) if (p[i]) return false;
    return true;
}

const char *jce_terrain_status_str(JceTerrainStatus s)
{
    switch (s) {
    case JCE_TERRAIN_OK:              return "ok";
    case JCE_TERRAIN_ERR_ARGS:        return "invalid arguments";
    case JCE_TERRAIN_ERR_TRUNCATED:   return "truncated";
    case JCE_TERRAIN_ERR_MAGIC:       return "bad magic";
    case JCE_TERRAIN_ERR_VERSION:     return "unsupported version";
    case JCE_TERRAIN_ERR_HEADER_HASH: return "header hash mismatch";
    case JCE_TERRAIN_ERR_DIR_HASH:    return "directory hash mismatch";
    case JCE_TERRAIN_ERR_TILE_HASH:   return "tile hash mismatch";
    case JCE_TERRAIN_ERR_RANGE:       return "range out of bounds";
    case JCE_TERRAIN_ERR_RESERVED:    return "reserved bytes not zero";
    case JCE_TERRAIN_ERR_OVERFLOW:    return "size overflow";
    case JCE_TERRAIN_ERR_UNSUPPORTED: return "unsupported feature";
    }
    return "unknown";
}

/* ── Header ────────────────────────────────────────────────────────── */

/* The header hash covers bytes 0..111.  Bytes 112..127 hold the hash itself
 * plus reserved space, so they are outside their own domain -- otherwise the
 * value would have to hash itself. */
#define HDR_HASH_OFFSET 112u
#define HDR_HASH_DOMAIN 112u

JceTerrainStatus jce_terrain_read_header(const void *data, size_t size,
                                         JceTerrainHeader *out)
{
    if (!data || !out) return JCE_TERRAIN_ERR_ARGS;
    if (size < JCE_TERRAIN_HEADER_BYTES) return JCE_TERRAIN_ERR_TRUNCATED;

    const uint8_t *p = (const uint8_t *)data;

    if (rd_u32(p + 0) != JCE_TERRAIN_MAGIC) return JCE_TERRAIN_ERR_MAGIC;
    if (rd_u16(p + 4) != JCE_TERRAIN_VERSION) return JCE_TERRAIN_ERR_VERSION;
    if (rd_u16(p + 6) != JCE_TERRAIN_HEADER_BYTES) return JCE_TERRAIN_ERR_VERSION;
    if (rd_u32(p + 8) != 0u) return JCE_TERRAIN_ERR_RESERVED; /* file flags */

    const uint64_t stored_hash = rd_u64(p + HDR_HASH_OFFSET);
    const uint64_t actual_hash = (uint64_t)XXH3_64bits(p, HDR_HASH_DOMAIN);
    if (stored_hash != actual_hash) return JCE_TERRAIN_ERR_HEADER_HASH;

    if (!all_zero(p + 120, 8)) return JCE_TERRAIN_ERR_RESERVED;

    JceTerrainHeader h;
    memset(&h, 0, sizeof h);
    h.channel_mask         = rd_u32(p + 12);
    h.width_samples        = rd_u32(p + 16);
    h.height_samples       = rd_u32(p + 20);
    h.tile_count_x         = rd_u32(p + 24);
    h.tile_count_z         = rd_u32(p + 28);
    h.tile_count           = rd_u32(p + 32);
    h.storage_tile_cells   = rd_u16(p + 36);
    h.render_chunk_cells   = rd_u16(p + 38);
    h.collision_tile_cells = rd_u16(p + 40);
    h.height_format        = p[42];
    h.weight_format        = p[43];
    h.hole_format          = p[44];
    h.normal_format        = p[45];
    h.diagonal_rule        = p[46];
    h.weight_texture_count = p[47];
    h.world_size_x         = rd_f32(p + 48);
    h.world_size_z         = rd_f32(p + 52);
    h.base_height          = rd_f32(p + 56);
    h.height_range         = rd_f32(p + 60);
    h.directory_offset     = rd_u64(p + 64);
    h.directory_bytes      = rd_u64(p + 72);
    h.payload_offset       = rd_u64(p + 80);
    h.payload_bytes        = rd_u64(p + 88);

    /* Structural sanity.  Anything that fails here means the file cannot be
     * trusted, so no tile is ever published from it. */
    if (h.width_samples < 2u || h.height_samples < 2u)
        return JCE_TERRAIN_ERR_RANGE;
    if (!(h.height_range > 0.0f) || !isfinite(h.height_range))
        return JCE_TERRAIN_ERR_RANGE;
    if (!isfinite(h.base_height) || !isfinite(h.world_size_x) ||
        !isfinite(h.world_size_z))
        return JCE_TERRAIN_ERR_RANGE;
    if (h.height_format != JCE_TERRAIN_HEIGHT_R16_UNORM &&
        h.height_format != JCE_TERRAIN_HEIGHT_F32_LE)
        return JCE_TERRAIN_ERR_UNSUPPORTED;
    if (h.diagonal_rule > JCE_TERRAIN_DIAG_ZIGZAG)
        return JCE_TERRAIN_ERR_UNSUPPORTED;
    if ((h.channel_mask & JCE_TERRAIN_CH_HEIGHT) == 0u)
        return JCE_TERRAIN_ERR_UNSUPPORTED;   /* height is mandatory */

    if (h.directory_offset != JCE_TERRAIN_HEADER_BYTES)
        return JCE_TERRAIN_ERR_RANGE;

    /* Checked product: tile_count must equal count_x * count_z without
     * wrapping, and the directory must be exactly that many entries. */
    if (h.tile_count_x != 0u &&
        h.tile_count_z > (uint32_t)0xFFFFFFFFu / h.tile_count_x)
        return JCE_TERRAIN_ERR_OVERFLOW;
    if (h.tile_count != h.tile_count_x * h.tile_count_z)
        return JCE_TERRAIN_ERR_RANGE;
    if (h.directory_bytes !=
        (uint64_t)h.tile_count * JCE_TERRAIN_DIRENT_BYTES)
        return JCE_TERRAIN_ERR_RANGE;

    /* Every declared region must lie inside the actual buffer. */
    if (h.directory_offset > size ||
        h.directory_bytes > size - h.directory_offset)
        return JCE_TERRAIN_ERR_RANGE;
    if (h.payload_offset > size || h.payload_bytes > size - h.payload_offset)
        return JCE_TERRAIN_ERR_RANGE;
    /* Trailing data is not canonical: a cook that appends is a cook that
     * differs, and byte-identical double-cook is a release gate. */
    if (h.payload_offset + h.payload_bytes != (uint64_t)size)
        return JCE_TERRAIN_ERR_RANGE;

    *out = h;
    return JCE_TERRAIN_OK;
}

/* ── Directory ─────────────────────────────────────────────────────── */

JceTerrainStatus jce_terrain_read_directory(const void *data, size_t size,
                                            const JceTerrainHeader *hdr,
                                            JceTerrainDirEntry *out_entries)
{
    if (!data || !hdr || !out_entries) return JCE_TERRAIN_ERR_ARGS;
    if (hdr->directory_offset + hdr->directory_bytes > (uint64_t)size)
        return JCE_TERRAIN_ERR_RANGE;

    const uint8_t *base = (const uint8_t *)data;
    const uint8_t *dir  = base + hdr->directory_offset;

    const uint64_t stored = rd_u64(base + 96);
    const uint64_t actual =
        (uint64_t)XXH3_64bits(dir, (size_t)hdr->directory_bytes);
    if (stored != actual) return JCE_TERRAIN_ERR_DIR_HASH;

    for (uint32_t i = 0; i < hdr->tile_count; i++) {
        const uint8_t *e = dir + (size_t)i * JCE_TERRAIN_DIRENT_BYTES;
        JceTerrainDirEntry d;
        memset(&d, 0, sizeof d);
        d.tile_x        = rd_u32(e + 0);
        d.tile_z        = rd_u32(e + 4);
        d.stored_offset = rd_u64(e + 8);
        d.stored_bytes  = rd_u32(e + 16);
        d.raw_bytes     = rd_u32(e + 20);
        d.raw_hash      = rd_u64(e + 24);
        d.min_height    = rd_f32(e + 32);
        d.max_height    = rd_f32(e + 36);
        d.sample_width  = rd_u16(e + 40);
        d.sample_height = rd_u16(e + 42);
        d.cell_width    = rd_u16(e + 44);
        d.cell_height   = rd_u16(e + 46);
        d.channel_mask  = rd_u32(e + 48);
        d.compression   = e[52];
        d.height_format = e[53];
        if (rd_u16(e + 54) != 0u) return JCE_TERRAIN_ERR_RESERVED;
        if (!all_zero(e + 56, 8)) return JCE_TERRAIN_ERR_RESERVED;

        if (d.tile_x >= hdr->tile_count_x || d.tile_z >= hdr->tile_count_z)
            return JCE_TERRAIN_ERR_RANGE;
        if (d.sample_width < 2u || d.sample_height < 2u)
            return JCE_TERRAIN_ERR_RANGE;
        if (d.height_format != hdr->height_format)
            return JCE_TERRAIN_ERR_RANGE;   /* must match the file header */
        if (d.compression > 1u) return JCE_TERRAIN_ERR_UNSUPPORTED;
        if (d.stored_offset > (uint64_t)size ||
            d.stored_bytes > (uint64_t)size - d.stored_offset)
            return JCE_TERRAIN_ERR_RANGE;
        if ((d.stored_offset & 15u) != 0u)
            return JCE_TERRAIN_ERR_RANGE;   /* 16-byte aligned payloads */

        out_entries[i] = d;
    }
    return JCE_TERRAIN_OK;
}

/* ── Tile payload ──────────────────────────────────────────────────── */

JceTerrainStatus jce_terrain_read_tile_heights(const void *data, size_t size,
                                               const JceTerrainHeader *hdr,
                                               const JceTerrainDirEntry *entry,
                                               float *out_heights)
{
    if (!data || !hdr || !entry || !out_heights) return JCE_TERRAIN_ERR_ARGS;
    if (entry->compression != 0u) return JCE_TERRAIN_ERR_UNSUPPORTED;

    const uint8_t *base = (const uint8_t *)data;
    if (entry->stored_offset + entry->stored_bytes > (uint64_t)size)
        return JCE_TERRAIN_ERR_RANGE;

    const uint8_t *tile = base + entry->stored_offset;
    if (entry->stored_bytes != entry->raw_bytes)
        return JCE_TERRAIN_ERR_RANGE;   /* raw: stored == raw by definition */
    if (entry->raw_bytes < JCE_TERRAIN_TILEHDR_BYTES)
        return JCE_TERRAIN_ERR_TRUNCATED;

    /* Hash covers the exact uncompressed payload INCLUDING its zero padding,
     * so a corrupt pad byte is caught rather than ignored. */
    const uint64_t actual = (uint64_t)XXH3_64bits(tile, entry->raw_bytes);
    if (actual != entry->raw_hash) return JCE_TERRAIN_ERR_TILE_HASH;

    if (rd_u32(tile + 0) != JCE_TERRAIN_TILE_MAGIC)
        return JCE_TERRAIN_ERR_MAGIC;
    if (rd_u16(tile + 4) != 1u) return JCE_TERRAIN_ERR_VERSION;
    if (rd_u16(tile + 6) != JCE_TERRAIN_TILEHDR_BYTES)
        return JCE_TERRAIN_ERR_VERSION;
    if (rd_u32(tile + 8) != entry->channel_mask)
        return JCE_TERRAIN_ERR_RANGE;
    if (rd_u32(tile + 12) != 0u) return JCE_TERRAIN_ERR_RESERVED;

    const uint32_t h_off = rd_u32(tile + 16);
    const uint32_t h_len = rd_u32(tile + 20);

    const uint64_t sample_count =
        (uint64_t)entry->sample_width * (uint64_t)entry->sample_height;
    if (sample_count > 0x40000000ull) return JCE_TERRAIN_ERR_OVERFLOW;

    const uint32_t bytes_per =
        (hdr->height_format == JCE_TERRAIN_HEIGHT_R16_UNORM) ? 2u : 4u;
    if (h_len != (uint32_t)sample_count * bytes_per)
        return JCE_TERRAIN_ERR_RANGE;
    if (h_off < JCE_TERRAIN_TILEHDR_BYTES ||
        h_off > entry->raw_bytes || h_len > entry->raw_bytes - h_off)
        return JCE_TERRAIN_ERR_RANGE;

    const uint8_t *hp = tile + h_off;
    const float lo = hdr->base_height;
    const float range = hdr->height_range;

    if (hdr->height_format == JCE_TERRAIN_HEIGHT_R16_UNORM) {
        const float inv = range / 65535.0f;
        for (uint64_t i = 0; i < sample_count; i++)
            out_heights[i] = lo + (float)rd_u16(hp + i * 2u) * inv;
    } else {
        const float hi = lo + range;
        for (uint64_t i = 0; i < sample_count; i++) {
            const float v = rd_f32(hp + i * 4u);
            /* Reject rather than clamp: a value outside the declared interval
             * means the writer and the metadata disagree, and silently
             * clamping would hide that from every downstream consumer. */
            if (!isfinite(v) || v < lo - 1e-3f || v > hi + 1e-3f)
                return JCE_TERRAIN_ERR_RANGE;
            out_heights[i] = v;
        }
    }
    return JCE_TERRAIN_OK;
}

/* ── Writing ───────────────────────────────────────────────────────── */

static size_t align16(size_t v) { return (v + 15u) & ~(size_t)15u; }

size_t jce_terrain_write_size_height_ridge(uint32_t samples_x,
                                           uint32_t samples_z)
{
    const size_t count   = (size_t)samples_x * (size_t)samples_z;
    const size_t payload = align16(JCE_TERRAIN_TILEHDR_BYTES + count * 2u
                                                             + count * 2u);
    return align16(JCE_TERRAIN_HEADER_BYTES + JCE_TERRAIN_DIRENT_BYTES)
         + payload;
}

size_t jce_terrain_write_size_height_only(uint32_t samples_x,
                                          uint32_t samples_z)
{
    const size_t count = (size_t)samples_x * (size_t)samples_z;
    const size_t hbytes = count * 2u;                    /* R16 */
    const size_t payload = align16(JCE_TERRAIN_TILEHDR_BYTES + hbytes);
    const size_t dir_end = JCE_TERRAIN_HEADER_BYTES + JCE_TERRAIN_DIRENT_BYTES;
    return align16(dir_end) + payload;
}

JceTerrainStatus jce_terrain_write_height_ridge(
    void *dst, size_t dst_size,
    const float *heights, const float *ridge,
    uint32_t samples_x, uint32_t samples_z,
    float world_size_x, float world_size_z,
    float base_height, float height_range,
    uint8_t height_format, uint8_t diagonal_rule,
    size_t *out_written)
{
    if (!dst || !heights) return JCE_TERRAIN_ERR_ARGS;
    if (samples_x < 2u || samples_z < 2u) return JCE_TERRAIN_ERR_ARGS;
    if (!(height_range > 0.0f)) return JCE_TERRAIN_ERR_ARGS;
    if (height_format != JCE_TERRAIN_HEIGHT_R16_UNORM)
        return JCE_TERRAIN_ERR_UNSUPPORTED;
    if (diagonal_rule > JCE_TERRAIN_DIAG_ZIGZAG)
        return JCE_TERRAIN_ERR_UNSUPPORTED;

    const size_t need = ridge
        ? jce_terrain_write_size_height_ridge(samples_x, samples_z)
        : jce_terrain_write_size_height_only(samples_x, samples_z);
    if (dst_size < need) return JCE_TERRAIN_ERR_TRUNCATED;

    const size_t count  = (size_t)samples_x * (size_t)samples_z;
    const size_t hbytes = count * 2u;
    const size_t rbytes = ridge ? count * 2u : 0u;
    const size_t raw_bytes =
        align16(JCE_TERRAIN_TILEHDR_BYTES + hbytes + rbytes);
    const uint32_t mask = JCE_TERRAIN_CH_HEIGHT
                        | (ridge ? JCE_TERRAIN_CH_RIDGE : 0u);
    const size_t dir_end   = JCE_TERRAIN_HEADER_BYTES + JCE_TERRAIN_DIRENT_BYTES;
    const size_t pay_off   = align16(dir_end);

    uint8_t *p = (uint8_t *)dst;
    memset(p, 0, need);

    /* Quantise, tracking the true min/max so the directory records what the
     * DECODED data actually is rather than what was declared. */
    const float lo = base_height;
    const float hi = base_height + height_range;
    float minh = hi, maxh = lo;
    uint8_t *hp = p + pay_off + JCE_TERRAIN_TILEHDR_BYTES;
    for (size_t i = 0; i < count; i++) {
        float v = heights[i];
        if (!isfinite(v)) return JCE_TERRAIN_ERR_RANGE;
        if (v < lo || v > hi) return JCE_TERRAIN_ERR_RANGE;
        if (v < minh) minh = v;
        if (v > maxh) maxh = v;
        float n = (v - lo) / height_range * 65535.0f;
        if (n < 0.0f) n = 0.0f;
        if (n > 65535.0f) n = 65535.0f;
        wr_u16(hp + i * 2u, (uint16_t)(n + 0.5f));
    }

    /* Ridge: [-1,1] -> 16-bit unorm.  Out-of-range is REJECTED rather than
     * clamped -- a ridge outside [-1,1] means the producer and this format
     * disagree about what the channel is, and clamping would hide that. */
    if (ridge) {
        uint8_t *rp = hp + hbytes;
        for (size_t i = 0; i < count; i++) {
            const float r = ridge[i];
            if (!isfinite(r) || r < -1.0f || r > 1.0f)
                return JCE_TERRAIN_ERR_RANGE;
            float n = (r + 1.0f) * 0.5f * 65535.0f;
            if (n < 0.0f) n = 0.0f;
            if (n > 65535.0f) n = 65535.0f;
            wr_u16(rp + i * 2u, (uint16_t)(n + 0.5f));
        }
    }

    /* Tile payload header. */
    uint8_t *th = p + pay_off;
    wr_u32(th + 0, JCE_TERRAIN_TILE_MAGIC);
    wr_u16(th + 4, 1u);
    wr_u16(th + 6, (uint16_t)JCE_TERRAIN_TILEHDR_BYTES);
    wr_u32(th + 8, mask);
    wr_u32(th + 12, 0u);
    wr_u32(th + 16, JCE_TERRAIN_TILEHDR_BYTES);
    wr_u32(th + 20, (uint32_t)hbytes);
    if (ridge) {
        /* Channel index 4 in the fixed bit order -> section pair at 16 + 4*8. */
        wr_u32(th + 48, (uint32_t)(JCE_TERRAIN_TILEHDR_BYTES + hbytes));
        wr_u32(th + 52, (uint32_t)rbytes);
    }
    /* Absent channels are (0,0) -- already zeroed by the memset. */

    const uint64_t tile_hash = (uint64_t)XXH3_64bits(th, raw_bytes);

    /* Directory entry. */
    uint8_t *e = p + JCE_TERRAIN_HEADER_BYTES;
    wr_u32(e + 0, 0u);                       /* tile_x */
    wr_u32(e + 4, 0u);                       /* tile_z */
    wr_u64(e + 8, (uint64_t)pay_off);
    wr_u32(e + 16, (uint32_t)raw_bytes);     /* stored == raw (uncompressed) */
    wr_u32(e + 20, (uint32_t)raw_bytes);
    wr_u64(e + 24, tile_hash);
    wr_f32(e + 32, minh);
    wr_f32(e + 36, maxh);
    wr_u16(e + 40, (uint16_t)samples_x);
    wr_u16(e + 42, (uint16_t)samples_z);
    wr_u16(e + 44, (uint16_t)(samples_x - 1u));
    wr_u16(e + 46, (uint16_t)(samples_z - 1u));
    wr_u32(e + 48, mask);
    e[52] = 0u;                              /* compression: raw */
    e[53] = height_format;

    const uint64_t dir_hash =
        (uint64_t)XXH3_64bits(p + JCE_TERRAIN_HEADER_BYTES,
                              JCE_TERRAIN_DIRENT_BYTES);

    /* File header. */
    wr_u32(p + 0, JCE_TERRAIN_MAGIC);
    wr_u16(p + 4, (uint16_t)JCE_TERRAIN_VERSION);
    wr_u16(p + 6, (uint16_t)JCE_TERRAIN_HEADER_BYTES);
    wr_u32(p + 8, 0u);
    wr_u32(p + 12, mask);
    wr_u32(p + 16, samples_x);
    wr_u32(p + 20, samples_z);
    wr_u32(p + 24, 1u);
    wr_u32(p + 28, 1u);
    wr_u32(p + 32, 1u);
    wr_u16(p + 36, (uint16_t)(samples_x - 1u));
    wr_u16(p + 38, (uint16_t)(samples_x - 1u));
    wr_u16(p + 40, (uint16_t)(samples_x - 1u));
    p[42] = height_format;
    p[43] = 0u;
    p[44] = 0u;
    p[45] = 0u;
    p[46] = diagonal_rule;
    p[47] = 1u;                              /* weight_texture_count */
    wr_f32(p + 48, world_size_x);
    wr_f32(p + 52, world_size_z);
    wr_f32(p + 56, base_height);
    wr_f32(p + 60, height_range);
    wr_u64(p + 64, JCE_TERRAIN_HEADER_BYTES);
    wr_u64(p + 72, JCE_TERRAIN_DIRENT_BYTES);
    wr_u64(p + 80, (uint64_t)pay_off);
    wr_u64(p + 88, (uint64_t)(need - pay_off));
    wr_u64(p + 96, dir_hash);
    wr_u64(p + 104, 0u);                     /* file hash: not used here */
    wr_u64(p + HDR_HASH_OFFSET,
           (uint64_t)XXH3_64bits(p, HDR_HASH_DOMAIN));

    if (out_written) *out_written = need;
    return JCE_TERRAIN_OK;
}

/* Height-only writing is the ridge writer with no ridge plane.  Kept as its
 * own entry point because it is the narrow case every existing caller uses,
 * and routing it through one implementation means the two can never drift. */
JceTerrainStatus jce_terrain_write_height_only(
    void *dst, size_t dst_size,
    const float *heights, uint32_t samples_x, uint32_t samples_z,
    float world_size_x, float world_size_z,
    float base_height, float height_range,
    uint8_t height_format, uint8_t diagonal_rule,
    size_t *out_written)
{
    return jce_terrain_write_height_ridge(
        dst, dst_size, heights, NULL, samples_x, samples_z,
        world_size_x, world_size_z, base_height, height_range,
        height_format, diagonal_rule, out_written);
}

JceTerrainStatus jce_terrain_read_tile_ridge(const void *data, size_t size,
                                             const JceTerrainHeader *hdr,
                                             const JceTerrainDirEntry *entry,
                                             float *out_ridge)
{
    if (!data || !hdr || !entry || !out_ridge) return JCE_TERRAIN_ERR_ARGS;

    /* "No ridge channel" is not corruption.  Reporting it distinctly lets a
     * caller fall back to "no mask" instead of failing the whole tile. */
    if ((entry->channel_mask & JCE_TERRAIN_CH_RIDGE) == 0u)
        return JCE_TERRAIN_ERR_UNSUPPORTED;

    const uint8_t *base = (const uint8_t *)data;
    if (entry->stored_offset + entry->stored_bytes > (uint64_t)size)
        return JCE_TERRAIN_ERR_RANGE;
    const uint8_t *tile = base + entry->stored_offset;
    if (entry->stored_bytes != entry->raw_bytes) return JCE_TERRAIN_ERR_RANGE;
    if (entry->raw_bytes < JCE_TERRAIN_TILEHDR_BYTES)
        return JCE_TERRAIN_ERR_TRUNCATED;

    /* Verify before decoding, exactly as the height path does: a tile is
     * published whole or not at all. */
    const uint64_t actual = (uint64_t)XXH3_64bits(tile, entry->raw_bytes);
    if (actual != entry->raw_hash) return JCE_TERRAIN_ERR_TILE_HASH;
    if (rd_u32(tile + 0) != JCE_TERRAIN_TILE_MAGIC) return JCE_TERRAIN_ERR_MAGIC;
    if (rd_u16(tile + 4) != 1u) return JCE_TERRAIN_ERR_VERSION;

    const uint32_t r_off = rd_u32(tile + 48);
    const uint32_t r_len = rd_u32(tile + 52);

    const uint64_t count =
        (uint64_t)entry->sample_width * (uint64_t)entry->sample_height;
    if (count > 0x40000000ull) return JCE_TERRAIN_ERR_OVERFLOW;
    if (r_len != (uint32_t)count * 2u) return JCE_TERRAIN_ERR_RANGE;
    if (r_off < JCE_TERRAIN_TILEHDR_BYTES ||
        r_off > entry->raw_bytes || r_len > entry->raw_bytes - r_off)
        return JCE_TERRAIN_ERR_RANGE;

    const uint8_t *rp = tile + r_off;
    for (uint64_t i = 0; i < count; i++)
        out_ridge[i] = (float)rd_u16(rp + i * 2u) * (2.0f / 65535.0f) - 1.0f;
    return JCE_TERRAIN_OK;
}

/* ── Multi-tile cooking ────────────────────────────────────────────────
 * See jce_terrain_format.h.  One tile payload per (tile_x, tile_z), each with
 * a one-sample overlap onto its right and bottom neighbours. */

static size_t tiled_tile_raw_bytes(uint32_t span, bool with_ridge)
{
    const size_t count = (size_t)span * (size_t)span;
    return align16(JCE_TERRAIN_TILEHDR_BYTES + count * 2u
                   + (with_ridge ? count * 2u : 0u));
}

size_t jce_terrain_write_size_tiled(uint32_t samples_x, uint32_t samples_z,
                                    uint32_t tile_cells, bool with_ridge)
{
    if (tile_cells == 0u || samples_x < 2u || samples_z < 2u) return 0u;
    const uint32_t cells_x = samples_x - 1u;
    const uint32_t cells_z = samples_z - 1u;
    if (cells_x % tile_cells || cells_z % tile_cells) return 0u;

    const uint32_t tx   = cells_x / tile_cells;
    const uint32_t tz   = cells_z / tile_cells;
    const uint32_t span = tile_cells + 1u;
    const size_t   dir  = (size_t)tx * tz * JCE_TERRAIN_DIRENT_BYTES;
    return align16(JCE_TERRAIN_HEADER_BYTES + dir)
         + (size_t)tx * tz * tiled_tile_raw_bytes(span, with_ridge);
}

JceTerrainStatus jce_terrain_write_tiled(
    void *dst, size_t dst_size,
    const float *heights, const float *ridge,
    uint32_t samples_x, uint32_t samples_z, uint32_t tile_cells,
    float world_size_x, float world_size_z,
    float base_height, float height_range,
    uint8_t height_format, uint8_t diagonal_rule,
    size_t *out_written)
{
    if (!dst || !heights) return JCE_TERRAIN_ERR_ARGS;
    if (samples_x < 2u || samples_z < 2u || tile_cells == 0u)
        return JCE_TERRAIN_ERR_ARGS;
    if (!(height_range > 0.0f)) return JCE_TERRAIN_ERR_ARGS;
    if (height_format != JCE_TERRAIN_HEIGHT_R16_UNORM)
        return JCE_TERRAIN_ERR_UNSUPPORTED;
    if (diagonal_rule > JCE_TERRAIN_DIAG_ZIGZAG)
        return JCE_TERRAIN_ERR_UNSUPPORTED;

    const uint32_t cells_x = samples_x - 1u;
    const uint32_t cells_z = samples_z - 1u;
    /* Refuse rather than emit a ragged edge tile: consumers derive the sample
     * count from the header, and a short final tile would make that a lie. */
    if (cells_x % tile_cells || cells_z % tile_cells)
        return JCE_TERRAIN_ERR_ARGS;

    const uint32_t tx   = cells_x / tile_cells;
    const uint32_t tz   = cells_z / tile_cells;
    const uint32_t span = tile_cells + 1u;
    if (span > 0xFFFFu) return JCE_TERRAIN_ERR_OVERFLOW;

    const bool   with_ridge = (ridge != NULL);
    const size_t need = jce_terrain_write_size_tiled(samples_x, samples_z,
                                                     tile_cells, with_ridge);
    if (need == 0u) return JCE_TERRAIN_ERR_ARGS;
    if (dst_size < need) return JCE_TERRAIN_ERR_TRUNCATED;

    const uint32_t mask = JCE_TERRAIN_CH_HEIGHT
                        | (with_ridge ? JCE_TERRAIN_CH_RIDGE : 0u);
    const size_t dir_bytes = (size_t)tx * tz * JCE_TERRAIN_DIRENT_BYTES;
    const size_t pay_off   = align16(JCE_TERRAIN_HEADER_BYTES + dir_bytes);
    const size_t tile_raw  = tiled_tile_raw_bytes(span, with_ridge);
    const size_t sub_count = (size_t)span * (size_t)span;
    const size_t hbytes    = sub_count * 2u;
    const size_t rbytes    = with_ridge ? sub_count * 2u : 0u;

    const float lo = base_height;
    const float hi = base_height + height_range;

    uint8_t *p = (uint8_t *)dst;
    memset(p, 0, need);

    for (uint32_t tzi = 0; tzi < tz; ++tzi) {
        for (uint32_t txi = 0; txi < tx; ++txi) {
            const size_t   ti = (size_t)tzi * tx + txi;
            uint8_t *const th = p + pay_off + ti * tile_raw;
            uint8_t *const hp = th + JCE_TERRAIN_TILEHDR_BYTES;
            uint8_t *const rp = hp + hbytes;

            /* Origin of this tile in the SOURCE grid.  Tiles start every
             * tile_cells samples and extend one past, which is what produces
             * the shared edge with the neighbour. */
            const uint32_t ox = txi * tile_cells;
            const uint32_t oz = tzi * tile_cells;

            float minh = hi, maxh = lo;
            for (uint32_t sz = 0; sz < span; ++sz) {
                for (uint32_t sx = 0; sx < span; ++sx) {
                    const size_t src  = (size_t)(oz + sz) * samples_x + (ox + sx);
                    const size_t dsti = (size_t)sz * span + sx;

                    const float v = heights[src];
                    if (!isfinite(v)) return JCE_TERRAIN_ERR_RANGE;
                    if (v < lo || v > hi) return JCE_TERRAIN_ERR_RANGE;
                    if (v < minh) minh = v;
                    if (v > maxh) maxh = v;
                    float n = (v - lo) / height_range * 65535.0f;
                    if (n < 0.0f) n = 0.0f;
                    if (n > 65535.0f) n = 65535.0f;
                    wr_u16(hp + dsti * 2u, (uint16_t)(n + 0.5f));

                    if (with_ridge) {
                        const float r = ridge[src];
                        if (!isfinite(r) || r < -1.0f || r > 1.0f)
                            return JCE_TERRAIN_ERR_RANGE;
                        float rn = (r + 1.0f) * 0.5f * 65535.0f;
                        if (rn < 0.0f) rn = 0.0f;
                        if (rn > 65535.0f) rn = 65535.0f;
                        wr_u16(rp + dsti * 2u, (uint16_t)(rn + 0.5f));
                    }
                }
            }

            wr_u32(th + 0, JCE_TERRAIN_TILE_MAGIC);
            wr_u16(th + 4, 1u);
            wr_u16(th + 6, (uint16_t)JCE_TERRAIN_TILEHDR_BYTES);
            wr_u32(th + 8, mask);
            wr_u32(th + 12, 0u);
            wr_u32(th + 16, JCE_TERRAIN_TILEHDR_BYTES);
            wr_u32(th + 20, (uint32_t)hbytes);
            if (with_ridge) {
                wr_u32(th + 48, (uint32_t)(JCE_TERRAIN_TILEHDR_BYTES + hbytes));
                wr_u32(th + 52, (uint32_t)rbytes);
            }

            const uint64_t tile_hash = (uint64_t)XXH3_64bits(th, tile_raw);

            uint8_t *const e = p + JCE_TERRAIN_HEADER_BYTES
                             + ti * JCE_TERRAIN_DIRENT_BYTES;
            wr_u32(e + 0, txi);
            wr_u32(e + 4, tzi);
            wr_u64(e + 8, (uint64_t)(pay_off + ti * tile_raw));
            wr_u32(e + 16, (uint32_t)tile_raw);
            wr_u32(e + 20, (uint32_t)tile_raw);
            wr_u64(e + 24, tile_hash);
            wr_f32(e + 32, minh);
            wr_f32(e + 36, maxh);
            wr_u16(e + 40, (uint16_t)span);
            wr_u16(e + 42, (uint16_t)span);
            wr_u16(e + 44, (uint16_t)tile_cells);
            wr_u16(e + 46, (uint16_t)tile_cells);
            wr_u32(e + 48, mask);
            e[52] = 0u;
            e[53] = height_format;
        }
    }

    const uint64_t dir_hash =
        (uint64_t)XXH3_64bits(p + JCE_TERRAIN_HEADER_BYTES, dir_bytes);

    wr_u32(p + 0, JCE_TERRAIN_MAGIC);
    wr_u16(p + 4, (uint16_t)JCE_TERRAIN_VERSION);
    wr_u16(p + 6, (uint16_t)JCE_TERRAIN_HEADER_BYTES);
    wr_u32(p + 8, 0u);
    wr_u32(p + 12, mask);
    wr_u32(p + 16, samples_x);
    wr_u32(p + 20, samples_z);
    wr_u32(p + 24, tx);
    wr_u32(p + 28, tz);
    wr_u32(p + 32, tx * tz);
    wr_u16(p + 36, (uint16_t)tile_cells);
    wr_u16(p + 38, (uint16_t)tile_cells);
    wr_u16(p + 40, (uint16_t)tile_cells);
    p[42] = height_format;
    p[46] = diagonal_rule;
    p[47] = 1u;
    wr_f32(p + 48, world_size_x);
    wr_f32(p + 52, world_size_z);
    wr_f32(p + 56, base_height);
    wr_f32(p + 60, height_range);
    wr_u64(p + 64, JCE_TERRAIN_HEADER_BYTES);
    wr_u64(p + 72, (uint64_t)dir_bytes);
    wr_u64(p + 80, (uint64_t)pay_off);
    wr_u64(p + 88, (uint64_t)(need - pay_off));
    wr_u64(p + 96, dir_hash);
    wr_u64(p + 104, 0u);
    wr_u64(p + HDR_HASH_OFFSET,
           (uint64_t)XXH3_64bits(p, HDR_HASH_DOMAIN));

    if (out_written) *out_written = need;
    return JCE_TERRAIN_OK;
}
