/*
 * jce_tilemap.h  Tilemap + tileset asset loaders (pure CPU).
 *
 * Parses the editor-authored sidecar formats:
 *   .tilemap.json  {"sprites","w","h","cellPx","cells":[w*h ints]}
 *                  (Tile Palette panel; cell 0 = empty, k = rects[k-1])
 *   .sprites.json  {"source","sourceW","sourceH","rects":[{name,x,y,w,h}]}
 *                  (Sprite Editor panel; the tile atlas description)
 *
 * Conventions (consumed by the scene renderer + runtime collider spawn):
 *   - 1 cell = 1 world unit in entity-local XY, scaled by the entity scale.
 *   - cell (col,row) spans local [col,col+1] x [-(row+1),-row]
 *     (rows grow DOWN, matching the Tile Palette's screen-space grid).
 *   - UVs are rect/sourceW-H; any non-zero cell counts as solid.
 *
 * Loading is tolerant: missing keys fall back to defaults, out-of-range
 * values are clamped, short "cells" arrays pad with empty.
 *
 * Thread safety: assets are immutable after load; load/unload from any
 * single thread.
 *
 * Layer: Middleware (Layer 4 — scene assets).
 */

#ifndef JCE_TILEMAP_H
#define JCE_TILEMAP_H

#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

struct JcePakArchive;

/* Hard cap on w*h so a corrupt file cannot drive a giant allocation. */
#define JCE_TILEMAP_MAX_CELLS     (1024 * 1024)
/* Suggested capacity for jce_tilemap_solid_rects() output buffers. */
#define JCE_TILEMAP_COL_MAX_RECTS 1024

/* ── Tilemap asset (.tilemap.json) ────────────────────────────────── */

typedef struct JceTilemapAsset JceTilemapAsset;

/* Parse from a memory buffer (`len` bytes; 0 = NUL-terminated). */
JCE_API JceTilemapAsset *jce_tilemap_load_mem(const char *data, size_t len);
/* Parse from a file on disk. */
JCE_API JceTilemapAsset *jce_tilemap_load_file(const char *path);
/* Parse from a PAK entry (deployed bundles).  NULL when not in the PAK. */
JCE_API JceTilemapAsset *jce_tilemap_load_from_pak(const struct JcePakArchive *pak,
                                                   const char *vpath);
/* Source-compatible alias of jce_tilemap_load_file (pre-P5 stub API). */
JCE_API JceTilemapAsset *jce_tilemap_load(const char *path);
JCE_API void             jce_tilemap_unload(JceTilemapAsset *t);

JCE_API uint32_t         jce_tilemap_width (const JceTilemapAsset *t);
JCE_API uint32_t         jce_tilemap_height(const JceTilemapAsset *t);
/* Tile id at (x,y): 0 = empty, k = tileset rect k-1.  OOB returns 0. */
JCE_API uint32_t         jce_tilemap_tile_at(const JceTilemapAsset *t, uint32_t x, uint32_t y);
/* The "sprites" key authored into the map (may be "" when absent). */
JCE_API const char      *jce_tilemap_sprites_path(const JceTilemapAsset *t);

/* ── Tileset asset (.sprites.json) ────────────────────────────────── */

typedef struct JceTilesetAsset JceTilesetAsset;

JCE_API JceTilesetAsset *jce_tileset_load_mem(const char *data, size_t len);
JCE_API JceTilesetAsset *jce_tileset_load_file(const char *path);
JCE_API JceTilesetAsset *jce_tileset_load_from_pak(const struct JcePakArchive *pak,
                                                   const char *vpath);
JCE_API void             jce_tileset_unload(JceTilesetAsset *ts);

/* The "source" atlas image path (may be "" when absent). */
JCE_API const char      *jce_tileset_image_path(const JceTilesetAsset *ts);
JCE_API uint32_t         jce_tileset_rect_count(const JceTilesetAsset *ts);

/* UV sub-rect for tile `id` (1-based, matching tilemap cell values).
 * Returns false for id 0 / id > rect_count / sourceW-H <= 0 — callers
 * should treat such tiles as empty. */
JCE_API bool             jce_tileset_tile_uv(const JceTilesetAsset *ts, uint32_t id,
                                             float *u0, float *v0,
                                             float *u1, float *v1);

/* ── Solid-cell extraction (2D collider spawn) ────────────────────── */

/* A merged rectangle of solid cells, in CELL units (x,y = top-left cell,
 * w,h >= 1).  Local-space extents follow the cell convention above. */
typedef struct {
    uint16_t x, y, w, h;
} JceTilemapSolidRect;

/* Greedy-merge every non-zero cell into axis-aligned rectangles
 * (maximal horizontal runs, then vertical extension of equal-width runs).
 * Writes at most `max` rects into `out` and returns the TOTAL number of
 * merged rects — a return value > max means the output was truncated. */
JCE_API uint32_t         jce_tilemap_solid_rects(const JceTilemapAsset *t,
                                                 JceTilemapSolidRect *out,
                                                 uint32_t max);

JCE_EXTERN_C_END

#endif /* JCE_TILEMAP_H */
