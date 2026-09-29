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

/*
 * Eye position for this frame, in world space.
 *
 * The batch no longer writes depth (see jce_sprite_batch.c), so sprites do
 * not occlude each other through the depth buffer and something has to put
 * them in back-to-front order.  That something is this: distance from the eye
 * breaks ties AFTER the authored sort key, which is the standard transparent
 * sort and matches the order Unity resolves (sorting layer, order, distance).
 *
 * Not called, or called with a stale eye, and coincident-key sprites merely
 * composite in an arbitrary order -- it does not corrupt the frame.
 */
JCE_API void jce_sprite_batch_set_view_pos(JceSpriteBatch *batch,
                                           float x, float y, float z);

/*
 * Eye position for this frame, in world space.
 *
 * The batch no longer writes depth (see jce_sprite_batch.c), so sprites do
 * not occlude each other through the depth buffer and something has to put
 * them in back-to-front order.  That something is this: distance from the eye
 * breaks ties AFTER the authored sort key, which is the standard transparent
 * sort and matches the order Unity resolves (sorting layer, order, distance).
 *
 * Not called, or called with a stale eye, and coincident-key sprites merely
 * composite in an arbitrary order -- it does not corrupt the frame.
 */
JCE_API void jce_sprite_batch_set_view_pos(JceSpriteBatch *batch,
                                           float x, float y, float z);

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
 * @param sort_key  Sorting key (lower = drawn first).  Build it with
 *                  jce_sprite_sort_key() rather than passing a bare number:
 *                  a caller that passes 0 while another passes a packed key
 *                  is not "unsorted", it is sorted to the very front.
 */
JCE_API void jce_sprite_batch_add(JceSpriteBatch *batch,
                           JceTexture texture,
                           const float *world,
                           float u0, float v0, float u1, float v1,
                           uint32_t color, int32_t sort_key);

/*
 * Pack (sorting layer, order in layer) into one comparable key.
 *
 * Unity's 2D sort is two-level and the coarse level wins outright, so the
 * layer occupies the high bits and the order the low ones; comparing the
 * packed ints is then exactly the lexicographic (layer, order) compare.
 * Order is biased into unsigned range so negatives sort below zero rather
 * than above everything.
 *
 * (0, 0) -- the neutral value every caller without author-controlled sorting
 * passes -- must round-trip through here too, NOT be written as a literal 0:
 * literal 0 decodes as order -32768 and would draw sprite animators and
 * billboards in front of every ordinary sprite.
 */
JCE_INLINE int32_t jce_sprite_sort_key(int sorting_layer, int sorting_order)
{
    int l = sorting_layer < 0 ? 0 : (sorting_layer > 32767 ? 32767 : sorting_layer);
    int o = sorting_order < -32768 ? -32768
          : (sorting_order > 32767 ? 32767 : sorting_order);
    return (int32_t)(((uint32_t)l << 16) | (uint32_t)(o + 32768));
}

/* Flush all queued sprites as draw calls.
 * Uses transient buffers for zero-copy GPU upload. */
JCE_API void jce_sprite_batch_flush(JceSpriteBatch *batch,
                             const JceRenderer *renderer,
                             uint16_t view_id);

/* Get number of sprites queued this frame. */
JCE_API uint32_t jce_sprite_batch_count(const JceSpriteBatch *batch);

JCE_EXTERN_C_END

#endif /* JCE_SPRITE_BATCH_H */
