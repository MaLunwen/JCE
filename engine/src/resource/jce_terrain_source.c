/*
 * jce_terrain_source.c -- v3-file-backed tile loader for the terrain store.
 * See jce_terrain_source.h for the contract.
 */

#include "jce_terrain_source.h"
#include "jce_terrain_format.h"

#include "os/core/jce_memory.h"

#include <jce/os/core/jce_filesystem.h>

#include <string.h>

struct JceTerrainSource {
    JceTerrainHeader    header;
    JceTerrainDirEntry *entries;      /* header.tile_count entries */
    const uint8_t      *data;         /* borrowed or owned, see owns_data */
    size_t              size;
    bool                owns_data;
};

static JceTerrainSource *src_from_bytes(const void *data, size_t size,
                                        bool take_ownership)
{
    if (!data || size == 0u) return NULL;

    JceTerrainSource *s =
        (JceTerrainSource *)JCE_CALLOC(1, sizeof(JceTerrainSource));
    if (!s) return NULL;

    s->data      = (const uint8_t *)data;
    s->size      = size;
    s->owns_data = take_ownership;

    /* Validate up front.  A caller that successfully acquires a handle must
     * not discover at the first pin that the asset was never readable. */
    if (jce_terrain_read_header(data, size, &s->header) != JCE_TERRAIN_OK)
        goto fail;
    if (s->header.tile_count == 0u) goto fail;

    s->entries = (JceTerrainDirEntry *)JCE_CALLOC(
        s->header.tile_count, sizeof(JceTerrainDirEntry));
    if (!s->entries) goto fail;

    if (jce_terrain_read_directory(data, size, &s->header, s->entries) !=
        JCE_TERRAIN_OK)
        goto fail;

    return s;

fail:
    JCE_FREE(s->entries);
    if (take_ownership) JCE_FREE((void *)data);
    JCE_FREE(s);
    return NULL;
}

JceTerrainSource *jce_terrain_source_open_memory(const void *data, size_t size)
{
    return src_from_bytes(data, size, /*take_ownership=*/false);
}

JceTerrainSource *jce_terrain_source_open(const struct JceFileSystem *fs,
                                          const char *virtual_path)
{
    if (!virtual_path || !virtual_path[0]) return NULL;

    uint64_t bytes = 0u;
    void *buf = jce_fs_read_all(fs, virtual_path, &bytes);
    if (!buf) return NULL;
    if (bytes == 0u || bytes > (uint64_t)(size_t)-1) {
        JCE_FREE(buf);
        return NULL;
    }
    return src_from_bytes(buf, (size_t)bytes, /*take_ownership=*/true);
}

void jce_terrain_source_close(JceTerrainSource *s)
{
    if (!s) return;
    JCE_FREE(s->entries);
    if (s->owns_data) JCE_FREE((void *)s->data);
    JCE_FREE(s);
}

uint32_t jce_terrain_source_tiles_x(const JceTerrainSource *s)
{
    return s ? s->header.tile_count_x : 0u;
}
uint32_t jce_terrain_source_tiles_z(const JceTerrainSource *s)
{
    return s ? s->header.tile_count_z : 0u;
}
float jce_terrain_source_base_height(const JceTerrainSource *s)
{
    return s ? s->header.base_height : 0.0f;
}
float jce_terrain_source_height_range(const JceTerrainSource *s)
{
    return s ? s->header.height_range : 0.0f;
}
uint8_t jce_terrain_source_diagonal_rule(const JceTerrainSource *s)
{
    return s ? s->header.diagonal_rule : (uint8_t)JCE_TERRAIN_DIAG_FIXED;
}

bool jce_terrain_source_load_tile(void *ctx, const char *virtual_path,
                                  JceTerrainTileKey key,
                                  float **out_heights,
                                  uint32_t *out_w, uint32_t *out_h)
{
    (void)virtual_path;   /* the source is already bound to one file */
    JceTerrainSource *s = (JceTerrainSource *)ctx;
    if (!s || !out_heights || !out_w || !out_h) return false;

    const JceTerrainDirEntry *e = NULL;
    for (uint32_t i = 0; i < s->header.tile_count; i++) {
        if (s->entries[i].tile_x == key.x && s->entries[i].tile_z == key.z) {
            e = &s->entries[i];
            break;
        }
    }
    if (!e) return false;   /* no such tile: reported, never faked */

    const uint64_t count =
        (uint64_t)e->sample_width * (uint64_t)e->sample_height;
    if (count == 0u || count > 0x4000000ull) return false;

    /* Engine allocator: the store frees this with JCE_FREE. */
    float *heights = (float *)JCE_MALLOC((size_t)count * sizeof(float));
    if (!heights) return false;

    if (jce_terrain_read_tile_heights(s->data, s->size, &s->header, e,
                                      heights) != JCE_TERRAIN_OK) {
        /* A hash or range failure must not publish a half-decoded tile. */
        JCE_FREE(heights);
        return false;
    }

    *out_heights = heights;
    *out_w       = e->sample_width;
    *out_h       = e->sample_height;
    return true;
}
