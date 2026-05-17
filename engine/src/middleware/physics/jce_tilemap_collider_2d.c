/*
 * jce_tilemap_collider_2d.c  Tilemap → Box2D shape list.
 *
 * Row-coalescing algorithm: for each chunk row, scan left-to-right;
 * grow the current "run" while cells are solid + match the sprite
 * mask; emit a single box when the run ends.  Cheap, deterministic,
 * 2-4× fewer shapes than per-cell.
 */

#include <jce/middleware/physics/jce_tilemap_collider_2d.h>

#include <string.h>

static bool cell_is_solid(const JceTilemap *map,
                            const JceTilemapColliderOptions *opts,
                            int32_t tx, int32_t ty)
{
    JceTileCell c = jce_tilemap_get(map, tx, ty);
    if (c.sprite_index == JCE_TILEMAP_EMPTY) return false;
    if (opts->sprite_mask == 0) return true;
    uint8_t lo = (uint8_t)(c.sprite_index & 0xFF);
    return (opts->sprite_mask & (uint32_t)(1u << (lo & 31))) != 0;
}

static void emit(JceTilemapColliderShape *out, uint32_t *n, uint32_t cap,
                  const JceTilemapColliderOptions *opts,
                  float x, float y, float w, float h)
{
    if (*n >= cap) return;
    JceTilemapColliderShape *s = &out[(*n)++];
    s->x = x; s->y = y; s->w = w; s->h = h;
    s->friction    = opts->default_friction;
    s->restitution = opts->default_restitution;
    s->is_trigger  = false;
}

uint32_t jce_tilemap_collider_2d_rebuild(const JceTilemap *map,
                                           const JceTilemapColliderOptions *opts_in,
                                           JceTilemapColliderShape *out,
                                           uint32_t cap)
{
    if (!map || !out || cap == 0) return 0;
    JceTilemapColliderOptions opts;
    if (opts_in) opts = *opts_in;
    else memset(&opts, 0, sizeof(opts));

    float ts = map->tile_world_size > 0 ? map->tile_world_size : 1.0f;
    uint32_t n = 0;

    /* Iterate every active chunk; within each, scan rows. */
    for (uint32_t ci = 0; ci < map->chunk_count && n < cap; ++ci) {
        const JceTilemapChunk *chunk = jce_tilemap_chunk_at(map, ci);
        if (!chunk) continue;
        int32_t cx0 = chunk->chunk_x * JCE_TILEMAP_CHUNK_SIZE;
        int32_t cy0 = chunk->chunk_y * JCE_TILEMAP_CHUNK_SIZE;

        for (int row = 0; row < JCE_TILEMAP_CHUNK_SIZE && n < cap; ++row) {
            int32_t ty = cy0 + row;
            if (!opts.coalesce_rows) {
                /* Emit one shape per solid cell. */
                for (int col = 0; col < JCE_TILEMAP_CHUNK_SIZE && n < cap; ++col) {
                    int32_t tx = cx0 + col;
                    if (!cell_is_solid(map, &opts, tx, ty)) continue;
                    emit(out, &n, cap, &opts, tx * ts, ty * ts, ts, ts);
                }
            } else {
                int run_start = -1;
                for (int col = 0; col <= JCE_TILEMAP_CHUNK_SIZE && n < cap; ++col) {
                    bool solid = false;
                    if (col < JCE_TILEMAP_CHUNK_SIZE) {
                        int32_t tx = cx0 + col;
                        solid = cell_is_solid(map, &opts, tx, ty);
                    }
                    if (solid) {
                        if (run_start < 0) run_start = col;
                    } else if (run_start >= 0) {
                        int len = col - run_start;
                        int32_t tx0 = cx0 + run_start;
                        emit(out, &n, cap, &opts,
                              tx0 * ts, ty * ts, ts * len, ts);
                        run_start = -1;
                    }
                }
            }
        }
    }
    return n;
}
