/*
 * jce_tilemap.h  2D tilemap data layer.
 *
 * Unity Tilemap equivalent — a 2D grid of cell indices into a sprite
 * atlas (jce_sprite_atlas).  Chunked storage lets large worlds avoid
 * allocating a flat WxH array, while keeping random-access O(1) within
 * a populated chunk.
 *
 * One Tilemap owns one atlas reference + a sparse map of chunks; each
 * chunk is a fixed CHUNK_SIZE × CHUNK_SIZE array of (sprite_index,
 * flags) cells.  Empty cells use sprite_index = 0xFFFF.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_TILEMAP_H
#define JCE_TILEMAP_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_TILEMAP_CHUNK_SIZE  16   /* tiles per chunk side */
#define JCE_TILEMAP_MAX_CHUNKS  256
#define JCE_TILEMAP_EMPTY       0xFFFFu

typedef enum {
    JCE_TILE_FLAG_FLIP_X = 1u << 0,
    JCE_TILE_FLAG_FLIP_Y = 1u << 1,
    JCE_TILE_FLAG_ROT_90 = 1u << 2,
} JceTileFlags;

typedef struct {
    uint16_t sprite_index;  /* index into the atlas; JCE_TILEMAP_EMPTY = empty */
    uint8_t  flags;
    uint8_t  variant;       /* user-defined: animation frame / random variant */
} JceTileCell;

typedef struct {
    int32_t      chunk_x;
    int32_t      chunk_y;
    JceTileCell  cells[JCE_TILEMAP_CHUNK_SIZE * JCE_TILEMAP_CHUNK_SIZE];
    bool         active;
} JceTilemapChunk;

typedef struct {
    /* Path to the sprite atlas this map references. */
    char             atlas_path[160];
    float            tile_world_size;   /* world units per tile */
    JceTilemapChunk  chunks[JCE_TILEMAP_MAX_CHUNKS];
    uint32_t         chunk_count;
} JceTilemap;

/* ── Lifecycle ───────────────────────────────────────────────── */

JCE_API void jce_tilemap_init(JceTilemap *m, const char *atlas_path,
                               float tile_world_size);
JCE_API void jce_tilemap_clear(JceTilemap *m);

/* ── Cell access ─────────────────────────────────────────────── */

/* Read the cell at integer tile coordinate (tx, ty).  Returns
 * JCE_TILEMAP_EMPTY when the chunk doesn't exist. */
JCE_API JceTileCell jce_tilemap_get(const JceTilemap *m,
                                     int32_t tx, int32_t ty);

/* Write a cell.  Creates the chunk on demand.  Returns false if the
 * chunk-table is full. */
JCE_API bool jce_tilemap_set(JceTilemap *m, int32_t tx, int32_t ty,
                              JceTileCell cell);

/* Convenience — set sprite index with default flags. */
JCE_API bool jce_tilemap_set_sprite(JceTilemap *m, int32_t tx, int32_t ty,
                                     uint16_t sprite_index);

/* Clear cell back to empty.  Doesn't free the chunk (matches Unity
 * Tilemap behaviour — chunks compact at save time). */
JCE_API bool jce_tilemap_clear_cell(JceTilemap *m, int32_t tx, int32_t ty);

/* ── Queries ─────────────────────────────────────────────────── */

JCE_API uint32_t jce_tilemap_chunk_count(const JceTilemap *m);
JCE_API const JceTilemapChunk *jce_tilemap_chunk_at(const JceTilemap *m,
                                                      uint32_t idx);

/* Count non-empty cells across all chunks (linear). */
JCE_API uint32_t jce_tilemap_nonempty_count(const JceTilemap *m);

/* JSON round-trip. */
JCE_API bool jce_tilemap_save_json(const JceTilemap *m, const char *path);
JCE_API bool jce_tilemap_load_json(JceTilemap *m, const char *path);

JCE_EXTERN_C_END

#endif /* JCE_TILEMAP_H */
