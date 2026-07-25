/*
 * jce_sprite_batch.h  Batched 2D sprite renderer.
 *
 * Collects textured quads (sprites), sorts by texture, and flushes
 * them as minimal draw calls using transient vertex/index buffers.
 */

#ifndef JCE_SPRITE_BATCH_H
#define JCE_SPRITE_BATCH_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_texture_types.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceSpriteBatch JceSpriteBatch;
typedef struct JceRenderer JceRenderer;

/* Create a sprite batch (max_sprites = maximum quads per flush). */
JCE_API JceSpriteBatch *jce_sprite_batch_create(uint32_t max_sprites);
JCE_API void            jce_sprite_batch_destroy(JceSpriteBatch *batch);

/* Begin a new batch frame. */
JCE_API void jce_sprite_batch_begin(JceSpriteBatch *batch);

/*
 * Add a sprite quad to the batch.
 *
 * @param batch     The sprite batch.
 * @param texture   Texture handle for the sprite atlas.
 * @param world     4x4 world transform matrix (billboard or 3D placement).
 * @param uv_min    UV min corner {u0, v0}.
 * @param uv_max    UV max corner {u1, v1}.
 * @param color     ABGR color tint.
 * @param sort_key  Sorting key (lower = drawn first).
 */
JCE_API void jce_sprite_batch_add(JceSpriteBatch *batch,
                           JceTexture texture,
                           const float *world,
                           float u0, float v0, float u1, float v1,
                           uint32_t color, int32_t sort_key);

/* Flush all queued sprites as draw calls.
 * Uses transient buffers for zero-copy GPU upload. */
JCE_API void jce_sprite_batch_flush(JceSpriteBatch *batch,
                             const JceRenderer *renderer,
                             uint16_t view_id);

/* Get number of sprites queued this frame. */
JCE_API uint32_t jce_sprite_batch_count(const JceSpriteBatch *batch);

JCE_EXTERN_C_END

#endif /* JCE_SPRITE_BATCH_H */
