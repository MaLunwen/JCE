/*
 * jce_terrain_format.h -- cooked terrain v3 binary codec (resource-private).
 *
 * WHY A NEW FORMAT
 * ----------------
 * v1/v2 wrote native `float` and `uint32_t` arrays straight to disk while
 * documenting the file as little-endian, had no per-tile checksums, no
 * random-access directory, and no independently compressed payloads.  A
 * 4097x4097 terrain therefore had to be loaded whole, could not be verified,
 * and could not be streamed.
 *
 * v3 is a fixed little-endian header plus a tile directory, with each tile
 * payload stored independently (raw or zstd) and hashed on its own.  That is
 * what makes paging, corruption isolation and deterministic cook hashes
 * possible at the same time.
 *
 * RULES THIS CODEC ENFORCES
 * -------------------------
 *  - Public code never casts file bytes to a C struct.  Every field is read
 *    and written through explicit little-endian helpers, so the on-disk layout
 *    does not depend on host endianness, struct packing or alignment.
 *  - Every offset and size is bounds-checked against the actual buffer before
 *    it is used, and every multiplication that could overflow is checked.
 *  - Reserved bytes must be zero.  A non-zero reserved field means the writer
 *    was a newer version making assumptions this reader does not share.
 *  - Heights are decoded to local-space Y.  Normalised storage values never
 *    escape this module.
 *
 * TWO FIELDS ADDED BEYOND THE ORIGINAL v3 DRAFT
 * ---------------------------------------------
 *  - `diagonal_rule`: which way each quad is triangulated.  The renderer mesh,
 *    the CPU raycast, the hole trimesh and the Bullet heightfield must all
 *    agree, or they differ by the full corner-to-corner height AT CELL
 *    CENTRES -- a disagreement no vertex-sampled test can detect.  Making it
 *    data rather than four independent code decisions is the only way to keep
 *    them aligned.
 *  - `weight_texture_count`: four material weights per tile is a product
 *    ceiling, not a v1 default.  Reserving the field now costs nothing;
 *    discovering later that a fifth layer needs a format break is expensive.
 *
 * Layer: Resource (L3) -- PRIVATE.  Not part of any public umbrella.
 */

#ifndef JCE_TERRAIN_FORMAT_H
#define JCE_TERRAIN_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JCE_TERRAIN_MAGIC        0x5245544Au /* 'JTER' little-endian */
#define JCE_TERRAIN_VERSION      3u
#define JCE_TERRAIN_HEADER_BYTES 128u
#define JCE_TERRAIN_DIRENT_BYTES 64u
#define JCE_TERRAIN_TILEHDR_BYTES 64u
#define JCE_TERRAIN_TILE_MAGIC   0x4C49544Au /* 'JTIL' little-endian */

/* Channel bits.  Fixed: the writer emits sections in this bit order. */
#define JCE_TERRAIN_CH_HEIGHT   0x01u
#define JCE_TERRAIN_CH_WEIGHTS  0x02u
#define JCE_TERRAIN_CH_HOLES    0x04u
#define JCE_TERRAIN_CH_NORMALS  0x08u
#define JCE_TERRAIN_CH_RIDGE    0x10u
#define JCE_TERRAIN_CH_DRAINAGE 0x20u

/* Height encodings. */
#define JCE_TERRAIN_HEIGHT_R16_UNORM 1u
#define JCE_TERRAIN_HEIGHT_F32_LE    2u

/* Quad triangulation rule -- mirrors JceHeightfieldDiagonal so the collision
 * bridge and the cooked bytes cannot disagree. */
#define JCE_TERRAIN_DIAG_FIXED   0u
#define JCE_TERRAIN_DIAG_DIAMOND 1u
#define JCE_TERRAIN_DIAG_ZIGZAG  2u

typedef enum {
    JCE_TERRAIN_OK = 0,
    JCE_TERRAIN_ERR_ARGS,          /* null / nonsense caller input           */
    JCE_TERRAIN_ERR_TRUNCATED,     /* buffer shorter than the header claims  */
    JCE_TERRAIN_ERR_MAGIC,         /* not a JTER file                        */
    JCE_TERRAIN_ERR_VERSION,       /* version this reader does not implement */
    JCE_TERRAIN_ERR_HEADER_HASH,   /* header self-hash mismatch              */
    JCE_TERRAIN_ERR_DIR_HASH,      /* directory hash mismatch                */
    JCE_TERRAIN_ERR_TILE_HASH,     /* tile payload hash mismatch             */
    JCE_TERRAIN_ERR_RANGE,         /* an offset/size escapes the buffer      */
    JCE_TERRAIN_ERR_RESERVED,      /* reserved bytes were not zero           */
    JCE_TERRAIN_ERR_OVERFLOW,      /* a size computation would overflow      */
    JCE_TERRAIN_ERR_UNSUPPORTED    /* a valid file this build cannot decode  */
} JceTerrainStatus;

typedef struct {
    uint32_t width_samples;
    uint32_t height_samples;
    uint32_t tile_count_x;
    uint32_t tile_count_z;
    uint32_t tile_count;
    uint32_t channel_mask;

    uint16_t storage_tile_cells;
    uint16_t render_chunk_cells;
    uint16_t collision_tile_cells;

    uint8_t  height_format;
    uint8_t  weight_format;
    uint8_t  hole_format;
    uint8_t  normal_format;
    uint8_t  diagonal_rule;
    uint8_t  weight_texture_count;

    float    world_size_x;
    float    world_size_z;
    float    base_height;
    float    height_range;

    uint64_t directory_offset;
    uint64_t directory_bytes;
    uint64_t payload_offset;
    uint64_t payload_bytes;
} JceTerrainHeader;

typedef struct {
    uint32_t tile_x;
    uint32_t tile_z;
    uint64_t stored_offset;
    uint32_t stored_bytes;
    uint32_t raw_bytes;
    uint64_t raw_hash;
    float    min_height;
    float    max_height;
    uint16_t sample_width;
    uint16_t sample_height;
    uint16_t cell_width;
    uint16_t cell_height;
    uint32_t channel_mask;
    uint8_t  compression;   /* 0 = raw, 1 = zstd */
    uint8_t  height_format;
} JceTerrainDirEntry;

/* ── Reading ──────────────────────────────────────────────────────── */

/* Parse and VERIFY the header: magic, version, self-hash, reserved bytes, and
 * that every declared region lies inside `size`.  Cheap -- no tile touched. */
JceTerrainStatus jce_terrain_read_header(const void *data, size_t size,
                                         JceTerrainHeader *out);

/* Parse and verify the whole directory (including its hash).  `out_entries`
 * must hold header->tile_count entries. */
JceTerrainStatus jce_terrain_read_directory(const void *data, size_t size,
                                            const JceTerrainHeader *hdr,
                                            JceTerrainDirEntry *out_entries);

/* Decoded heights for one tile, in LOCAL-SPACE Y.  `out_heights` must hold
 * entry->sample_width * entry->sample_height floats.  Verifies the tile hash
 * before decoding and rejects any value outside the declared interval. */
JceTerrainStatus jce_terrain_read_tile_heights(const void *data, size_t size,
                                               const JceTerrainHeader *hdr,
                                               const JceTerrainDirEntry *entry,
                                               float *out_heights);

/* ── Writing ──────────────────────────────────────────────────────── */

/* Bytes a single-tile height-only file occupies.  Lets callers allocate
 * exactly, so the writer never needs to grow a buffer. */
size_t jce_terrain_write_size_height_only(uint32_t samples_x,
                                          uint32_t samples_z);

/* Serialise a height-only, single-tile, uncompressed v3 file.
 *
 * Deliberately the narrow case: it is enough to prove the header, directory,
 * payload framing and every hash domain round-trip, which is what the store,
 * the cooker and the streaming layer all build on.  Multi-tile and compressed
 * writing extend this, they do not replace it. */
JceTerrainStatus jce_terrain_write_height_only(
    void *dst, size_t dst_size,
    const float *heights, uint32_t samples_x, uint32_t samples_z,
    float world_size_x, float world_size_z,
    float base_height, float height_range,
    uint8_t height_format, uint8_t diagonal_rule,
    size_t *out_written);

/* ── Ridge channel ───────────────────────────────────────────────────
 *
 * The ridge/crease mask produced by the erosion pass, in [-1,1]: -1 deep in a
 * crease, +1 on a ridge.  Cooked alongside the heights because it is DERIVED
 * from them -- recomputing it at load would mean shipping the erosion filter
 * and its exact parameters into the runtime, and any drift between the two
 * would silently move every splat boundary and foliage cluster that reads it.
 *
 * Stored as 16-bit unorm of (r+1)/2, so it costs the same as the height plane
 * and resolves to about 3e-5 -- far finer than anything that consumes a mask.
 *
 * The section sits at channel index 4 in the fixed bit order, so a reader that
 * does not know about ridge sees a channel bit it ignores and a section it
 * never visits; files without CH_RIDGE stay readable unchanged. */
size_t jce_terrain_write_size_height_ridge(uint32_t samples_x,
                                           uint32_t samples_z);

/* As jce_terrain_write_height_only, plus an optional ridge plane.  Passing
 * NULL for `ridge` produces a BYTE-IDENTICAL file to the height-only writer,
 * so gaining the capability cannot change existing output. */
JceTerrainStatus jce_terrain_write_height_ridge(
    void *dst, size_t dst_size,
    const float *heights, const float *ridge,
    uint32_t samples_x, uint32_t samples_z,
    float world_size_x, float world_size_z,
    float base_height, float height_range,
    uint8_t height_format, uint8_t diagonal_rule,
    size_t *out_written);

/* Decode the ridge plane.  Returns JCE_TERRAIN_ERR_UNSUPPORTED when the tile
 * carries no ridge channel -- distinct from a decode failure, so a caller can
 * fall back to "no mask" rather than treating it as corruption. */
JceTerrainStatus jce_terrain_read_tile_ridge(const void *data, size_t size,
                                             const JceTerrainHeader *hdr,
                                             const JceTerrainDirEntry *entry,
                                             float *out_ridge);

/* ── Multi-tile cooking ──────────────────────────────────────────────
 *
 * The single-tile writers above prove the framing; this is what makes the
 * format do its job.  A 4097x4097 terrain is 200 MB of heights -- the whole
 * reason v3 has a directory is so a consumer can page ONE tile without reading
 * any of the others, and that only pays off if the cooker actually emits many.
 *
 * `tile_cells` is the tile size in CELLS.  Each tile stores
 * (tile_cells + 1)^2 samples: a one-sample overlap with its right and bottom
 * neighbours, so a bilinear sample anywhere inside a tile's cell range never
 * has to straddle two tiles.  Without that overlap every tile boundary needs a
 * second tile resident to interpolate across, which defeats the paging.
 *
 * (samples_x - 1) and (samples_z - 1) must both be divisible by `tile_cells`,
 * otherwise the grid cannot be covered exactly and the request is REFUSED --
 * a partial edge tile would silently change the sample count consumers derive
 * from the header.
 *
 * `ridge` is optional and, when present, is tiled identically. */
size_t jce_terrain_write_size_tiled(uint32_t samples_x, uint32_t samples_z,
                                    uint32_t tile_cells, bool with_ridge);

JceTerrainStatus jce_terrain_write_tiled(
    void *dst, size_t dst_size,
    const float *heights, const float *ridge,
    uint32_t samples_x, uint32_t samples_z, uint32_t tile_cells,
    float world_size_x, float world_size_z,
    float base_height, float height_range,
    uint8_t height_format, uint8_t diagonal_rule,
    size_t *out_written);

const char *jce_terrain_status_str(JceTerrainStatus s);

#ifdef __cplusplus
}
#endif

#endif /* JCE_TERRAIN_FORMAT_H */
