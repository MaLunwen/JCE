/* Model-local texture ownership across worker decode and render upload. */
#ifndef JCE_MODEL_TEXTURE_POOL_H
#define JCE_MODEL_TEXTURE_POOL_H

#include <jce/renderer/jce_texture.h>

typedef struct JceModelTexturePool JceModelTexturePool;
typedef JceTextureCpu *(*JceModelTextureDecodeFn)(void *user, int mode);

/* Slots start at one; zero means absent. Keys identify images within this
 * model, not host paths. Mode includes color space and sampler flags.
 * Decode failures are remembered. Allocation failure returns zero without
 * decoding. Acquisition ends before the first upload. */
JceModelTexturePool *jce_model_texture_pool_create(void);
uint32_t jce_model_texture_pool_acquire(JceModelTexturePool *pool,
    uint64_t key, int mode, JceModelTextureDecodeFn decode, void *user);

/* Render-thread only; each slot consumes its CPU staging at most once,
 * including failed uploads. Repeated references borrow the same handle. */
JceTexture jce_model_texture_pool_upload(JceModelTexturePool *pool, uint32_t slot);
uint64_t jce_model_texture_pool_gpu_bytes(const JceModelTexturePool *pool);

/* Worker-safe before upload. Once any slot is uploaded, destruction belongs
 * to the render thread. Frees pending CPU data and unique GPU handles once. */
void jce_model_texture_pool_destroy(JceModelTexturePool *pool);

#endif
