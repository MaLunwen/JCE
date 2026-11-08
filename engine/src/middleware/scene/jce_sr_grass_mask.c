/* Exact-content reuse after the ordinary bounded asset read. */
#include "jce_sr_grass_mask.h"
#include "jce_sr_foliage_cooked.h"
#include "os/core/jce_trace_scope_internal.h"
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_hash.h>
#include <jce/resource/jce_image_decode.h>
#include <stdlib.h>
#include <string.h>

#define SR_GRASS_MASK_MEMO_BYTES (256u * 1024u)

typedef struct SrGrassMaskEntry {
    void *encoded;
    size_t encoded_size;
    SrFoliageBlobOwner encoded_owner;
    JceFileSystem *fs;
    JceFsActivePolicy policy;
    char path[256];
    float threshold;
    float *cells;
    int dim;
    uint32_t content_hash;
} SrGrassMaskEntry;

bool sr_grass_mask_batch_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("JCE_GRASS_MASK_BATCH");
        enabled = value && strcmp(value, "1") == 0;
    }
    return enabled != 0;
}

static void sr_grass_mask_entry_clear(SrGrassMaskEntry *entry)
{
    sr_foliage_blob_free(entry->encoded, entry->encoded_owner);
    JCE_FREE(entry->cells);
}

void sr_grass_mask_batch_end(SrGrassMaskBatch *batch)
{
    if (!batch->entry) return;
    sr_grass_mask_entry_clear(batch->entry);
    JCE_FREE(batch->entry);
    batch->entry = NULL;
}

void sr_grass_mask_release(SrGrassMaskLease *lease)
{
    if (lease->owned) JCE_FREE((void *)lease->cells);
    *lease = (SrGrassMaskLease){0};
}

SrGrassMaskStatus sr_grass_mask_acquire(JceSceneRenderer *sr,
    SrGrassMaskBatch *batch, const char *path, float threshold, SrGrassMaskLease *lease)
{
    *lease = (SrGrassMaskLease){0};
    const JceTraceScope scope = jce_trace_scope_begin("sr.grass.mask.acquire");
    SrGrassMaskStatus status = SR_GRASS_MASK_UNAVAILABLE;
    JceFileSystem *fs = jce_fs_get_active();
    const JceFsActivePolicy policy = jce_fs_get_active_policy();
    size_t size = 0;
    SrFoliageBlobOwner owner = SR_FOLIAGE_BLOB_NONE;
    void *encoded = sr_foliage_read_image_blob(sr, path, &size, &owner);
    const bool stable = fs == jce_fs_get_active() && policy == jce_fs_get_active_policy();
    if (!encoded) goto done;
    const size_t path_size = strlen(path) + 1u;
    const bool admit = batch && stable && size > 0u && size <= SR_GRASS_MASK_MEMO_BYTES &&
        path_size <= sizeof(((SrGrassMaskEntry *)0)->path);
    SrGrassMaskEntry *entry = admit ? batch->entry : NULL;
    if (entry && entry->fs == fs && entry->policy == policy &&
        strcmp(entry->path, path) == 0 &&
        memcmp(&entry->threshold, &threshold, sizeof threshold) == 0 &&
        entry->encoded_size == size && memcmp(entry->encoded, encoded, size) == 0) {
        *lease = (SrGrassMaskLease){entry->cells, entry->dim, entry->content_hash, false};
        status = SR_GRASS_MASK_REUSED;
        goto done;
    }

    JceImage image = {0};
    const JceTraceScope decode = jce_trace_scope_begin("sr.grass.mask.decode");
    const bool decoded = jce_image_decode(encoded, size, &image) && image.pixels &&
        image.width && image.height && image.width <= 4096u && image.height <= 4096u;
    jce_trace_scope_end(decode, decoded ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    if (!admit) {
        sr_foliage_blob_free(encoded, owner);
        encoded = NULL;
    }
    if (!decoded) {
        jce_image_free(&image);
        goto done;
    }

    const JceTraceScope grid_scope = jce_trace_scope_begin("sr.grass.mask.grid");
    uint32_t hash = jce_fnv1a32_append(JCE_FNV1A32_INIT, image.pixels,
                                      (size_t)image.width * image.height * 4u);
    hash = jce_fnv1a32_append(hash, &image.width, sizeof image.width);
    hash = jce_fnv1a32_append(hash, &image.height, sizeof image.height);
    const int dim = image.width > 256u ? 256 : (int)image.width;
    float *grid = (float *)JCE_MALLOC((size_t)dim * (size_t)dim * sizeof *grid);
    if (!grid) {
        jce_image_free(&image);
        jce_trace_scope_end(grid_scope, JCE_TRACE_TASK_FAILED);
        status = SR_GRASS_MASK_ALLOCATION_FAILED;
        goto done;
    }
    for (int y = 0; y < dim; ++y) {
        const int sy = (int)((int64_t)y * (int64_t)image.height / dim);
        for (int x = 0; x < dim; ++x) {
            const int sx = (int)((int64_t)x * (int64_t)image.width / dim);
            const float green = image.pixels[((size_t)sy * image.width + sx) * 4u + 1u] / 255.0f;
            grid[(size_t)y * dim + x] = green >= threshold ? 1.0f : 0.0f;
        }
    }
    jce_image_free(&image);
    jce_trace_scope_end(grid_scope, JCE_TRACE_TASK_SUCCEEDED);
    *lease = (SrGrassMaskLease){grid, dim, hash, true};
    status = SR_GRASS_MASK_DECODED_UNCACHED;

    /* Only a completely prepared replacement retires the previous entry. */
    if (admit) {
        if (!entry) entry = (SrGrassMaskEntry *)JCE_MALLOC(sizeof *entry);
        else sr_grass_mask_entry_clear(entry);
        if (entry) {
            *entry = (SrGrassMaskEntry){
                .encoded = encoded, .encoded_size = size, .encoded_owner = owner,
                .fs = fs, .policy = policy, .threshold = threshold,
                .cells = grid, .dim = dim, .content_hash = hash
            };
            memcpy(entry->path, path, path_size);
            batch->entry = entry;
            encoded = NULL;
            lease->owned = false;
            status = SR_GRASS_MASK_DECODED;
        }
    }

done:
    if (encoded) sr_foliage_blob_free(encoded, owner);
    if (scope.id) {
#define MASK_COUNT(name_, value_) jce_trace_counter_scoped_internal( \
    JCE_TRACE_CATEGORY_RENDER, name_, (int64_t)(value_), scope.id, scope.id)
        MASK_COUNT("grass.mask.owner", (uintptr_t)sr);
        MASK_COUNT("grass.mask.fs", (uintptr_t)fs);
        MASK_COUNT("grass.mask.policy", policy);
        MASK_COUNT("grass.mask.fs_stable", stable);
        MASK_COUNT("grass.mask.encoded_bytes", size);
        MASK_COUNT("grass.mask.result", status);
        MASK_COUNT("grass.mask.dimension", lease->dim);
        MASK_COUNT("grass.mask.content_hash", lease->content_hash);
#undef MASK_COUNT
    }
    jce_trace_scope_end(scope, lease->cells ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return status;
}
