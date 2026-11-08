/*
 * jce_sr_terrain.c  Scene-renderer terrain module (split from
 * jce_scene_renderer.c).
 *
 * Terrain per-chunk LOD cache, surface collection, and per-chunk draw
 * (colour + shadow). Per-terrain projection is shared with the scene
 * through jce_terrain_surface_internal.h.
 */

#include "jce_terrain_cache.h"
#include "jce_sr_internal.h"
#include "jce_scene_internal.h"
#include <jce/renderer/jce_image.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/os/core/jce_hash.h>
#include "renderer/jce_render_encoder.h"
#include <stdlib.h>
#include "jce_sr_terrain_vertices.h"
#include "jce_sr_snow_mesh_cover.h"
#include "jce_sr_snow_limits.h"
#include "os/core/jce_trace_scope_internal.h"
#include <limits.h>

void sr_terrain_surfaces_collect(JceSceneRenderer *sr, JceScene *scene,
                                 EntityList *list, float reference_y,
                                 SrTerrainSurfaceSet *out)
{
    memset(out, 0, sizeof *out);
    out->reference_y = reference_y;
    out->hash = JCE_FNV1A32_INIT;

    JceTerrainCache *cache = jce_scene_terrain_cache(scene);
    for (int i = 0; i < list->count; ++i) {
        const JceEntity entity = list->entities[i];
        if (!jce_scene_has_terrain(scene, entity)) continue;
        JceTerrainComponent *tc = jce_scene_get_terrain(scene, entity);
        if (!tc || !tc->visible || !tc->terrain_path[0]) continue;

        const jce_mat4 world = jce_scene_get_world_matrix(scene, entity);
        const int slot = sr_terrain_find_or_load_slot(sr, scene,
                                                       tc->terrain_path);
        const uint64_t revision = cache
            ? jce_terrain_cache_revision(cache, tc->terrain_path) : 0u;
        out->hash = jce_fnv1a32_append(out->hash, &entity, sizeof entity);
        out->hash = jce_fnv1a32_append(out->hash, tc->terrain_path,
                                       (uint32_t)strlen(tc->terrain_path));
        out->hash = jce_fnv1a32_append(out->hash, &revision, sizeof revision);
        out->hash = jce_fnv1a32_append(out->hash, &world, sizeof world);

        if (out->count >= SR_TERRAIN_SURFACE_MAX) continue;
        if (slot < 0 || !sr->terrain_cache[slot].terrain) continue;
        if (jce_terrain_surface_init(&out->surfaces[out->count],
                            sr->terrain_cache[slot].terrain, &world))
            out->count++;
    }
    out->hash = jce_fnv1a32_append(out->hash, &out->count,
                                   sizeof out->count);
}

bool sr_terrain_surfaces_sample(void *user, float world_x,
                                float world_z, bool want_normal,
                                float *out_world_y,
                                float out_world_normal[3])
{
    SrTerrainSurfaceSet *set = (SrTerrainSurfaceSet *)user;
    if (!set || !out_world_y) return false;

    bool found = false;
    float best_distance = 1e30f;
    float best_y = 0.0f;
    jce_vec3 best_normal = jce_v3(0.0f, 1.0f, 0.0f);
    for (int i = 0; i < set->count; ++i) {
        float y;
        jce_vec3 normal = jce_v3(0.0f, 1.0f, 0.0f);
        if (!jce_terrain_surface_sample(&set->surfaces[i], world_x, world_z,
                                   want_normal, &y, &normal))
            continue;
        const float distance = fabsf(y - set->reference_y);
        if (!found || distance < best_distance) {
            found = true;
            best_distance = distance;
            best_y = y;
            best_normal = normal;
        }
    }
    if (!found) return false;
    *out_world_y = best_y;
    if (want_normal) {
        out_world_normal[0] = best_normal.x;
        out_world_normal[1] = best_normal.y;
        out_world_normal[2] = best_normal.z;
    }
    return true;
}


enum {
    SR_TERRAIN_MATERIAL_CACHE_MAX = 8,
    SR_TERRAIN_MATERIAL_MAX_INFLIGHT = 2,
    SR_TERRAIN_MATERIAL_LAYERS = 8,
    SR_TERRAIN_MATERIAL_DIM = 256
};

#define SR_TERRAIN_SOURCE_MAX_BYTES (UINT64_C(128) * 1024u * 1024u)

typedef struct SrTerrainMaterialJob {
    const JcePakArchive *pak;
    char                 path[SR_TERRAIN_MATERIAL_LAYERS][1024];
    uint8_t              authored_mask;
    uint8_t              host_mask;
    uint8_t              loaded_mask;
    uint8_t             *pixels;
    uint32_t             layer_bytes;
    uint8_t              mip_count;
} SrTerrainMaterialJob;

typedef struct SrTerrainMaterialEntry {
    char                  source[SR_TERRAIN_MATERIAL_LAYERS][256];
    uint32_t              key_hash;
    uint8_t               loaded_mask;
    bool                  used;
    bool                  pending;
    bool                  failed;
    uint64_t              last_used_frame;
    bgfx_texture_handle_t texture;
    JceAsyncTask         *task;
    SrTerrainMaterialJob *job;
} SrTerrainMaterialEntry;

enum {
    SR_SNOW_STAMP_MAX = 8,
    SR_SNOW_COVER_MAX = 8,
    SR_SNOW_COVER_VECS = 3,
    SR_SNOW_COUNT_RADIX = 16,
    SR_SNOW_MARKER_SCAN_MAX = 64,
};

typedef struct {
    float stamps[SR_SNOW_STAMP_MAX][4];
    float covers[SR_SNOW_COVER_MAX][SR_SNOW_COVER_VECS][4];
    uint16_t stamp_count;
    uint16_t cover_count;
} SrTerrainSnowMarkers;

struct SrTerrainMaterialCache {
    SrTerrainMaterialEntry entries[SR_TERRAIN_MATERIAL_CACHE_MAX];
    int                     inflight;
    bool                    supported;
    bgfx_texture_handle_t   fallback;
    bgfx_uniform_handle_t   sampler;
    bgfx_uniform_handle_t   normal_scales;
    bgfx_uniform_handle_t   mask_flags;
    bgfx_uniform_handle_t   snow_stamps;
    bgfx_uniform_handle_t   snow_cover;
    bgfx_uniform_handle_t   snow_geometry;
    bgfx_uniform_handle_t   snow_look;
    bgfx_program_handle_t   snow_depth;
    bool                    snow_depth_attempted;
    bool                    snow_geometry_enabled;
};

typedef struct SrTerrainMaterialBinding {
    bgfx_texture_handle_t texture;
    float                 normal_scales[4];
    float                 mask_flags[4];
    bool                  valid;
} SrTerrainMaterialBinding;

static float sr_terrain_clampf(float value, float lo, float hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static uint32_t sr_terrain_layer_bytes(uint32_t dim, uint8_t *out_mips)
{
    uint32_t bytes = 0;
    uint8_t mips = 0;
    for (;;) {
        bytes += dim * dim * 4u;
        ++mips;
        if (dim == 1u) break;
        dim >>= 1u;
    }
    if (out_mips) *out_mips = mips;
    return bytes;
}

static uint8_t sr_terrain_float_to_unorm(float value)
{
    value = sr_terrain_clampf(value, 0.0f, 1.0f);
    return (uint8_t)(value * 255.0f + 0.5f);
}

static void sr_terrain_decode_normal(const float rgba[4], float out[3])
{
    float x = rgba[0] * (2.0f / 255.0f) - 1.0f;
    float y = rgba[1] * (2.0f / 255.0f) - 1.0f;
    float z = rgba[2] * (2.0f / 255.0f) - 1.0f;
    float len2 = x * x + y * y + z * z;
    if (len2 < 1e-10f) {
        out[0] = 0.0f; out[1] = 0.0f; out[2] = 1.0f;
        return;
    }
    float inv_len = 1.0f / sqrtf(len2);
    out[0] = x * inv_len;
    out[1] = y * inv_len;
    out[2] = z * inv_len;
}

static void sr_terrain_encode_normal(const float normal[3], uint8_t out[4])
{
    out[0] = sr_terrain_float_to_unorm(normal[0] * 0.5f + 0.5f);
    out[1] = sr_terrain_float_to_unorm(normal[1] * 0.5f + 0.5f);
    out[2] = sr_terrain_float_to_unorm(normal[2] * 0.5f + 0.5f);
    out[3] = 255u;
}

static void sr_terrain_sample_bilinear(const uint8_t *src,
                                        uint32_t width, uint32_t height,
                                        float x, float y, float out[4])
{
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    float tx = x - (float)x0;
    float ty = y - (float)y0;
    if (x0 < 0) { x0 = 0; tx = 0.0f; }
    if (y0 < 0) { y0 = 0; ty = 0.0f; }
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    if (x1 >= (int)width) x1 = (int)width - 1;
    if (y1 >= (int)height) y1 = (int)height - 1;
    if (x0 >= (int)width) x0 = (int)width - 1;
    if (y0 >= (int)height) y0 = (int)height - 1;

    const uint8_t *p00 = src + ((size_t)y0 * width + (uint32_t)x0) * 4u;
    const uint8_t *p10 = src + ((size_t)y0 * width + (uint32_t)x1) * 4u;
    const uint8_t *p01 = src + ((size_t)y1 * width + (uint32_t)x0) * 4u;
    const uint8_t *p11 = src + ((size_t)y1 * width + (uint32_t)x1) * 4u;
    for (int c = 0; c < 4; ++c) {
        float top = (float)p00[c] + ((float)p10[c] - (float)p00[c]) * tx;
        float bot = (float)p01[c] + ((float)p11[c] - (float)p01[c]) * tx;
        out[c] = top + (bot - top) * ty;
    }
}

static void sr_terrain_resample_layer(const uint8_t *src,
                                       uint32_t width, uint32_t height,
                                       uint8_t *dst, bool normal_map)
{
    const float sx = (float)width / (float)SR_TERRAIN_MATERIAL_DIM;
    const float sy = (float)height / (float)SR_TERRAIN_MATERIAL_DIM;
    for (uint32_t y = 0; y < SR_TERRAIN_MATERIAL_DIM; ++y) {
        for (uint32_t x = 0; x < SR_TERRAIN_MATERIAL_DIM; ++x) {
            float sample[4];
            sr_terrain_sample_bilinear(src, width, height,
                ((float)x + 0.5f) * sx - 0.5f,
                ((float)y + 0.5f) * sy - 0.5f, sample);
            uint8_t *pixel = dst + ((size_t)y * SR_TERRAIN_MATERIAL_DIM + x) * 4u;
            if (normal_map) {
                float normal[3];
                sr_terrain_decode_normal(sample, normal);
                sr_terrain_encode_normal(normal, pixel);
            } else {
                for (int c = 0; c < 4; ++c)
                    pixel[c] = sr_terrain_float_to_unorm(sample[c] / 255.0f);
            }
        }
    }
}

static void sr_terrain_build_layer_mips(uint8_t *chain, bool normal_map)
{
    uint32_t prev_dim = SR_TERRAIN_MATERIAL_DIM;
    uint8_t *prev = chain;
    uint8_t *dst = prev + (size_t)prev_dim * prev_dim * 4u;
    while (prev_dim > 1u) {
        uint32_t dim = prev_dim >> 1u;
        for (uint32_t y = 0; y < dim; ++y) {
            for (uint32_t x = 0; x < dim; ++x) {
                uint8_t *out = dst + ((size_t)y * dim + x) * 4u;
                const uint8_t *sample[4] = {
                    prev + ((size_t)(y * 2u) * prev_dim + x * 2u) * 4u,
                    prev + ((size_t)(y * 2u) * prev_dim + x * 2u + 1u) * 4u,
                    prev + ((size_t)(y * 2u + 1u) * prev_dim + x * 2u) * 4u,
                    prev + ((size_t)(y * 2u + 1u) * prev_dim + x * 2u + 1u) * 4u
                };
                if (normal_map) {
                    float sum[3] = { 0.0f, 0.0f, 0.0f };
                    for (int s = 0; s < 4; ++s) {
                        float rgba[4] = { sample[s][0], sample[s][1],
                                          sample[s][2], sample[s][3] };
                        float normal[3];
                        sr_terrain_decode_normal(rgba, normal);
                        sum[0] += normal[0];
                        sum[1] += normal[1];
                        sum[2] += normal[2];
                    }
                    float len2 = sum[0] * sum[0] + sum[1] * sum[1] +
                                 sum[2] * sum[2];
                    if (len2 < 1e-10f) {
                        sum[0] = 0.0f; sum[1] = 0.0f; sum[2] = 1.0f;
                    } else {
                        float inv_len = 1.0f / sqrtf(len2);
                        sum[0] *= inv_len; sum[1] *= inv_len; sum[2] *= inv_len;
                    }
                    sr_terrain_encode_normal(sum, out);
                } else {
                    for (int c = 0; c < 4; ++c) {
                        uint32_t value = (uint32_t)sample[0][c] + sample[1][c] +
                                         sample[2][c] + sample[3][c];
                        out[c] = (uint8_t)((value + 2u) >> 2u);
                    }
                }
            }
        }
        prev = dst;
        prev_dim = dim;
        dst += (size_t)dim * dim * 4u;
    }
}

static void sr_terrain_fill_default_chain(uint8_t *chain, uint32_t bytes,
                                           bool normal_map)
{
    const uint8_t value[4] = {
        normal_map ? 128u : 0u,
        normal_map ? 128u : 255u,
        normal_map ? 255u : 128u,
        normal_map ? 255u : 0u
    };
    for (uint32_t i = 0; i < bytes; i += 4u)
        memcpy(chain + i, value, sizeof value);
}

static void *sr_terrain_read_source(const SrTerrainMaterialJob *job, int layer,
                                    uint64_t *out_size, bool *out_fs_buffer)
{
    *out_size = 0;
    *out_fs_buffer = false;
    if (!(job->host_mask & (uint8_t)(1u << layer)) && job->pak) {
        const JcePakAsset *asset = jce_pak_find(job->pak, job->path[layer]);
        if (asset && asset->original_size > 0 &&
            asset->original_size <= SR_TERRAIN_SOURCE_MAX_BYTES &&
            asset->original_size <= (uint64_t)SIZE_MAX) {
            void *data = JCE_MALLOC((size_t)asset->original_size);
            if (data && jce_pak_decompress(asset, data,
                                           (size_t)asset->original_size) ==
                        (size_t)asset->original_size) {
                *out_size = asset->original_size;
                return data;
            }
            JCE_FREE(data);
        }
    }

    uint64_t read = 0, total = 0;
    void *data = jce_fs_host_read_capped(job->path[layer],
        SR_TERRAIN_SOURCE_MAX_BYTES, &read, &total);
    if (!data || read == 0 || read != total) {
        jce_fs_buffer_free(data);
        return NULL;
    }
    *out_size = read;
    *out_fs_buffer = true;
    return data;
}

static JceAsyncRunResult sr_terrain_material_worker(JceAsyncContext *ctx,
                                                     void *arg)
{
    SrTerrainMaterialJob *job = (SrTerrainMaterialJob *)arg;
    job->layer_bytes = sr_terrain_layer_bytes(SR_TERRAIN_MATERIAL_DIM,
                                               &job->mip_count);
    size_t total = (size_t)job->layer_bytes * SR_TERRAIN_MATERIAL_LAYERS;
    job->pixels = (uint8_t *)JCE_MALLOC(total);
    if (!job->pixels) {
        jce_async_context_fail(ctx, -1, "terrain layer bake allocation failed");
        return JCE_ASYNC_RUN_FAILED;
    }

    for (int layer = 0; layer < SR_TERRAIN_MATERIAL_LAYERS; ++layer) {
        uint8_t *chain = job->pixels + (size_t)layer * job->layer_bytes;
        sr_terrain_fill_default_chain(chain, job->layer_bytes, layer < 4);
        if (!(job->authored_mask & (uint8_t)(1u << layer))) {
            jce_async_context_set_progress(ctx,
                (float)(layer + 1) / (float)SR_TERRAIN_MATERIAL_LAYERS);
            continue;
        }
        if (jce_async_context_cancel_requested(ctx))
            return JCE_ASYNC_RUN_CANCELLED;

        uint64_t encoded_size = 0;
        bool fs_buffer = false;
        void *encoded = sr_terrain_read_source(job, layer, &encoded_size,
                                                &fs_buffer);
        uint8_t *rgba = NULL;
        uint32_t width = 0, height = 0;
        bool cooked_pixels = false;
        if (encoded) {
            cooked_pixels = jce_texture_decode_cooked_rgba8(
                encoded, (size_t)encoded_size, &rgba, &width, &height);
            if (!cooked_pixels) {
                int iw = 0, ih = 0;
                rgba = jce_image_load_rgba8_from_memory(encoded, encoded_size,
                                                         &iw, &ih);
                if (iw > 0 && ih > 0) {
                    width = (uint32_t)iw;
                    height = (uint32_t)ih;
                }
            }
        }
        if (fs_buffer) jce_fs_buffer_free(encoded);
        else JCE_FREE(encoded);

        if (rgba && width > 0 && height > 0) {
            sr_terrain_resample_layer(rgba, width, height, chain, layer < 4);
            sr_terrain_build_layer_mips(chain, layer < 4);
            job->loaded_mask |= (uint8_t)(1u << layer);
        } else {
            LOG_WARN(LOG_TAG, "terrain layer texture failed: '%s'",
                     job->path[layer]);
        }
        if (cooked_pixels) JCE_FREE(rgba);
        else jce_image_free_rgba8(rgba);
        jce_async_context_set_progress(ctx,
            (float)(layer + 1) / (float)SR_TERRAIN_MATERIAL_LAYERS);
    }
    return JCE_ASYNC_RUN_SUCCESS;
}

static void sr_terrain_material_job_destroy(SrTerrainMaterialJob *job)
{
    if (!job) return;
    JCE_FREE(job->pixels);
    JCE_FREE(job);
}

static bgfx_texture_handle_t sr_terrain_material_upload(
    const SrTerrainMaterialJob *job)
{
    bgfx_texture_handle_t invalid = { UINT16_MAX };
    bgfx_texture_handle_t texture = bgfx_create_texture_2d(
        SR_TERRAIN_MATERIAL_DIM, SR_TERRAIN_MATERIAL_DIM, true,
        SR_TERRAIN_MATERIAL_LAYERS, BGFX_TEXTURE_FORMAT_RGBA8, 0, NULL, 0);
    if (!BGFX_HANDLE_IS_VALID(texture)) return invalid;

    for (uint16_t layer = 0; layer < SR_TERRAIN_MATERIAL_LAYERS; ++layer) {
        const uint8_t *src = job->pixels + (size_t)layer * job->layer_bytes;
        uint16_t dim = SR_TERRAIN_MATERIAL_DIM;
        for (uint8_t mip = 0; mip < job->mip_count; ++mip) {
            uint32_t bytes = (uint32_t)dim * dim * 4u;
            const bgfx_memory_t *mem = bgfx_copy(src, bytes);
            if (!mem) {
                bgfx_destroy_texture(texture);
                return invalid;
            }
            bgfx_update_texture_2d(texture, layer, mip, 0, 0, dim, dim,
                                   mem, UINT16_MAX);
            src += bytes;
            if (dim > 1u) dim >>= 1u;
        }
    }
    return texture;
}

static struct SrTerrainMaterialCache *sr_terrain_material_cache_get(
    JceSceneRenderer *sr)
{
    if (sr->terrain_material_cache) return sr->terrain_material_cache;
    struct SrTerrainMaterialCache *cache =
        (struct SrTerrainMaterialCache *)JCE_MALLOC(sizeof(*cache));
    if (!cache) return NULL;
    memset(cache, 0, sizeof(*cache));
    cache->fallback.idx = UINT16_MAX;
    cache->sampler.idx = UINT16_MAX;
    cache->normal_scales.idx = UINT16_MAX;
    cache->mask_flags.idx = UINT16_MAX;
    cache->snow_stamps = bgfx_create_uniform("u_terrainSnowStamps",
        BGFX_UNIFORM_TYPE_VEC4, SR_SNOW_STAMP_MAX);
    cache->snow_cover = bgfx_create_uniform("u_terrainSnowCover",
        BGFX_UNIFORM_TYPE_VEC4, SR_SNOW_COVER_MAX * SR_SNOW_COVER_VECS);
    cache->snow_geometry = bgfx_create_uniform("u_terrainSnowGeometry",
        BGFX_UNIFORM_TYPE_VEC4, 1);
    cache->snow_look = bgfx_create_uniform("u_terrainSnowLook", BGFX_UNIFORM_TYPE_VEC4, 1);
    cache->snow_depth.idx = UINT16_MAX;
    const char *snow_setting = getenv("JCE_TERRAIN_SNOW_GEOMETRY");
    cache->snow_geometry_enabled = !snow_setting || strcmp(snow_setting, "0") != 0;
    for (int i = 0; i < SR_TERRAIN_MATERIAL_CACHE_MAX; ++i)
        cache->entries[i].texture.idx = UINT16_MAX;

    const bgfx_caps_t *caps = bgfx_get_caps();
    if (caps && (caps->supported & BGFX_CAPS_TEXTURE_2D_ARRAY)) {
        uint8_t defaults[SR_TERRAIN_MATERIAL_LAYERS * 4u];
        for (int layer = 0; layer < SR_TERRAIN_MATERIAL_LAYERS; ++layer) {
            uint8_t *pixel = defaults + layer * 4;
            pixel[0] = layer < 4 ? 128u : 0u;
            pixel[1] = layer < 4 ? 128u : 255u;
            pixel[2] = layer < 4 ? 255u : 128u;
            pixel[3] = layer < 4 ? 255u : 0u;
        }
        cache->fallback = bgfx_create_texture_2d(1, 1, false,
            SR_TERRAIN_MATERIAL_LAYERS, BGFX_TEXTURE_FORMAT_RGBA8, 0,
            bgfx_copy(defaults, sizeof defaults), 0);
        cache->sampler = bgfx_create_uniform("s_terrainLayerData",
            BGFX_UNIFORM_TYPE_SAMPLER, 1);
        cache->normal_scales = bgfx_create_uniform("u_terrainNormalScales",
            BGFX_UNIFORM_TYPE_VEC4, 1);
        cache->mask_flags = bgfx_create_uniform("u_terrainMaskFlags",
            BGFX_UNIFORM_TYPE_VEC4, 1);
        cache->supported = BGFX_HANDLE_IS_VALID(cache->fallback) &&
                           BGFX_HANDLE_IS_VALID(cache->sampler) &&
                           BGFX_HANDLE_IS_VALID(cache->normal_scales) &&
                           BGFX_HANDLE_IS_VALID(cache->mask_flags);
    }
    if (!cache->supported)
        LOG_WARN(LOG_TAG, "terrain layer arrays unsupported; using uniform material");
    sr->terrain_material_cache = cache;
    return cache;
}

static void sr_terrain_material_poll(struct SrTerrainMaterialCache *cache)
{
    if (!cache || cache->inflight == 0) return;
    for (int i = 0; i < SR_TERRAIN_MATERIAL_CACHE_MAX; ++i) {
        SrTerrainMaterialEntry *entry = &cache->entries[i];
        if (!entry->pending || !entry->task ||
            !jce_async_task_is_terminal(entry->task)) continue;

        SrTerrainMaterialJob *job = entry->job;
        bgfx_texture_handle_t texture = { UINT16_MAX };
        if (jce_async_task_state(entry->task) == JCE_ASYNC_STATE_SUCCEEDED &&
            job && job->pixels && job->loaded_mask)
            texture = sr_terrain_material_upload(job);

        entry->texture = texture;
        entry->loaded_mask = job ? job->loaded_mask : 0u;
        entry->failed = !BGFX_HANDLE_IS_VALID(texture);
        entry->pending = false;
        entry->job = NULL;
        jce_async_task_release(entry->task);
        entry->task = NULL;
        sr_terrain_material_job_destroy(job);
        if (cache->inflight > 0) --cache->inflight;
    }
}

static uint32_t sr_terrain_material_key(const JceTerrainComponent *tc,
    char paths[SR_TERRAIN_MATERIAL_LAYERS][256], uint8_t *out_authored)
{
    uint32_t hash = JCE_FNV1A32_INIT;
    uint8_t authored = 0;
    memset(paths, 0, SR_TERRAIN_MATERIAL_LAYERS * 256u);
    if (tc) {
        for (int i = 0; i < 4; ++i) {
            jce_strlcpy(paths[i], tc->layer_normal_path[i], sizeof paths[i]);
            jce_strlcpy(paths[i + 4], tc->layer_mask_path[i],
                        sizeof paths[i + 4]);
        }
    }
    for (int i = 0; i < SR_TERRAIN_MATERIAL_LAYERS; ++i) {
        size_t len = strlen(paths[i]);
        if (len) authored |= (uint8_t)(1u << i);
        hash = jce_fnv1a32_append(hash, paths[i], len + 1u);
    }
    *out_authored = authored;
    return hash;
}

static SrTerrainMaterialBinding sr_terrain_material_get(
    JceSceneRenderer *sr, const JceTerrainComponent *tc)
{
    SrTerrainMaterialBinding binding;
    memset(&binding, 0, sizeof binding);
    binding.texture.idx = UINT16_MAX;
    struct SrTerrainMaterialCache *cache = sr_terrain_material_cache_get(sr);
    if (!cache || !cache->supported) return binding;
    sr_terrain_material_poll(cache);
    binding.texture = cache->fallback;
    binding.valid = true;

    char paths[SR_TERRAIN_MATERIAL_LAYERS][256];
    uint8_t authored = 0;
    uint32_t key = sr_terrain_material_key(tc, paths, &authored);
    if (!authored) return binding;

    int free_slot = -1;
    int lru_slot = -1;
    uint64_t lru_frame = UINT64_MAX;
    for (int i = 0; i < SR_TERRAIN_MATERIAL_CACHE_MAX; ++i) {
        SrTerrainMaterialEntry *entry = &cache->entries[i];
        if (!entry->used) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (entry->key_hash == key &&
            memcmp(entry->source, paths, sizeof paths) == 0) {
            entry->last_used_frame = sr->model_frame;
            if (!entry->pending && !entry->failed &&
                BGFX_HANDLE_IS_VALID(entry->texture)) {
                binding.texture = entry->texture;
                for (int layer = 0; layer < 4; ++layer) {
                    if (entry->loaded_mask & (uint8_t)(1u << layer))
                        binding.normal_scales[layer] = sr_terrain_clampf(
                            tc->layer_normal_scale[layer], 0.0f, 8.0f);
                    if (entry->loaded_mask & (uint8_t)(1u << (layer + 4)))
                        binding.mask_flags[layer] = 1.0f;
                }
            }
            return binding;
        }
        if (!entry->pending && entry->last_used_frame < lru_frame) {
            lru_frame = entry->last_used_frame;
            lru_slot = i;
        }
    }

    if (cache->inflight >= SR_TERRAIN_MATERIAL_MAX_INFLIGHT)
        return binding;
    int slot = free_slot >= 0 ? free_slot : lru_slot;
    if (slot < 0) return binding;
    SrTerrainMaterialEntry *entry = &cache->entries[slot];

    SrTerrainMaterialJob *job =
        (SrTerrainMaterialJob *)JCE_MALLOC(sizeof(*job));
    if (!job) return binding;
    memset(job, 0, sizeof(*job));
    job->pak = sr->pak;
    job->authored_mask = authored;
    for (int i = 0; i < SR_TERRAIN_MATERIAL_LAYERS; ++i) {
        if (!(authored & (uint8_t)(1u << i))) continue;
        char resolved[1024];
        if (sr->has_cbs && sr->cbs.resolve_path &&
            sr->cbs.resolve_path(paths[i], resolved, (int)sizeof resolved,
                                 sr->cbs.userdata)) {
            jce_strlcpy(job->path[i], resolved, sizeof job->path[i]);
            job->host_mask |= (uint8_t)(1u << i);
        } else {
            jce_strlcpy(job->path[i], paths[i], sizeof job->path[i]);
        }
    }

    JceAsyncTaskDesc desc;
    jce_async_task_desc_init(&desc);
    desc.work = sr_terrain_material_worker;
    desc.user_data = job;
    desc.debug_name = "scene.terrain-material.bake";
    desc.priority = JCE_ASYNC_PRIORITY_HIGH;
    JceAsyncTask *task = jce_async_submit(jce_async_default_executor(), &desc);
    if (!task) {
        sr_terrain_material_job_destroy(job);
        return binding;
    }

    /* Keep the previous LRU entry alive until the replacement task owns a
     * scheduler reference.  Allocation or submission pressure must degrade to
     * the fallback texture, not evict a material that is still renderable. */
    if (entry->used && BGFX_HANDLE_IS_VALID(entry->texture))
        bgfx_destroy_texture(entry->texture);
    memset(entry, 0, sizeof(*entry));
    entry->texture.idx = UINT16_MAX;
    memcpy(entry->source, paths, sizeof paths);
    entry->key_hash = key;
    entry->used = true;
    entry->pending = true;
    entry->last_used_frame = sr->model_frame;
    entry->task = task;
    entry->job = job;
    ++cache->inflight;
    return binding;
}

static void sr_terrain_material_bind(JceSceneRenderer *sr,
                                      const SrTerrainMaterialBinding *binding)
{
    struct SrTerrainMaterialCache *cache = sr->terrain_material_cache;
    if (!cache || !cache->supported || !binding->valid) return;
    bgfx_set_texture(2, cache->sampler, binding->texture, UINT32_MAX);
    bgfx_set_uniform(cache->normal_scales, binding->normal_scales, 1);
    bgfx_set_uniform(cache->mask_flags, binding->mask_flags, 1);
}

void sr_terrain_material_cache_destroy(JceSceneRenderer *sr)
{
    if (!sr || !sr->terrain_material_cache) return;
    struct SrTerrainMaterialCache *cache = sr->terrain_material_cache;
    for (int i = 0; i < SR_TERRAIN_MATERIAL_CACHE_MAX; ++i) {
        SrTerrainMaterialEntry *entry = &cache->entries[i];
        if (entry->pending && entry->task) {
            (void)jce_async_task_discard(entry->task);
        }
        sr_terrain_material_job_destroy(entry->job);
        if (BGFX_HANDLE_IS_VALID(entry->texture))
            bgfx_destroy_texture(entry->texture);
    }
    if (BGFX_HANDLE_IS_VALID(cache->fallback))
        bgfx_destroy_texture(cache->fallback);
    if (BGFX_HANDLE_IS_VALID(cache->sampler))
        bgfx_destroy_uniform(cache->sampler);
    if (BGFX_HANDLE_IS_VALID(cache->normal_scales))
        bgfx_destroy_uniform(cache->normal_scales);
    if (BGFX_HANDLE_IS_VALID(cache->mask_flags))
        bgfx_destroy_uniform(cache->mask_flags);
    if (BGFX_HANDLE_IS_VALID(cache->snow_stamps))
        bgfx_destroy_uniform(cache->snow_stamps);
    if (BGFX_HANDLE_IS_VALID(cache->snow_cover))
        bgfx_destroy_uniform(cache->snow_cover);
    if (BGFX_HANDLE_IS_VALID(cache->snow_geometry))
        bgfx_destroy_uniform(cache->snow_geometry);
    if (BGFX_HANDLE_IS_VALID(cache->snow_look))
        bgfx_destroy_uniform(cache->snow_look);
    if (BGFX_HANDLE_IS_VALID(cache->snow_depth))
        bgfx_destroy_program(cache->snow_depth);
    JCE_FREE(cache);
    sr->terrain_material_cache = NULL;
}

/* ── Terrain per-chunk LOD cache (P1-terrain-lod) ─────────────────────
 *
 * sr_terrain_get_slot() find-or-loads a terrain by path and lazily fills the
 * per-chunk metadata (chunk count + each chunk's terrain-LOCAL AABB).  The
 * actual GPU chunk meshes are built on demand by sr_terrain_chunk_mesh() at
 * whatever LOD the camera-distance test asks for, and cached until the
 * required LOD changes (or the terrain is invalidated).  Nothing here merges
 * chunks: the renderer issues one draw per visible chunk. */

/* Compute chunk (cx,cz)'s terrain-local AABB by scanning its height samples.
 * Cheap (runs once per chunk at load) and gives a snug Y range for culling. */
static void sr_terrain_chunk_local_aabb(const JceTerrain *t, int cx, int cz,
                                        jce_vec3 *out_min, jce_vec3 *out_max)
{
    int w  = jce_terrain_width(t);
    int h  = jce_terrain_height(t);
    int cs = jce_terrain_chunk_size(t);
    float wsx = jce_terrain_world_size_x(t);
    float wsz = jce_terrain_world_size_z(t);
    float mh  = jce_terrain_max_height(t);
    const float *heights = jce_terrain_heights(t);

    int x0 = cx * cs, z0 = cz * cs;
    int x1 = x0 + cs, z1 = z0 + cs;
    if (x1 > w - 1) x1 = w - 1;
    if (z1 > h - 1) z1 = h - 1;

    float dx = (w > 1) ? wsx / (float)(w - 1) : 0.0f;
    float dz = (h > 1) ? wsz / (float)(h - 1) : 0.0f;

    float hmin = +FLT_MAX, hmax = -FLT_MAX;
    if (heights) {
        for (int zz = z0; zz <= z1; zz++)
        for (int xx = x0; xx <= x1; xx++) {
            float hv = heights[(size_t)zz * (size_t)w + (size_t)xx] * mh;
            if (hv < hmin) hmin = hv;
            if (hv > hmax) hmax = hv;
        }
    }
    if (hmin > hmax) { hmin = 0.0f; hmax = mh; }

    out_min->x = (float)x0 * dx; out_max->x = (float)x1 * dx;
    out_min->z = (float)z0 * dz; out_max->z = (float)z1 * dz;
    out_min->y = hmin;           out_max->y = hmax;
}

/* Free all per-chunk cache arrays + GPU meshes for one terrain slot. */
void sr_terrain_free_chunks(JceSceneRenderer *sr, int slot)
{
    if (slot < 0 || slot >= 16) return;
    if (sr->terrain_cache[slot].chunk_meshes) {
        for (int i = 0; i < sr->terrain_cache[slot].chunk_count; i++)
            if (sr->terrain_cache[slot].chunk_meshes[i])
                jce_mesh_destroy(sr->terrain_cache[slot].chunk_meshes[i]);
        JCE_FREE(sr->terrain_cache[slot].chunk_meshes);
        sr->terrain_cache[slot].chunk_meshes = NULL;
    }
    if (sr->terrain_cache[slot].chunk_lod) {
        JCE_FREE(sr->terrain_cache[slot].chunk_lod);
        sr->terrain_cache[slot].chunk_lod = NULL;
    }
    if (sr->terrain_cache[slot].chunk_min) {
        JCE_FREE(sr->terrain_cache[slot].chunk_min);
        sr->terrain_cache[slot].chunk_min = NULL;
    }
    if (sr->terrain_cache[slot].chunk_max) {
        JCE_FREE(sr->terrain_cache[slot].chunk_max);
        sr->terrain_cache[slot].chunk_max = NULL;
    }
    if (sr->terrain_cache[slot].chunk_splat_tex) {
        for (int i = 0; i < sr->terrain_cache[slot].chunk_count; i++)
            if (BGFX_HANDLE_IS_VALID(sr->terrain_cache[slot].chunk_splat_tex[i]))
                bgfx_destroy_texture(sr->terrain_cache[slot].chunk_splat_tex[i]);
        JCE_FREE(sr->terrain_cache[slot].chunk_splat_tex);
        sr->terrain_cache[slot].chunk_splat_tex = NULL;
    }
    sr->terrain_cache[slot].chunk_count = 0;
}

/* Allocate + fill the per-chunk metadata for a freshly loaded terrain. */
static bool sr_terrain_init_chunks(JceSceneRenderer *sr, int slot)
{
    JceTerrain *terr = sr->terrain_cache[slot].terrain;
    if (!terr) return false;
    int ncx = jce_terrain_chunk_count_x(terr);
    int ncz = jce_terrain_chunk_count_z(terr);
    int n   = ncx * ncz;
    if (n <= 0 || n > SR_TERRAIN_MAX_CHUNKS) return false;

    JceMesh  **meshes = (JceMesh **)JCE_MALLOC(sizeof(JceMesh *) * (size_t)n);
    int8_t    *lods   = (int8_t  *)JCE_MALLOC(sizeof(int8_t)    * (size_t)n);
    jce_vec3  *mins   = (jce_vec3 *)JCE_MALLOC(sizeof(jce_vec3) * (size_t)n);
    jce_vec3  *maxs   = (jce_vec3 *)JCE_MALLOC(sizeof(jce_vec3) * (size_t)n);
    /* Per-chunk splat textures only for a tiled terrain (else NULL → the
     * monolithic splat_tex path is used). */
    bgfx_texture_handle_t *splats = jce_terrain_is_tiled(terr)
        ? (bgfx_texture_handle_t *)JCE_MALLOC(sizeof(bgfx_texture_handle_t) * (size_t)n)
        : NULL;
    if (!meshes || !lods || !mins || !maxs ||
        (jce_terrain_is_tiled(terr) && !splats)) {
        if (meshes) JCE_FREE(meshes);
        if (lods)   JCE_FREE(lods);
        if (mins)   JCE_FREE(mins);
        if (maxs)   JCE_FREE(maxs);
        if (splats) JCE_FREE(splats);
        return false;
    }
    for (int cz = 0; cz < ncz; cz++)
    for (int cx = 0; cx < ncx; cx++) {
        int idx = cz * ncx + cx;
        meshes[idx] = NULL;
        lods[idx]   = -1;
        if (splats) splats[idx].idx = UINT16_MAX;
        sr_terrain_chunk_local_aabb(terr, cx, cz, &mins[idx], &maxs[idx]);
    }
    sr->terrain_cache[slot].chunk_meshes    = meshes;
    sr->terrain_cache[slot].chunk_lod       = lods;
    sr->terrain_cache[slot].chunk_min       = mins;
    sr->terrain_cache[slot].chunk_max       = maxs;
    sr->terrain_cache[slot].chunk_splat_tex = splats;
    sr->terrain_cache[slot].chunk_count  = n;
    sr->terrain_cache[slot].chunk_nx     = ncx;
    sr->terrain_cache[slot].chunk_nz     = ncz;
    return true;
}

/* Build (or rebuild) chunk `idx`'s GPU mesh at `lod`, with a downward skirt
 * around its outer ring to hide T-junction cracks against neighbouring chunks
 * drawn at a different LOD.  Returns the cached mesh, or NULL on failure.
 * No-op (returns the cached mesh) when the chunk is already built at `lod`. */
/* JCE owns the conversion allocation; test ports only inject allocator faults. */
static void *sr_terrain_vertex_allocate(size_t bytes, void *user)
{
    (void)user;
    return JCE_MALLOC(bytes);
}
static void sr_terrain_vertex_release(void *ptr, void *user)
{
    (void)user;
    JCE_FREE(ptr);
}
static const SrTerrainVertexAllocator sr_terrain_vertex_allocator = {
    sr_terrain_vertex_allocate, sr_terrain_vertex_release, NULL
};
static void sr_terrain_mesh_rejected(JceSceneRenderer *sr, int slot, int cx,
    int cz, int lod, int vertices, int indices, uint32_t reason, const char *label)
{
    if (!(sr->terrain_vertex_reported_reasons & reason)) {
        sr->terrain_vertex_reported_reasons |= reason;
        LOG_WARN(LOG_TAG, "terrain: mesh rejected owner=%p asset=%s slot=%d chunk=(%d,%d) lod=%d vertices=%d indices=%d reason=%s terrain_stride=%u mesh_stride=%u",
            (void *)sr, sr->terrain_cache[slot].path, slot, cx, cz, lod,
            vertices, indices, label, (unsigned)sizeof(JceTerrainVertex),
            (unsigned)sizeof(JceMeshVertex));
    }
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "sr.terrain.mesh.rejected.reason", reason);
}
static JceMesh *sr_terrain_chunk_mesh_build(JceSceneRenderer *sr, int slot,
                               int cx, int cz, int lod)
{
    JceTerrain *terr = sr->terrain_cache[slot].terrain;
    if (!terr) return NULL;
    int ncx = sr->terrain_cache[slot].chunk_nx;
    int idx = cz * ncx + cx;
    if (idx < 0 || idx >= sr->terrain_cache[slot].chunk_count) return NULL;

    if (sr->terrain_cache[slot].chunk_meshes[idx] &&
        sr->terrain_cache[slot].chunk_lod[idx] == (int8_t)lod)
        return sr->terrain_cache[slot].chunk_meshes[idx];

    int core_v = 0, core_i = 0;
    jce_terrain_chunk_mesh_size(terr, cx, cz, lod, &core_v, &core_i);
    if (core_v <= 0 || core_i <= 0 || core_v > INT_MAX / 24 ||
        core_i > INT_MAX - core_v * 24 || lod < 0 || lod > 30) {
        sr_terrain_mesh_rejected(sr, slot, cx, cz, lod, core_v, core_i,
            1u, "invalid-generated-capacity");
        return NULL;
    }

    /* Skirt adds at most a full ring of duplicated edge verts plus two
     * triangles per edge segment.  nx == nz == sqrt(core_v) for square
     * chunks, but allocate the worst case: 4 edges of `core_v` verts. */
    int skirt_v_cap = 4 * core_v;
    int skirt_i_cap = 4 * core_v * 6;
    int cap_v = core_v + skirt_v_cap;
    int cap_i = core_i + skirt_i_cap;
    if ((size_t)cap_v > SIZE_MAX / sizeof(JceTerrainVertex) ||
        (size_t)cap_i > SIZE_MAX / sizeof(uint32_t)) {
        sr_terrain_mesh_rejected(sr, slot, cx, cz, lod, cap_v, cap_i,
            1u, "source-byte-capacity-overflow");
        return NULL;
    }

    JceTerrainVertex *vb = (JceTerrainVertex *)
        JCE_MALLOC(sizeof(JceTerrainVertex) * (size_t)cap_v);
    uint32_t *ib = (uint32_t *)JCE_MALLOC(sizeof(uint32_t) * (size_t)cap_i);
    if (!vb || !ib) {
        JCE_FREE(vb); JCE_FREE(ib);
        sr_terrain_mesh_rejected(sr, slot, cx, cz, lod, cap_v, cap_i,
            2u, "source-allocation-failed");
        return NULL;
    }

    int wrote_v = 0, wrote_i = 0;
    jce_terrain_chunk_build_mesh(terr, cx, cz, lod,
                                 vb, core_v, ib, core_i, &wrote_v, &wrote_i);
    if (wrote_v <= 0 || wrote_i <= 0 || wrote_v > core_v || wrote_i > core_i) {
        JCE_FREE(vb); JCE_FREE(ib);
        sr_terrain_mesh_rejected(sr, slot, cx, cz, lod, wrote_v, wrote_i,
            4u, "invalid-generated-count");
        return NULL;
    }

    /* The core grid is row-major nx*nz (see jce_terrain_chunk_build_mesh).
     * Derive nx/nz from the chunk extents at this LOD to walk its border. */
    int w  = jce_terrain_width(terr);
    int h  = jce_terrain_height(terr);
    int cs = jce_terrain_chunk_size(terr);
    int x0 = cx * cs, z0 = cz * cs;
    int x1 = x0 + cs, z1 = z0 + cs;
    if (x1 > w - 1) x1 = w - 1;
    if (z1 > h - 1) z1 = h - 1;
    int step = (lod <= 0) ? 1 : (1 << lod);
    int nx = (x1 - x0) / step + 1;
    int nz = (z1 - z0) / step + 1;

    float skirt = jce_terrain_max_height(terr) * SR_TERRAIN_SKIRT_FRAC;
    if (skirt < 0.01f) skirt = 0.01f;

    /* Append a skirt strip along one chunk border.  edge_ids[0..count-1] are
     * the core border vertex indices in order; for each we add a duplicated
     * vertex dropped by `skirt`, then stitch vertical quads between the core
     * ring and the apron.  `flip` selects the winding so all four edges face
     * outward consistently with the terrain's CW core winding. */
    {
        /* nx and nz are at most chunk_size+1; cap the border walk so an
         * unusually large chunk_size cannot overflow this scratch ring. */
        enum { SR_TC_EDGE_CAP = 4097 };
        int edge_ids[SR_TC_EDGE_CAP];
        for (int e4 = 0; e4 < 4; e4++) {
            int count, flip;
            switch (e4) {
            case 0: /* north (j=0)      */ count = nx; flip = 0; break;
            case 1: /* south (j=nz-1)   */ count = nx; flip = 1; break;
            case 2: /* west  (i=0)      */ count = nz; flip = 1; break;
            default:/* east  (i=nx-1)   */ count = nz; flip = 0; break;
            }
            if (count > SR_TC_EDGE_CAP) count = SR_TC_EDGE_CAP;
            for (int s = 0; s < count; s++) {
                switch (e4) {
                case 0:  edge_ids[s] = s;                       break;
                case 1:  edge_ids[s] = (nz - 1) * nx + s;       break;
                case 2:  edge_ids[s] = s * nx;                  break;
                default: edge_ids[s] = s * nx + (nx - 1);       break;
                }
            }
            int first_apron = wrote_v;
            for (int s = 0; s < count; s++) {
                int cvid = edge_ids[s];
                if (cvid < 0 || cvid >= core_v) continue;
                if (wrote_v >= cap_v) break;
                JceTerrainVertex a = vb[cvid];
                a.py -= skirt;
                vb[wrote_v++] = a;
            }
            for (int s = 0; s + 1 < count; s++) {
                if (wrote_i + 6 > cap_i) break;
                int top0 = edge_ids[s];
                int top1 = edge_ids[s + 1];
                int bot0 = first_apron + s;
                int bot1 = first_apron + s + 1;
                if (top0 < 0 || top1 < 0 ||
                    top0 >= core_v || top1 >= core_v ||
                    bot1 >= wrote_v) continue;
                if (flip) {
                    ib[wrote_i++] = (uint32_t)top0; ib[wrote_i++] = (uint32_t)bot0;
                    ib[wrote_i++] = (uint32_t)top1; ib[wrote_i++] = (uint32_t)top1;
                    ib[wrote_i++] = (uint32_t)bot0; ib[wrote_i++] = (uint32_t)bot1;
                } else {
                    ib[wrote_i++] = (uint32_t)top0; ib[wrote_i++] = (uint32_t)top1;
                    ib[wrote_i++] = (uint32_t)bot0; ib[wrote_i++] = (uint32_t)bot1;
                    ib[wrote_i++] = (uint32_t)bot0; ib[wrote_i++] = (uint32_t)top1;
                }
            }
        }
    }

    SrTerrainVertexOwner converted = {0};
    const SrTerrainVertexResult conversion = sr_terrain_vertices_create(
        &converted, vb, (uint32_t)wrote_v, &sr_terrain_vertex_allocator);
    JCE_FREE(vb);
    if (conversion != SR_TERRAIN_VERTICES_OK) {
        JCE_FREE(ib);
        sr_terrain_mesh_rejected(sr, slot, cx, cz, lod, wrote_v, wrote_i,
            8u, conversion == SR_TERRAIN_VERTICES_OOM ?
                "converted-allocation-failed" : "invalid-conversion-input");
        return NULL;
    }
    /* Mesh owner copies the input and generates tangents for the whole zero
     * tangent stream. The public terrain vertex format stays unchanged. */
    JceMesh *m = jce_mesh_create(converted.vertices, (uint32_t)wrote_v,
                                 ib, (uint32_t)wrote_i);
    sr_terrain_vertices_destroy(&converted);
    JCE_FREE(ib);
    if (!m) {
        sr_terrain_mesh_rejected(sr, slot, cx, cz, lod, wrote_v, wrote_i,
            16u, "mesh-owner-create-failed");
        return NULL;
    }
    /* Keep the previous allocation owned until the candidate is accepted.
     * A failed request returns NULL and cannot masquerade as its old LOD. */
    if (sr->terrain_cache[slot].chunk_meshes[idx])
        jce_mesh_destroy(sr->terrain_cache[slot].chunk_meshes[idx]);
    sr->terrain_cache[slot].chunk_meshes[idx] = m;
    sr->terrain_cache[slot].chunk_lod[idx]    = (int8_t)lod;
    return m;
}

JceMesh *sr_terrain_chunk_mesh(JceSceneRenderer *sr, int slot,
                               int cx, int cz, int lod)
{
    if (!sr || slot < 0 || slot >= 16 || cx < 0 || cz < 0 || lod < 0 || lod > 30 ||
        cx >= sr->terrain_cache[slot].chunk_nx ||
        cz >= sr->terrain_cache[slot].chunk_nz) return NULL;
    const int idx = cz * sr->terrain_cache[slot].chunk_nx + cx;
    if (idx >= sr->terrain_cache[slot].chunk_count ||
        !sr->terrain_cache[slot].terrain ||
        !sr->terrain_cache[slot].chunk_meshes ||
        !sr->terrain_cache[slot].chunk_lod) return NULL;
    if (sr->terrain_cache[slot].chunk_meshes[idx] &&
        sr->terrain_cache[slot].chunk_lod[idx] == (int8_t)lod)
        return sr->terrain_cache[slot].chunk_meshes[idx];
    JceTraceScope scope = jce_trace_scope_begin("sr.terrain.chunk_mesh");
    JceMesh *mesh = sr_terrain_chunk_mesh_build(sr, slot, cx, cz, lod);
    jce_trace_scope_end(scope, mesh ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return mesh;
}

/* Find (or lazily load) the terrain cache slot for `path`, initialising the
 * per-chunk metadata on first load.  Returns the slot index, or -1 on failure
 * (no free slot, load failed, or chunk init failed). */
/* Release everything this slot OWNS.  The JceTerrain is not among it: that is
 * borrowed from the scene's terrain cache and freed there.  Freeing it here
 * would leave the pick pass and the physics world pointing at a dead grid. */
void sr_terrain_slot_free(JceSceneRenderer *sr, int slot)
{
    if (!sr || slot < 0 || slot >= 16) return;
    sr_terrain_free_chunks(sr, slot);
    if (BGFX_HANDLE_IS_VALID(sr->terrain_cache[slot].splat_tex))
        bgfx_destroy_texture(sr->terrain_cache[slot].splat_tex);
    memset(&sr->terrain_cache[slot], 0, sizeof sr->terrain_cache[slot]);
    sr->terrain_cache[slot].splat_tex.idx = UINT16_MAX;
}

static int sr_terrain_clampi(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

void jce_scene_renderer_invalidate_terrain_region(
    JceSceneRenderer *sr, JceScene *scene, const char *path,
    float min_x, float min_z, float max_x, float max_z,
    uint32_t edit_flags)
{
    if (!sr || edit_flags == 0u) return;
    if (min_x > max_x) { float t = min_x; min_x = max_x; max_x = t; }
    if (min_z > max_z) { float t = min_z; min_z = max_z; max_z = t; }

    for (int slot = 0; slot < 16; ++slot) {
        if (!sr->terrain_cache[slot].used ||
            !sr->terrain_cache[slot].terrain)
            continue;
        if (path && path[0] &&
            strncmp(sr->terrain_cache[slot].path, path,
                    sizeof sr->terrain_cache[slot].path) != 0)
            continue;

        JceTerrain *terrain = sr->terrain_cache[slot].terrain;
        if (scene && path && path[0]) {
            JceTerrainCache *cache = jce_scene_terrain_cache(scene);
            if (jce_terrain_cache_peek(cache, path) == terrain) {
                const uint64_t revision =
                    jce_terrain_cache_revision(cache, path);
                if (revision != 0u)
                    sr->terrain_cache[slot].revision = revision;
            }
        }
        const int nx = sr->terrain_cache[slot].chunk_nx;
        const int nz = sr->terrain_cache[slot].chunk_nz;
        const int chunk_size = jce_terrain_chunk_size(terrain);
        const int width = jce_terrain_width(terrain);
        const int height = jce_terrain_height(terrain);
        if (nx <= 0 || nz <= 0 || chunk_size <= 0 ||
            width < 2 || height < 2)
            continue;

        const float terrain_size_x = jce_terrain_world_size_x(terrain);
        const float terrain_size_z = jce_terrain_world_size_z(terrain);
        if (max_x < 0.0f || max_z < 0.0f ||
            min_x > terrain_size_x || min_z > terrain_size_z)
            continue;
        const float region_min_x = min_x < 0.0f ? 0.0f : min_x;
        const float region_min_z = min_z < 0.0f ? 0.0f : min_z;
        const float region_max_x = max_x > terrain_size_x
            ? terrain_size_x : max_x;
        const float region_max_z = max_z > terrain_size_z
            ? terrain_size_z : max_z;

        const float cell_x = terrain_size_x / (float)(width - 1);
        const float cell_z = terrain_size_z / (float)(height - 1);
        const float chunk_x = cell_x * (float)chunk_size;
        const float chunk_z = cell_z * (float)chunk_size;
        if (chunk_x <= 0.0f || chunk_z <= 0.0f) continue;

        int cx0 = (int)floorf(region_min_x / chunk_x);
        int cz0 = (int)floorf(region_min_z / chunk_z);
        int cx1 = (int)floorf(region_max_x / chunk_x);
        int cz1 = (int)floorf(region_max_z / chunk_z);
        cx0 = sr_terrain_clampi(cx0, 0, nx - 1);
        cz0 = sr_terrain_clampi(cz0, 0, nz - 1);
        cx1 = sr_terrain_clampi(cx1, 0, nx - 1);
        cz1 = sr_terrain_clampi(cz1, 0, nz - 1);

        if (edit_flags & (JCE_TERRAIN_EDIT_HEIGHTS |
                          JCE_TERRAIN_EDIT_HOLES)) {
            /* Neighbour chunk edge normals sample across the edited boundary. */
            const int hx0 = sr_terrain_clampi(cx0 - 1, 0, nx - 1);
            const int hz0 = sr_terrain_clampi(cz0 - 1, 0, nz - 1);
            const int hx1 = sr_terrain_clampi(cx1 + 1, 0, nx - 1);
            const int hz1 = sr_terrain_clampi(cz1 + 1, 0, nz - 1);
            for (int cz = hz0; cz <= hz1; ++cz) {
                for (int cx = hx0; cx <= hx1; ++cx) {
                    const int index = cz * nx + cx;
                    if (sr->terrain_cache[slot].chunk_meshes[index]) {
                        jce_mesh_destroy(
                            sr->terrain_cache[slot].chunk_meshes[index]);
                        sr->terrain_cache[slot].chunk_meshes[index] = NULL;
                    }
                    sr->terrain_cache[slot].chunk_lod[index] = -1;
                    sr_terrain_chunk_local_aabb(
                        terrain, cx, cz,
                        &sr->terrain_cache[slot].chunk_min[index],
                        &sr->terrain_cache[slot].chunk_max[index]);
                }
            }
        }

        if (edit_flags & JCE_TERRAIN_EDIT_SPLAT) {
            if (sr->terrain_cache[slot].chunk_splat_tex) {
                for (int cz = cz0; cz <= cz1; ++cz) {
                    for (int cx = cx0; cx <= cx1; ++cx) {
                        const int index = cz * nx + cx;
                        bgfx_texture_handle_t *texture =
                            &sr->terrain_cache[slot].chunk_splat_tex[index];
                        if (BGFX_HANDLE_IS_VALID(*texture))
                            bgfx_destroy_texture(*texture);
                        texture->idx = UINT16_MAX;
                    }
                }
            } else {
                if (BGFX_HANDLE_IS_VALID(sr->terrain_cache[slot].splat_tex))
                    bgfx_destroy_texture(sr->terrain_cache[slot].splat_tex);
                sr->terrain_cache[slot].splat_tex.idx = UINT16_MAX;
                sr->terrain_cache[slot].splat_uploaded = false;
            }
        }
    }
}

/* JceTerrainCacheLoadFn: PAK-first so a deployed bundle needs no host
 * filesystem access; the cache owns whatever this returns. */
static JceTerrain *sr_terrain_cache_load(void *ud, const char *path)
{
    JceSceneRenderer *sr = (JceSceneRenderer *)ud;
    /* jce-terrain-owner-exempt: this IS the cache's loader callback; the
     * cache takes ownership of the result. */
    JceTerrain *terr = jce_terrain_load_from_pak(sr->pak, path);
    if (terr) return terr;

    char        resolved[1024];
    const char *load_path = path;
    if (sr->has_cbs && sr->cbs.resolve_path &&
        sr->cbs.resolve_path(path, resolved, (int)sizeof(resolved),
                             sr->cbs.userdata)) {
        load_path = resolved;
    }
    /* jce-terrain-owner-exempt: same callback, host-filesystem fallback. */
    return jce_terrain_load_file(load_path);
}

int sr_terrain_find_or_load_slot(JceSceneRenderer *sr, JceScene *scene,
                                 const char *path)
{
    if (!sr || !scene || !path || !path[0]) return -1;
    JceTerrainCache *tcache = jce_scene_terrain_cache(scene);
    const uint64_t rev = jce_terrain_cache_revision(tcache, path);

    int slot = -1, free_slot = -1;
    for (int i = 0; i < 16; i++) {
        if (sr->terrain_cache[i].used &&
            strncmp(sr->terrain_cache[i].path, path,
                    sizeof sr->terrain_cache[i].path) == 0) {
            slot = i; break;
        }
        if (!sr->terrain_cache[i].used && free_slot < 0) free_slot = i;
    }

    /* A stale borrow is a rebuild, not a reuse: chunk meshes and the splat
     * texture were derived from a grid the cache may already have freed.  This
     * is the path an editor sculpt travels -- before the shared cache existed
     * there was no revision to compare and the renderer simply kept drawing
     * whatever it loaded at level open. */
    if (slot >= 0 && sr->terrain_cache[slot].revision != rev) {
        sr_terrain_slot_free(sr, slot);
        free_slot = slot;
        slot = -1;
    }

    if (slot >= 0)
        return sr->terrain_cache[slot].failed ? -1 : slot;
    if (free_slot < 0) return -1;

    slot = free_slot;
    memset(&sr->terrain_cache[slot], 0, sizeof sr->terrain_cache[slot]);
    jce_strlcpy(sr->terrain_cache[slot].path, path,
                sizeof sr->terrain_cache[slot].path);
    sr->terrain_cache[slot].used = true;
    sr->terrain_cache[slot].splat_tex.idx = UINT16_MAX;

    /* BORROWED from the scene's terrain cache -- the pick pass and the physics
     * world read the same grid.  Never freed here. */
    JceTerrain *terr = jce_terrain_cache_acquire(tcache, path,
                                                 sr_terrain_cache_load, sr);
    sr->terrain_cache[slot].revision =
        jce_terrain_cache_revision(tcache, path);
    if (!terr) {
        sr->terrain_cache[slot].failed = true;
        LOG_WARN(LOG_TAG, "terrain load failed: '%s'", path);
        return -1;
    }
    sr->terrain_cache[slot].terrain = terr;
    if (!sr_terrain_init_chunks(sr, slot)) {
        sr->terrain_cache[slot].failed = true;
        return -1;
    }
    return slot;
}

/* ── Terrain per-chunk draw (P1-terrain-lod) ──────────────────────────
 *
 * Replaces the old "merge every chunk into one ~16M-vert mesh" draw.  For each
 * terrain entity we walk its chunk grid and, per chunk:
 *   1. transform the chunk's local AABB by the entity world matrix,
 *   2. frustum-cull it against the camera (skip if fully outside),
 *   3. pick a LOD from the camera→chunk-centre distance,
 *   4. build/cache the chunk mesh (with skirts) at that LOD,
 *   5. re-bind transform + terrain textures/params and submit one draw.
 * Step 5 is repeated per chunk because jce_mesh_submit_terrain discards all
 * bound state (BGFX_DISCARD_ALL) after each submit. */

/* Build a box-filtered RGBA8 mip chain for the splat map into ONE bgfx memory
 * block (mip0..mipN, sequential, as bgfx expects for a hasMips upload).
 * Large-world #4: an un-mipped W×H splat (~64 MB at 4097²) aliases badly on
 * distant terrain and can never be dropped under VRAM pressure; a real mip chain
 * lets trilinear sampling pick the right level and the mip-bias hook evict the
 * top mips.  Averages the 4 packed layer weights per 2×2 (weights stay ~summed). */
static const bgfx_memory_t *sr_splat_build_mips(const uint32_t *splat, int w, int h)
{
    int    mw = w, mh = h, levels = 1;
    size_t total = (size_t)w * h * 4u;
    while (mw > 1 || mh > 1) {
        mw = mw > 1 ? mw >> 1 : 1; mh = mh > 1 ? mh >> 1 : 1;
        total += (size_t)mw * mh * 4u; ++levels;
    }
    const bgfx_memory_t *mem = bgfx_alloc((uint32_t)total);
    if (!mem) return NULL;
    uint8_t *dst = mem->data;
    memcpy(dst, splat, (size_t)w * h * 4u);            /* mip 0 = source */
    const uint8_t *prev = dst; int pw = w, ph = h;
    uint8_t *cur = dst + (size_t)w * h * 4u;
    mw = w > 1 ? w >> 1 : 1; mh = h > 1 ? h >> 1 : 1;
    for (int l = 1; l < levels; ++l) {
        for (int y = 0; y < mh; ++y)
            for (int x = 0; x < mw; ++x) {
                int x0 = x * 2, y0 = y * 2;
                int x1 = (x0 + 1 < pw) ? x0 + 1 : x0;
                int y1 = (y0 + 1 < ph) ? y0 + 1 : y0;
                for (int c = 0; c < 4; ++c) {
                    int s = prev[(y0 * pw + x0) * 4 + c] + prev[(y0 * pw + x1) * 4 + c]
                          + prev[(y1 * pw + x0) * 4 + c] + prev[(y1 * pw + x1) * 4 + c];
                    cur[(y * mw + x) * 4 + c] = (uint8_t)(s >> 2);
                }
            }
        prev = cur; pw = mw; ph = mh; cur += (size_t)mw * mh * 4u;
        mw = mw > 1 ? mw >> 1 : 1; mh = mh > 1 ? mh >> 1 : 1;
    }
    return mem;
}

/* Lazily build + cache chunk (cx,cz)'s per-tile splat texture (with the same
 * box-filtered mip chain as the monolithic path) for a TILED terrain. Assumes
 * chunk_size == tile_dim so chunk index == tile (cx,cz). Returns an invalid
 * handle on failure (caller falls back to white). */
static bgfx_texture_handle_t sr_terrain_tile_splat_tex(JceSceneRenderer *sr,
                                                       int slot, int cx, int cz,
                                                       int tile_dim)
{
    bgfx_texture_handle_t inv = { UINT16_MAX };
    bgfx_texture_handle_t *cache = sr->terrain_cache[slot].chunk_splat_tex;
    if (!cache) return inv;
    int ncx = sr->terrain_cache[slot].chunk_nx;
    int idx = cz * ncx + cx;
    if (idx < 0 || idx >= sr->terrain_cache[slot].chunk_count) return inv;
    if (BGFX_HANDLE_IS_VALID(cache[idx])) return cache[idx];

    int    span = tile_dim + 1;
    size_t n    = (size_t)span * (size_t)span;
    uint32_t *sp = (uint32_t *)JCE_MALLOC(n * sizeof(uint32_t));
    bgfx_texture_handle_t st = inv;
    if (sp && jce_terrain_tile_copy(sr->terrain_cache[slot].terrain,
                                    cx, cz, NULL, sp)) {
        const bgfx_memory_t *mem = sr_splat_build_mips(sp, span, span);
        if (mem)
            st = bgfx_create_texture_2d((uint16_t)span, (uint16_t)span, true, 1,
                    BGFX_TEXTURE_FORMAT_RGBA8,
                    BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem, 0);
    }
    if (sp) JCE_FREE(sp);
    cache[idx] = st;
    return st;
}

/* A roof is a rectangular plane with zero transmission. A canopy is an
 * ellipse with authored transmission. Project the same transformed plane
 * into world XZ and retain its world Y slope for the fragment height test. */
static bool sr_terrain_snow_cover(const jce_mat4 *world, float transmission,
                                  float cover[SR_SNOW_COVER_VECS][4])
{
    const jce_vec4 center = world->col[3];
    const jce_vec4 x = world->col[0], z = world->col[2];
    const float determinant = x.x * z.z - z.x * x.z;
    const float reach_x = fabsf(x.x) + fabsf(z.x);
    const float reach_z = fabsf(x.z) + fabsf(z.z);
    if (!(fabsf(determinant) >= 0.025f && reach_x <= 48.0f && reach_z <= 48.0f &&
          fabsf(x.y) + fabsf(z.y) <= 48.0f)) return false;

    const float inverse = 1.0f / determinant;
    const float data[SR_SNOW_COVER_VECS][4] = {
        {center.x, center.y, center.z, transmission},
        {z.z * inverse, -z.x * inverse, -x.z * inverse, x.x * inverse},
        {x.y, z.y, reach_x, reach_z},
    };
    memcpy(cover, data, sizeof data);
    return true;
}

static void sr_terrain_snow_markers(JceSceneRenderer *sr, JceScene *scene,
    JceEntity terrain_entity, JceTerrain *terrain, const jce_mat4 *model,
    bool allow_stamps, bool allow_cover,
    SrTerrainSnowMarkers *out)
{
    allow_stamps = allow_stamps && !jce_terrain_is_tiled(terrain);
    if (!allow_stamps && !allow_cover) return;
    JceEntity children[SR_SNOW_MARKER_SCAN_MAX];
    const int count = jce_scene_get_children(scene, terrain_entity, children,
                                             SR_SNOW_MARKER_SCAN_MAX);
    JceTerrainSurface surface;
    bool surface_ready = false;
    for (int i = 0; i < count; ++i) {
        const JceEntity child = children[i];
        const char *tag = jce_scene_get_entity_tag_name(scene, child);
        const bool stamp = allow_stamps && out->stamp_count < SR_SNOW_STAMP_MAX &&
                           strcmp(tag, "SnowStamp") == 0;
        const bool cover_slot = allow_cover && out->cover_count < SR_SNOW_COVER_MAX;
        const bool roof = cover_slot && strcmp(tag, "SnowRoof") == 0;
        const bool canopy = cover_slot && strcmp(tag, "SnowCanopy") == 0;
        const bool mesh = cover_slot && strcmp(tag, "SnowOccluder") == 0;
        if ((!stamp && !roof && !canopy && !mesh) || !entity_enabled(scene, child)) continue;
        const JceTransform *marker = jce_scene_get_transform(scene, child);
        if (!marker) continue;
        const jce_mat4 world = jce_scene_get_world_matrix(scene, child);
        const jce_vec4 p = world.col[3];
        if (!(fabsf(p.x) <= 32768.0f && fabsf(p.z) <= 32768.0f) || !isfinite(p.y)) continue;
        if (mesh) {
            out->cover_count += sr_snow_mesh_cover(sr, scene, child, &world,
                &out->covers[out->cover_count], SR_SNOW_COVER_MAX - out->cover_count);
            continue;
        }
        if (roof || canopy) {
            const float transmission = roof ? -1.0f : marker->scale.y;
            if ((!canopy || (transmission >= 0.0f && transmission < 1.0f)) &&
                sr_terrain_snow_cover(&world, transmission, out->covers[out->cover_count]))
                ++out->cover_count;
            continue;
        }

        if (!(marker->scale.x >= 0.20f && marker->scale.x <= 2.50f) ||
            !sr_snow_depth_valid(marker->scale.y)) continue;
        if (!surface_ready) {
            surface_ready = jce_terrain_surface_init(&surface, terrain, model);
            if (!surface_ready) { allow_stamps = false; continue; }
        }
        float ground;
        if (!jce_terrain_surface_sample(&surface, p.x, p.z, false, &ground, NULL) ||
            !(fabsf(p.y - ground) <= 0.35f)) continue;
        float *entry = out->stamps[out->stamp_count++];
        entry[0] = p.x;
        entry[1] = p.z;
        entry[2] = marker->scale.x;
        entry[3] = marker->scale.y;
    }
}

static uint16_t sr_terrain_chunk_snow_cover(const SrTerrainSnowMarkers *markers,
    jce_vec3 min, jce_vec3 max, float covers[SR_SNOW_COVER_MAX][SR_SNOW_COVER_VECS][4])
{
    uint16_t count = 0;
    for (uint16_t i = 0; i < markers->cover_count; ++i) {
        const float (*cover)[4] = markers->covers[i];
        const float *center = cover[0], *slope_and_reach = cover[2];
        if (center[0] + slope_and_reach[2] < min.x || center[0] - slope_and_reach[2] > max.x ||
            center[2] + slope_and_reach[3] < min.z || center[2] - slope_and_reach[3] > max.z ||
            center[1] + fabsf(slope_and_reach[0]) + fabsf(slope_and_reach[1]) <= min.y + 0.08f)
            continue;
        memcpy(covers[count++], cover, sizeof covers[0]);
    }
    return count;
}

typedef struct {
    float snow_amount;
    float minimum_radius;
} SrTerrainSnowGeometry;

static float sr_terrain_geometry_snow_amount(const JceSceneRenderer *sr)
{
    if (!sr->env || !(sr->env->snow_amount >= 1.0f / 32.0f)) return 0.0f;
    return floorf(fminf(sr->env->snow_amount, 1.0f) * 32.0f) / 32.0f;
}

static SrTerrainSnowGeometry sr_terrain_snow_geometry(JceSceneRenderer *sr,
    JceTerrain *terrain, const jce_mat4 *model, const SrTerrainSnowMarkers *snow)
{
    const SrTerrainSnowGeometry none = {0};
    const float amount = sr_terrain_geometry_snow_amount(sr);
    if (amount == 0.0f || snow->stamp_count == 0) return none;
    const int width = jce_terrain_width(terrain), height = jce_terrain_height(terrain);
    if (width < 2 || height < 2) return none;
    const float dx = hypotf(model->col[0].x, model->col[0].z) *
        jce_terrain_world_size_x(terrain) / (float)(width - 1);
    const float dz = hypotf(model->col[2].x, model->col[2].z) *
        jce_terrain_world_size_z(terrain) / (float)(height - 1);
    const float radius = 1.5f * fmaxf(dx, dz);
    if (!(radius > 0.0f && radius <= 2.5f)) return none;
    float largest = 0.0f;
    for (uint16_t i = 0; i < snow->stamp_count; ++i)
        largest = fmaxf(largest, snow->stamps[i][2]);
    if (largest < radius) return none;

    struct SrTerrainMaterialCache *cache = sr->terrain_material_cache;
    if (!cache || !cache->snow_geometry_enabled ||
        !BGFX_HANDLE_IS_VALID(cache->snow_geometry)) return none;
    if (!cache->snow_depth_attempted) {
        cache->snow_depth_attempted = true;
        cache->snow_depth.idx = shader_load_program_named(sr->pak, "terrain_snow_depth", "shadow").idx;
    }
    if (!BGFX_HANDLE_IS_VALID(cache->snow_depth)) return none;
    return (SrTerrainSnowGeometry){amount, radius};
}

static SrTerrainSnowGeometry sr_terrain_collect_geometry(JceSceneRenderer *sr,
    JceScene *scene, JceEntity entity, JceTerrain *terrain, const jce_mat4 *model,
    SrTerrainSnowMarkers *snow)
{
    if (!terrain || sr_terrain_geometry_snow_amount(sr) == 0.0f ||
        !jce_shader_valid(jce_renderer_get_program_terrain(sr->renderer)))
        return (SrTerrainSnowGeometry){0};
    const struct SrTerrainMaterialCache *cache = sr_terrain_material_cache_get(sr);
    if (!cache || !cache->snow_geometry_enabled) return (SrTerrainSnowGeometry){0};
    sr_terrain_snow_markers(sr, scene, entity, terrain, model,
        BGFX_HANDLE_IS_VALID(cache->snow_stamps), BGFX_HANDLE_IS_VALID(cache->snow_cover), snow);
    return sr_terrain_snow_geometry(sr, terrain, model, snow);
}

static bool sr_terrain_chunk_snow_geometry(const SrTerrainSnowMarkers *snow,
    SrTerrainSnowGeometry geometry, jce_vec3 min, jce_vec3 max)
{
    if (geometry.snow_amount == 0.0f) return false;
    for (uint16_t i = 0; i < snow->stamp_count; ++i) {
        const float *stamp = snow->stamps[i];
        if (stamp[2] < geometry.minimum_radius) continue;
        const float reach = stamp[2] * 1.035f;
        if (stamp[0] + reach >= min.x && stamp[0] - reach <= max.x &&
            stamp[1] + reach >= min.z && stamp[1] - reach <= max.z)
            return true;
    }
    return false;
}

static void sr_terrain_bind_snow(JceSceneRenderer *sr, const SrTerrainSnowMarkers *snow,
    SrTerrainSnowGeometry geometry, jce_vec3 min, jce_vec3 max, float params[4])
{
    const struct SrTerrainMaterialCache *cache = sr->terrain_material_cache;
    float chunk_cover[SR_SNOW_COVER_MAX][SR_SNOW_COVER_VECS][4];
    const uint16_t covers = sr_terrain_chunk_snow_cover(snow, min, max, chunk_cover);
    params[2] = (float)snow->stamp_count;
    if (covers > 0) {
        params[2] = -(float)(covers * SR_SNOW_COUNT_RADIX + snow->stamp_count);
        bgfx_set_uniform(cache->snow_cover, chunk_cover, (uint16_t)(covers * SR_SNOW_COVER_VECS));
    }
    bgfx_set_uniform(sr->u_terrain_params, params, 1);
    if (snow->stamp_count > 0)
        bgfx_set_uniform(cache->snow_stamps, snow->stamps, snow->stamp_count);
    if (cache && BGFX_HANDLE_IS_VALID(cache->snow_geometry)) {
        const float data[4] = {geometry.snow_amount, geometry.minimum_radius,
                               SR_SNOW_DEPTH_MAX_M, SR_SNOW_RIM_MAX_M};
        bgfx_set_uniform(cache->snow_geometry, data, 1);
    }
}

uint64_t sr_terrain_snow_shadow_key(JceSceneRenderer *sr, JceScene *scene, const EntityList *list)
{
    if (sr_terrain_geometry_snow_amount(sr) == 0.0f ||
        !jce_shader_valid(jce_renderer_get_program_terrain(sr->renderer))) return 0;
    const struct SrTerrainMaterialCache *material = sr_terrain_material_cache_get(sr);
    if (!material || !material->snow_geometry_enabled) return 0;
    uint64_t key = 0;
    JceTerrainCache *terrain_cache = jce_scene_terrain_cache(scene);
    for (int i = 0; i < list->count; ++i) {
        const JceEntity entity = list->entities[i];
        JceTerrainComponent *tc = jce_scene_get_terrain(scene, entity);
        if (!tc || !tc->visible || !tc->terrain_path[0] || !entity_enabled(scene, entity) ||
            !jce_scene_has_transform(scene, entity) || !jce_scene_get_child_count(scene, entity))
            continue;
        const int slot = sr_terrain_find_or_load_slot(sr, scene, tc->terrain_path);
        if (slot < 0 || sr->terrain_cache[slot].chunk_count <= 0) continue;
        const jce_mat4 model = jce_scene_get_world_matrix(scene, entity);
        SrTerrainSnowMarkers snow = {0};
        const SrTerrainSnowGeometry geometry = sr_terrain_collect_geometry(sr, scene, entity,
            sr->terrain_cache[slot].terrain, &model, &snow);
        if (geometry.snow_amount == 0.0f) continue;
        if (key == 0) key = JCE_FNV1A64_INIT;
        key = jce_fnv1a64_append(key, &entity, sizeof entity);
        key = jce_fnv1a64_append(key, &model, sizeof model);
        const uint64_t asset_revision = terrain_cache
            ? jce_terrain_cache_revision(terrain_cache, tc->terrain_path) : 0;
        key = jce_fnv1a64_append(key, &asset_revision, sizeof asset_revision);
        key = jce_fnv1a64_append(key, &geometry.snow_amount, sizeof geometry.snow_amount);
        key = jce_fnv1a64_append(key, &geometry.minimum_radius, sizeof geometry.minimum_radius);
        key = jce_fnv1a64_append(key, &snow.stamp_count, sizeof snow.stamp_count);
        key = jce_fnv1a64_append(key, snow.stamps, snow.stamp_count * sizeof snow.stamps[0]);
        key = jce_fnv1a64_append(key, &snow.cover_count, sizeof snow.cover_count);
        key = jce_fnv1a64_append(key, snow.covers, snow.cover_count * sizeof snow.covers[0]);
    }
    return key;
}

void sr_draw_terrain_chunks(JceSceneRenderer *sr, JceScene *scene,
                            EntityList *list, JceEntity entity,
                            const JceCamera *camera, uint16_t view_id,
                            int slot, JceTerrainComponent *tc,
                            const jce_mat4 *model,
                            const JcePbrMaterial *pbr, uint32_t receiver_layer,
                            JceSceneViewModeKind mode)
{
    if (!sr || slot < 0 || slot >= 16) return;
    JceTerrain *terr = sr->terrain_cache[slot].terrain;
    if (!terr || sr->terrain_cache[slot].chunk_count <= 0) return;

    /* The ordinary PBR fallback has a different sampler ABI. It keeps the
     * base layer, heightfield and chunk LOD without splat blending. */
    const bool layered = jce_shader_valid(jce_renderer_get_program_terrain(sr->renderer));
    bgfx_texture_handle_t layer_tex[4] = {
        sr->white_tex, sr->white_tex, sr->white_tex, sr->white_tex
    };
    for (int layer = 0; tc && layer < (layered ? 4 : 1); ++layer) {
        if (!tc->layer_albedo_path[layer][0]) continue;
        JceTexture texture = sr_resolve_texture_srgb(sr, tc->layer_albedo_path[layer]);
        if (jce_texture_valid(texture)) layer_tex[layer].idx = texture.idx;
    }

    /* Lazy splat texture upload (once per terrain). */
    if (layered && !sr->terrain_cache[slot].splat_uploaded) {
        int tw = jce_terrain_width(terr);
        int th = jce_terrain_height(terr);
        const uint32_t *splat = jce_terrain_splat(terr);
        if (splat && tw > 0 && th > 0) {
            /* hasMips=true + a full CPU-built mip chain: trilinear sampling then
             * picks the right level for distant chunks (no shimmer) and the mips
             * are droppable under VRAM pressure. */
            const bgfx_memory_t *mem = sr_splat_build_mips(splat, tw, th);
            if (mem)
                sr->terrain_cache[slot].splat_tex =
                    bgfx_create_texture_2d((uint16_t)tw, (uint16_t)th, true, 1,
                        BGFX_TEXTURE_FORMAT_RGBA8,
                        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem, 0);
        }
        sr->terrain_cache[slot].splat_uploaded = true;
    }
    bgfx_texture_handle_t splat_h = sr->terrain_cache[slot].splat_tex;
    if (!BGFX_HANDLE_IS_VALID(splat_h)) splat_h = sr->white_tex;

    SrTerrainMaterialBinding layer_data = layered
        ? sr_terrain_material_get(sr, tc) : (SrTerrainMaterialBinding){0};
    SrTerrainSnowMarkers snow = {0};
    const struct SrTerrainMaterialCache *material_cache = sr->terrain_material_cache;
    if (layered && material_cache && sr->env &&
        (sr->env->snow_amount > 0.001f || sr->env->global_wetness > 0.001f))
        sr_terrain_snow_markers(sr, scene, entity, terr, model,
            sr->env->snow_amount > 0.001f &&
                BGFX_HANDLE_IS_VALID(material_cache->snow_stamps),
            BGFX_HANDLE_IS_VALID(material_cache->snow_cover), &snow);
    const SrTerrainSnowGeometry snow_geometry = sr_terrain_snow_geometry(sr, terr, model, &snow);

    const JceSceneRenderLook *look = jce_scene_internal_get_render_look(scene);
    const JceSceneTerrainSnowLook snow_look = look ? look->terrain_snow : (JceSceneTerrainSnowLook){0};
    const float snow_look_data[4] = {
        snow_look.brightness, snow_look.warmth, snow_look.lowland_tint, snow_look.micro_normal_strength,
    };
    const bool snow_look_active = layered && material_cache && sr->env &&
        sr->env->snow_amount > 0.0f && BGFX_HANDLE_IS_VALID(material_cache->snow_look) &&
        (snow_look_data[0] != 0.0f || snow_look_data[1] != 0.0f ||
         snow_look_data[2] > 0.0f || snow_look_data[3] > 0.0f);
    float mask_count = layer_data.mask_flags[0] + layer_data.mask_flags[1] +
                       layer_data.mask_flags[2] + layer_data.mask_flags[3];

    float tparams[4] = {
        (tc && tc->tile_scale > 0.0f) ? tc->tile_scale : 10.0f,
        (tc && tc->splat_enabled) ? 1.0f : 0.0f,
        0.0f,
        (tc && mask_count > 0.5f)
            ? sr_terrain_clampf(tc->height_blend, 0.0f, 1.0f) : 0.0f
    };
    /* The reserved negative splat values opt this draw into the optional
     * uniform. Other terrain callers can keep binding their existing 0/1. */
    if (snow_look_active) tparams[1] = tparams[1] > 0.5f ? -2.0f : -1.0f;
    /* large-world #4 per-tile splat: a tiled terrain has no monolithic splat
     * map. When its tile grid aligns with the chunk grid (tile_dim==chunk_size,
     * so chunk i ↔ tile i) we bind a per-tile splat texture per chunk (in the
     * loop below) and remap the global UV into that tile via u_terrainTileUV.
     * Otherwise chunks can't be mapped to tiles, so fall back to a clean base
     * layer instead of blending an all-white (equal-weight) splat into mud. */
    int tg_x = 0, tg_z = 0, tg_dim = 0;
    jce_terrain_tile_grid(terr, &tg_x, &tg_z, &tg_dim);
    bool per_tile_splat = jce_terrain_is_tiled(terr) && tg_dim > 0 &&
                          tg_dim == jce_terrain_chunk_size(terr) &&
                          tc && tc->splat_enabled;
    if (jce_terrain_is_tiled(terr) && !per_tile_splat) tparams[1] = 0.0f;

    /* Frustum planes from the camera (independent of the entity-level cull
     * toggle so terrain always benefits from per-chunk culling). */
    jce_vec4 planes[6];
    bool have_planes = false;
    if (camera) {
        const jce_mat4 v  = jce_camera_view(camera);
        /* The viewport's aspect, not 16:9 -- see sr->frame_aspect. A narrower
         * test frustum than the real one culls ground that is still on
         * screen, and the ground is the one thing whose absence shows the
         * skybox through the floor. */
        const jce_mat4 p  = jce_camera_proj(camera, sr->frame_aspect,
                                            sr->homogeneous_depth);
        const jce_mat4 vp = jce_m4_multiply(&p, &v);
        sr_extract_frustum_planes(&vp, planes);
        have_planes = true;
    }
    jce_vec3 cam_pos = camera ? jce_camera_get_position(camera)
                              : jce_v3(0, 0, 0);

    /* Streaming prefetch (large-world #4): page in the tiles around the camera
     * before the chunk loop samples them, so crossing a tile boundary doesn't
     * first-touch-stall.  Camera → terrain-local via the model translation (exact
     * for placed-by-translation terrains, the common case; the lazy load in
     * terrain_h is the safety net for scaled/rotated ones).  Radius ≈ 2 tiles. */
    if (camera && jce_terrain_is_tiled(terr)) {
        int   ptx = 0, ptz = 0, ptd = 0;
        jce_terrain_tile_grid(terr, &ptx, &ptz, &ptd);
        float wsx = jce_terrain_world_size_x(terr);
        if (ptx > 0 && wsx > 0.0f) {
            float local_x = cam_pos.x - model->col[3].x;
            float local_z = cam_pos.z - model->col[3].z;
            jce_terrain_prefetch(terr, local_x, local_z, 2.0f * (wsx / (float)ptx));
        }
    }

    int ncx = sr->terrain_cache[slot].chunk_nx;
    int ncz = sr->terrain_cache[slot].chunk_nz;

    sr->stat_terrain_chunks_total   += ncx * ncz;

    for (int cz = 0; cz < ncz; cz++)
    for (int cx = 0; cx < ncx; cx++) {
        int idx = cz * ncx + cx;
        jce_vec3 lmn = sr->terrain_cache[slot].chunk_min[idx];
        jce_vec3 lmx = sr->terrain_cache[slot].chunk_max[idx];

        jce_vec3 wmn, wmx;
        sr_transform_aabb(model, lmn, lmx, &wmn, &wmx);

        const bool displaced = sr_terrain_chunk_snow_geometry(&snow, snow_geometry, wmn, wmx);
        jce_vec3 cull_min = wmn, cull_max = wmx;
        if (displaced) { cull_min.y -= SR_SNOW_DEPTH_MAX_M; cull_max.y += SR_SNOW_RIM_MAX_M; }
        if (have_planes && !sr_aabb_in_frustum(planes, cull_min, cull_max)) {
            sr->stat_terrain_chunks_culled++;
            continue;
        }

        /* LOD from camera distance to the chunk's world-space centre. */
        jce_vec3 ctr = { 0.5f * (wmn.x + wmx.x),
                         0.5f * (wmn.y + wmx.y),
                         0.5f * (wmn.z + wmx.z) };
        float dx = ctr.x - cam_pos.x;
        float dy = ctr.y - cam_pos.y;
        float dz = ctr.z - cam_pos.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        int lod = (int)(dist / SR_TERRAIN_LOD_DIST);
        if (lod < 0) lod = 0;
        if (lod > SR_TERRAIN_MAX_LOD) lod = SR_TERRAIN_MAX_LOD;
        if (displaced) lod = 0;

        JceMesh *cm = sr_terrain_chunk_mesh(sr, slot, cx, cz, lod);
        if (!cm) {
            /* Fall back to LOD 0 if the requested LOD collapsed (tiny chunk). */
            if (lod != 0) cm = sr_terrain_chunk_mesh(sr, slot, cx, cz, 0);
            if (!cm) continue;
        }

        if (mode == JCE_SCENE_VIEW_WIREFRAME) {
            sr_draw_mesh_view_wireframe(sr, cm, view_id, model, false);
            sr->stat_terrain_chunks_drawn++;
            continue;
        }

        /* Per-chunk state (re-bound every submit; BGFX_DISCARD_ALL clears it). */
        sr_inline_bind_pbr_global_overrides(
            sr, pbr, view_id, scene, list,
            (JceTexture){ layer_tex[0].idx },
            layered ? (JceTexture){ layer_tex[3].idx } : (JceTexture){ UINT16_MAX },
            receiver_layer);
        bgfx_set_transform(model->raw[0], 1);
        if (layered) {
            bgfx_set_texture(14, sr->s_terrain_layer1, layer_tex[1], UINT32_MAX);
            bgfx_set_texture(15, sr->s_terrain_layer2, layer_tex[2], UINT32_MAX);
            sr_terrain_material_bind(sr, &layer_data);
            if (snow_look_active)
                bgfx_set_uniform(material_cache->snow_look, snow_look_data, 1);
            sr_terrain_bind_snow(sr, &snow,
                displaced ? snow_geometry : (SrTerrainSnowGeometry){0}, wmn, wmx, tparams);
            /* Stage 3 is fs_terrain.sc's s_cloudShadow -- the one stage terrain
             * could free, because it held a declared-but-never-bound s_aoMap.
             * Bound here rather than with the shared shadow state: the mesh
             * shader uses stage 3 for a real AO map, so a global bind would
             * overwrite it. This is terrain's ONLY draw path. */
            sr_bind_cloud_shadow(sr);
            /* Stage 1 is fs_terrain.sc's s_terrainAO -- the screen-space AO target.
             * Bound after the material bind, which puts a metallic-roughness map
             * there that this shader has never read. White when SSAO did not run,
             * because an unbound sampler is undefined the moment something reads
             * it, and the shader's own u_ssaoParams.x gate is what actually
             * decides whether the value is used. */
            {
                bgfx_texture_handle_t ao_h = sr->white_tex;
                if (sr->ssao_active_frame && sr->ssao_ao_idx != UINT16_MAX)
                    ao_h.idx = sr->ssao_ao_idx;
                bgfx_set_texture(1, sr->s_terrain_ao, ao_h, UINT32_MAX);
            }

            if (per_tile_splat) {
                /* Bind this chunk's tile splat texture + remap the global terrain UV
                 * into the tile's local [0..1]. */
                bgfx_texture_handle_t st =
                    sr_terrain_tile_splat_tex(sr, slot, cx, cz, tg_dim);
                bgfx_set_texture(13, sr->s_terrain_splat,
                    BGFX_HANDLE_IS_VALID(st) ? st : sr->white_tex, UINT32_MAX);
                float tile_uv[4] = { (float)cx / (float)tg_x,
                                     (float)cz / (float)tg_z,
                                     (float)tg_x, (float)tg_z };
                bgfx_set_uniform(sr->u_terrain_tile_uv, tile_uv, 1);
            } else {
                bgfx_set_texture(13, sr->s_terrain_splat, splat_h, UINT32_MAX);
                float tile_uv[4] = { 0.0f, 0.0f, 1.0f, 1.0f };  /* identity remap */
                bgfx_set_uniform(sr->u_terrain_tile_uv, tile_uv, 1);
            }
        }

        jce_mesh_submit_terrain(cm, sr->renderer, view_id);
        if (mode == JCE_SCENE_VIEW_WIREFRAME_TEXTURED)
            sr_draw_mesh_view_wireframe(sr, cm, view_id, model, true);
        sr->stat_terrain_chunks_drawn++;
    }
}

/* If entity e is a terrain, submit its chunks as depth-only shadow casters
 * into `view_id` and return true (so the shadow loop skips the generic mesh
 * path).  Mirrors sr_try_submit_mesh_renderer_model_shadow.  Reuses any chunk
 * mesh already cached by the colour pass; otherwise builds it at the coarse
 * shadow LOD.  Returns false for non-terrain entities. */
bool sr_try_submit_terrain_shadow(JceSceneRenderer *sr, JceScene *scene,
                                  JceEntity e, uint16_t view_id,
                                  const jce_mat4 *cull_vp)
{
    sr->stat_terrain_shadow_calls++;
    if (!jce_scene_has_terrain(scene, e)) return false;
    JceTerrainComponent *tc = jce_scene_get_terrain(scene, e);
    if (!tc || !tc->visible || !tc->terrain_path[0]) return false;
    int slot = sr_terrain_find_or_load_slot(sr, scene, tc->terrain_path);
    if (slot < 0 || sr->terrain_cache[slot].chunk_count <= 0) return false;
    if (!jce_scene_has_transform(scene, e)) return false;

    jce_mat4 model = jce_scene_get_world_matrix(scene, e);
    SrTerrainSnowMarkers snow = {0};
    const SrTerrainSnowGeometry snow_geometry = sr_terrain_collect_geometry(sr, scene, e,
        sr->terrain_cache[slot].terrain, &model, &snow);
    int ncx = sr->terrain_cache[slot].chunk_nx;
    int ncz = sr->terrain_cache[slot].chunk_nz;

    /* Cull against the shadow view's own frustum.
     *
     * This loop had no test at all: every chunk went into every cascade and
     * every local-light view. The caller's per-caster cull does not cover it
     * -- sr_shadow_caster_aabb returns false for terrain by design ("Returns
     * false for primitives/terrain/unresolvable"), so `has_aabb` is false and
     * the cascade reject never fires for a terrain entity. The shipped
     * hidden_cove is 16x16 chunks, so that was 256 depth draws per cascade,
     * 1024 a frame, against a colour pass that draws a few dozen.
     *
     * The planes come from the caller because only the caller knows which
     * cascade this view id is. NULL keeps the old behaviour for the paths
     * that have no matrix to give (the single-map legacy path, local lights),
     * which is correct rather than merely compatible: a cull with the wrong
     * frustum removes shadows that should be there. */
    jce_vec4 splanes[6];
    const bool scull = (cull_vp != NULL);
    if (scull) sr_extract_frustum_planes(cull_vp, splanes);

    for (int cz = 0; cz < ncz; cz++)
    for (int cx = 0; cx < ncx; cx++) {
        int cidx = cz * ncx + cx;
        jce_vec3 wmn = {0}, wmx = {0};
        if (scull || snow_geometry.snow_amount > 0.0f)
            sr_transform_aabb(&model, sr->terrain_cache[slot].chunk_min[cidx],
                              sr->terrain_cache[slot].chunk_max[cidx], &wmn, &wmx);
        const bool displaced = sr_terrain_chunk_snow_geometry(&snow, snow_geometry, wmn, wmx);
        if (scull) {
            jce_vec3 cull_min = wmn, cull_max = wmx;
            if (displaced) { cull_min.y -= SR_SNOW_DEPTH_MAX_M; cull_max.y += SR_SNOW_RIM_MAX_M; }
            if (!sr_aabb_in_frustum(splanes, cull_min, cull_max)) {
                sr->stat_terrain_shadow_culled++;
                continue;
            }
        }
        sr->stat_terrain_shadow_drawn++;
        JceMesh *cm = displaced ? sr_terrain_chunk_mesh(sr, slot, cx, cz, 0)
                                : sr->terrain_cache[slot].chunk_meshes[cidx];
        if (!cm && !displaced) cm = sr_terrain_chunk_mesh(sr, slot, cx, cz, SR_TERRAIN_SHADOW_LOD);
        if (!cm) continue;
        bgfx_set_transform(model.raw[0], 1);
        if (!displaced) {
            jce_mesh_submit_shadow(cm, sr->renderer, view_id);
            continue;
        }
        float params[4] = {0};
        sr_terrain_bind_snow(sr, &snow, snow_geometry, wmn, wmx, params);
        const bgfx_vertex_buffer_handle_t vb = {(uint16_t)jce_mesh_get_vbh(cm)};
        const bgfx_index_buffer_handle_t ib = {(uint16_t)jce_mesh_get_ibh(cm)};
        jce_enc_set_vertex_buffer(0, vb, 0, jce_mesh_vertex_count(cm));
        if (BGFX_HANDLE_IS_VALID(ib))
            jce_enc_set_index_buffer(ib, 0, jce_mesh_index_count(cm));
        jce_enc_set_state(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                          BGFX_STATE_CULL_CW | BGFX_STATE_MSAA, 0);
        jce_enc_submit(view_id, sr->terrain_material_cache->snow_depth, 0, BGFX_DISCARD_ALL);
    }
    return true;
}
