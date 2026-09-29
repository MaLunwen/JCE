/* Cooked foliage placement asset loading.
 *
 * This is deliberately separate from jce_sr_environment.c: authored placement
 * is resource I/O and validation, while that module owns GPU submission.  The
 * same path works for project-loose editor assets and embedded PAK content. */
#include "jce_sr_foliage_cooked.h"
#include "jce_sr_internal.h"

typedef enum {
    SR_FOLIAGE_BLOB_NONE = 0,
    SR_FOLIAGE_BLOB_FS,
    SR_FOLIAGE_BLOB_ALLOC
} SrFoliageBlobOwner;

static void sr_foliage_blob_free(void *data, SrFoliageBlobOwner owner)
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

static void *sr_foliage_read_asset(JceSceneRenderer *sr, const char *path,
                                   size_t max_size, size_t *out_size,
                                   SrFoliageBlobOwner *out_owner)
{
    *out_size = 0u;
    *out_owner = SR_FOLIAGE_BLOB_NONE;

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

uint32_t sr_foliage_param_hash(const JceVegetationScatterComponent *scatter,
                               const jce_vec3 *origin,
                               uint32_t surface_hash)
{
    uint32_t h = jce_fnv1a32_append(JCE_FNV1A32_INIT, &scatter->seed,
                                    sizeof scatter->seed);
    h = jce_fnv1a32_append(h, &scatter->density, sizeof scatter->density);
    h = jce_fnv1a32_append(h, &scatter->area_x, sizeof scatter->area_x);
    h = jce_fnv1a32_append(h, &scatter->area_z, sizeof scatter->area_z);
    h = jce_fnv1a32_append(h, &scatter->max_slope_deg,
                           sizeof scatter->max_slope_deg);
    h = jce_fnv1a32_append(h, &scatter->scale_min,
                           sizeof scatter->scale_min);
    h = jce_fnv1a32_append(h, &scatter->scale_max,
                           sizeof scatter->scale_max);
    h = jce_fnv1a32_append(h, &scatter->align_to_normal,
                           sizeof scatter->align_to_normal);
    h = jce_fnv1a32_append(h, origin, sizeof *origin);
    h = jce_fnv1a32_append(h, &surface_hash, sizeof surface_hash);
    h = jce_fnv1a32_append(h, scatter->baked_placement_path,
        (uint32_t)strlen(scatter->baked_placement_path));
    h = jce_fnv1a32_append(h, scatter->density_mask_path,
        (uint32_t)strlen(scatter->density_mask_path));
    h = jce_fnv1a32_append(h, &scatter->density_paint_active,
                           sizeof scatter->density_paint_active);
    if (scatter->density_paint_active) {
        h = jce_fnv1a32_append(h, scatter->density_paint,
                               (uint32_t)sizeof scatter->density_paint);
    }
    return h;
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
