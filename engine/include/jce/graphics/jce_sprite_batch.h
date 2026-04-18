/*
 * jce_sprite_batch.h  Batched 2D sprite renderer.
 *
 * Collects textured quads (sprites), sorts by texture, and flushes
 * them as minimal draw calls using transient vertex/index buffers.
 */

#ifndef JCE_SPRITE_BATCH_H
#define JCE_SPRITE_BATCH_H

#include <jce/core/jce_math.h>
#include <bgfx/c99/bgfx.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceSpriteBatch JceSpriteBatch;
typedef struct JceRenderer JceRenderer;

/* Create a sprite batch (max_sprites = maximum quads per flush). */
JceSpriteBatch *jce_sprite_batch_create(uint32_t max_sprites);
void            jce_sprite_batch_destroy(JceSpriteBatch *batch);

/* Begin a new batch frame. */
void jce_sprite_batch_begin(JceSpriteBatch *batch);

/*
 * Add a sprite quad to the batch.
 *
 * @param batch     The sprite batch.
 * @param texture   bgfx texture handle for the sprite atlas.
 * @param world     4x4 world transform matrix (billboard or 3D placement).
 * @param uv_min    UV min corner {u0, v0}.
 * @param uv_max    UV max corner {u1, v1}.
 * @param color     ABGR color tint.
 * @param sort_key  Sorting key (lower = drawn first).
 */
void jce_sprite_batch_add(JceSpriteBatch *batch,
                           bgfx_texture_handle_t texture,
                           const float *world,
                           float u0, float v0, float u1, float v1,
                           uint32_t color, int32_t sort_key);

/* Flush all queued sprites as draw calls.
 * Uses transient buffers for zero-copy GPU upload. */
void jce_sprite_batch_flush(JceSpriteBatch *batch,
                             const JceRenderer *renderer,
                             uint16_t view_id);

/* Get number of sprites queued this frame. */
uint32_t jce_sprite_batch_count(const JceSpriteBatch *batch);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SPRITE_BATCH_H */
