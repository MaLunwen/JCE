#include "jce_model_texture_pool.h"
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_hash.h>

typedef struct {
    uint64_t key;
    int mode;
    JceTextureCpu *cpu;
    JceTexture gpu;
    bool uploaded;
} ModelTexture;

struct JceModelTexturePool {
    ModelTexture *textures;
    uint32_t *buckets;
    uint32_t count, capacity;
    bool upload_started;
};

static uint32_t texture_bucket(uint64_t key, int mode, uint32_t mask)
{
    return (uint32_t)(jce_hash_fmix64(key) ^
        jce_hash_fmix64((uint32_t)mode)) & mask;
}

JceModelTexturePool *jce_model_texture_pool_create(void)
{
    return JCE_NEW(JceModelTexturePool);
}

uint32_t jce_model_texture_pool_acquire(JceModelTexturePool *pool,
    uint64_t key, int mode, JceModelTextureDecodeFn decode, void *user)
{
    if (!pool || !key || !decode || pool->upload_started) return 0;

    if (pool->capacity) {
        uint32_t mask = pool->capacity * 2u - 1u;
        uint32_t bucket = texture_bucket(key, mode, mask);
        while (pool->buckets[bucket]) {
            uint32_t slot = pool->buckets[bucket];
            ModelTexture *entry = &pool->textures[slot - 1u];
            if (entry->key == key && entry->mode == mode) return slot;
            bucket = (bucket + 1u) & mask;
        }
    }

    if (pool->count == pool->capacity) {
        if (pool->capacity > UINT32_MAX / 4u) return 0;
        uint32_t capacity = pool->capacity ? pool->capacity * 2u : 8u;
        if ((uint64_t)capacity > SIZE_MAX / sizeof(ModelTexture) ||
            (uint64_t)capacity * 2u > SIZE_MAX / sizeof(uint32_t)) return 0;
        uint32_t *buckets = JCE_CALLOC((size_t)capacity * 2u, sizeof(uint32_t));
        if (!buckets) return 0;
        ModelTexture *textures = JCE_REALLOC(pool->textures,
            (size_t)capacity * sizeof(ModelTexture));
        if (!textures) { JCE_FREE(buckets); return 0; }
        uint32_t mask = capacity * 2u - 1u;
        for (uint32_t i = 0; i < pool->count; ++i) {
            uint32_t bucket = texture_bucket(textures[i].key, textures[i].mode, mask);
            while (buckets[bucket]) bucket = (bucket + 1u) & mask;
            buckets[bucket] = i + 1u;
        }
        JCE_FREE(pool->buckets);
        pool->buckets = buckets;
        pool->textures = textures;
        pool->capacity = capacity;
    }
    ModelTexture *entry = &pool->textures[pool->count++];
    entry->key = key;
    entry->mode = mode;
    entry->gpu = JCE_TEXTURE_INVALID;
    entry->uploaded = false;
    entry->cpu = decode(user, mode);
    uint32_t mask = pool->capacity * 2u - 1u;
    uint32_t bucket = texture_bucket(key, mode, mask);
    while (pool->buckets[bucket]) bucket = (bucket + 1u) & mask;
    pool->buckets[bucket] = pool->count;
    return pool->count;
}

JceTexture jce_model_texture_pool_upload(JceModelTexturePool *pool, uint32_t slot)
{
    if (!pool || !slot || slot > pool->count) return JCE_TEXTURE_INVALID;
    pool->upload_started = true;
    ModelTexture *entry = &pool->textures[slot - 1u];
    if (!entry->uploaded) {
        entry->uploaded = true;
        if (entry->cpu) entry->gpu = jce_texture_upload_cpu(entry->cpu);
        entry->cpu = NULL;
    }
    return entry->gpu;
}

uint64_t jce_model_texture_pool_gpu_bytes(const JceModelTexturePool *pool)
{
    uint64_t bytes = 0;
    if (!pool) return 0;
    for (uint32_t i = 0; i < pool->count; ++i) {
        if (!jce_texture_valid(pool->textures[i].gpu)) continue;
        uint32_t width = 0, height = 0;
        jce_texture_get_size(pool->textures[i].gpu, &width, &height);
        bytes += (uint64_t)width * height * 4u;
    }
    return bytes;
}

void jce_model_texture_pool_destroy(JceModelTexturePool *pool)
{
    if (!pool) return;
    for (uint32_t i = 0; i < pool->count; ++i) {
        jce_texture_cpu_free(pool->textures[i].cpu);
        if (jce_texture_valid(pool->textures[i].gpu))
            jce_texture_destroy(pool->textures[i].gpu);
    }
    JCE_FREE(pool->textures);
    JCE_FREE(pool->buckets);
    JCE_FREE(pool);
}
