/* Cooked foliage placement asset loading.
 *
 * This is deliberately separate from jce_sr_environment.c: authored placement
 * is resource I/O and validation, while that module owns GPU submission.  The
 * same path works for project-loose editor assets and embedded PAK content. */
#include "jce_sr_foliage_cooked.h"
#include "jce_sr_internal.h"
#include "os/core/jce_filesystem_read_internal.h"
#include <jce/os/core/jce_path.h>

void sr_foliage_blob_free(void *data, SrFoliageBlobOwner owner)
{
    if (owner == SR_FOLIAGE_BLOB_FS)
        jce_fs_buffer_free(data);
    else if (owner == SR_FOLIAGE_BLOB_ALLOC)
        JCE_FREE(data);
}

static void *sr_foliage_read_host_capped(const char *path, size_t max_size,
                                         size_t *out_size)
{
    uint64_t got = 0u;
    uint64_t total = 0u;
    void *data = jce_fs_host_read_capped(path, (uint64_t)max_size,
                                         &got, &total);
    if (!data || got != total || total > (uint64_t)max_size) {
        jce_fs_buffer_free(data);
        return NULL;
    }
    *out_size = (size_t)got;
    return data;
}

void *sr_foliage_read_asset(JceSceneRenderer *sr, const char *path,
                                   size_t max_size, size_t *out_size,
                                   SrFoliageBlobOwner *out_owner)
{
    *out_size = 0u;
    *out_owner = SR_FOLIAGE_BLOB_NONE;

    /* Relative component assets belong to the active content owner before
     * editor resolution. An isolated miss must not escape through a callback
     * to an absolute file in the previously open project or this renderer PAK.
     * The VFS checks the bound before allocation or PAK decompression. */
    JceFileSystem *active = jce_fs_get_active();
    if (active && !jce_path_is_absolute(path)) {
        uint64_t total = 0u;
        void *mounted = jce_fs_read_all_bounded_internal(
            active, path, (uint64_t)max_size, &total);
        if (mounted || total ||
            jce_fs_get_active_policy() == JCE_FS_ACTIVE_ISOLATED) {
            *out_size = (size_t)total;
            if (mounted) *out_owner = SR_FOLIAGE_BLOB_FS;
            return mounted;
        }
    }

    if (sr->has_cbs && sr->cbs.resolve_path) {
        char resolved[1024];
        if (sr->cbs.resolve_path(path, resolved, (int)sizeof resolved,
                                 sr->cbs.userdata)) {
            void *data = sr_foliage_read_host_capped(resolved, max_size,
                                                      out_size);
            if (data) {
                *out_owner = SR_FOLIAGE_BLOB_FS;
                return data;
            }
        }
    }

    if (sr->pak) {
        const JcePakAsset *asset = jce_pak_find(sr->pak, path);
        if (asset && asset->original_size <= (uint64_t)max_size) {
            const size_t size = (size_t)asset->original_size;
            void *data = JCE_MALLOC(size > 0u ? size : 1u);
            if (data) {
                const size_t got = jce_pak_decompress_ex(sr->pak, asset,
                                                         data, size);
                if (got == size) {
                    *out_size = size;
                    *out_owner = SR_FOLIAGE_BLOB_ALLOC;
                    return data;
                }
                JCE_FREE(data);
            }
        }
    }

    void *data = sr_foliage_read_host_capped(path, max_size, out_size);
    if (data) *out_owner = SR_FOLIAGE_BLOB_FS;
    return data;
}

void *sr_foliage_read_image_blob(JceSceneRenderer *sr, const char *path,
    size_t *out_size, SrFoliageBlobOwner *out_owner)
{
    if (!sr || !path || !path[0]) return NULL;
    if (jce_fs_get_active_policy() == JCE_FS_ACTIVE_ISOLATED &&
        (!jce_fs_get_active() || jce_path_is_absolute(path))) return NULL;
    return sr_foliage_read_asset(sr, path, 64u * 1024u * 1024u, out_size, out_owner);
}

static bool sr_foliage_load_cooked_asset(
    JceSceneRenderer *sr, const char *path, uint32_t expected_seed,
    JceFoliageInstance **inout_instances, uint32_t *inout_capacity,
    uint32_t *out_count)
{
    if (out_count) *out_count = 0u;
    if (!sr || !path || !path[0] || !inout_instances || !inout_capacity ||
        !out_count)
        return false;

    const size_t max_size = jce_foliage_cook_size(JCE_FOLIAGE_MAX_INSTANCES);
    size_t size = 0u;
    SrFoliageBlobOwner owner = SR_FOLIAGE_BLOB_NONE;
    void *data = sr_foliage_read_asset(sr, path, max_size, &size, &owner);
    if (!data) {
        LOG_WARN(LOG_TAG, "foliage cooked placement missing or oversized: '%s'",
                 path);
        return false;
    }

    if (!jce_foliage_cooked_validate(data, size)) {
        LOG_WARN(LOG_TAG, "foliage cooked placement failed validation: '%s'",
                 path);
        sr_foliage_blob_free(data, owner);
        return false;
    }
    if (jce_foliage_cooked_seed(data, size) != expected_seed) {
        LOG_WARN(LOG_TAG,
                 "foliage cooked placement seed mismatch: '%s' (file=%u scene=%u)",
                 path, jce_foliage_cooked_seed(data, size), expected_seed);
        sr_foliage_blob_free(data, owner);
        return false;
    }

    const uint32_t count = jce_foliage_cooked_count(data, size);
    if (count > *inout_capacity) {
        JceFoliageInstance *next = (JceFoliageInstance *)JCE_REALLOC(
            *inout_instances, (size_t)count * sizeof(JceFoliageInstance));
        if (!next) {
            sr_foliage_blob_free(data, owner);
            return false;
        }
        *inout_instances = next;
        *inout_capacity = count;
    }

    if (count > 0u) {
        const uint32_t loaded = jce_foliage_load_cooked(
            data, size, *inout_instances, *inout_capacity);
        if (loaded != count) {
            sr_foliage_blob_free(data, owner);
            return false;
        }
    }

    *out_count = count;
    sr_foliage_blob_free(data, owner);
    return true;
}

bool sr_foliage_try_load_cooked(JceSceneRenderer *sr,
                                const JceVegetationScatterComponent *scatter,
                                int slot)
{
    bool loaded;

    if (!sr || !scatter || slot < 0 || slot >= 16 ||
        !scatter->baked_placement_path[0])
        return false;

    loaded = sr_foliage_load_cooked_asset(
        sr, scatter->baked_placement_path, scatter->seed,
        &sr->foliage_cache[slot].insts,
        &sr->foliage_cache[slot].inst_cap,
        &sr->foliage_cache[slot].inst_count);
    if (!loaded) {
        LOG_WARN(LOG_TAG,
                 "foliage '%s': cooked placement unavailable; "
                 "falling back to deterministic scatter",
                 scatter->baked_placement_path);
    }
    return loaded;
}
