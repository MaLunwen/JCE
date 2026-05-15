/*
 * jce_tilemap.c  Chunked 2D tilemap storage.
 *
 * Chunks live in a fixed array, linearly searched by (chunk_x, chunk_y).
 * For the typical 256-chunk cap (4096x4096 tiles given 16-tile chunks)
 * this is fine; for larger worlds, swap to a hash later.
 */

#include <jce/renderer/jce_tilemap.h>
#include <jce/os/core/jce_json.h>

#include <stdio.h>
#include <string.h>

void jce_tilemap_init(JceTilemap *m, const char *atlas_path,
                       float tile_world_size)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));
    if (atlas_path) {
        strncpy(m->atlas_path, atlas_path, sizeof(m->atlas_path) - 1);
        m->atlas_path[sizeof(m->atlas_path) - 1] = '\0';
    }
    m->tile_world_size = tile_world_size > 0.0f ? tile_world_size : 1.0f;
}

void jce_tilemap_clear(JceTilemap *m)
{
    if (!m) return;
    memset(m->chunks, 0, sizeof(m->chunks));
    m->chunk_count = 0;
}

static int find_chunk(const JceTilemap *m, int32_t cx, int32_t cy)
{
    for (uint32_t i = 0; i < m->chunk_count; ++i) {
        if (!m->chunks[i].active) continue;
        if (m->chunks[i].chunk_x == cx && m->chunks[i].chunk_y == cy)
            return (int)i;
    }
    return -1;
}

static int find_or_create_chunk(JceTilemap *m, int32_t cx, int32_t cy)
{
    int existing = find_chunk(m, cx, cy);
    if (existing >= 0) return existing;
    /* Reuse first inactive slot, then append. */
    int slot = -1;
    for (uint32_t i = 0; i < m->chunk_count; ++i) {
        if (!m->chunks[i].active) { slot = (int)i; break; }
    }
    if (slot < 0) {
        if (m->chunk_count >= JCE_TILEMAP_MAX_CHUNKS) return -1;
        slot = (int)m->chunk_count++;
    }
    JceTilemapChunk *c = &m->chunks[slot];
    memset(c, 0, sizeof(*c));
    c->chunk_x = cx;
    c->chunk_y = cy;
    /* Initialise every cell to "empty". */
    for (int i = 0;
         i < JCE_TILEMAP_CHUNK_SIZE * JCE_TILEMAP_CHUNK_SIZE; ++i)
        c->cells[i].sprite_index = JCE_TILEMAP_EMPTY;
    c->active = true;
    return slot;
}

static int local_index(int32_t tx, int32_t ty)
{
    int lx = ((tx % JCE_TILEMAP_CHUNK_SIZE) + JCE_TILEMAP_CHUNK_SIZE) %
             JCE_TILEMAP_CHUNK_SIZE;
    int ly = ((ty % JCE_TILEMAP_CHUNK_SIZE) + JCE_TILEMAP_CHUNK_SIZE) %
             JCE_TILEMAP_CHUNK_SIZE;
    return ly * JCE_TILEMAP_CHUNK_SIZE + lx;
}

static void cell_coords_to_chunk(int32_t tx, int32_t ty,
                                  int32_t *cx, int32_t *cy)
{
    /* Floor division so negative coords land in the correct chunk. */
    *cx = (tx >= 0) ? (tx / JCE_TILEMAP_CHUNK_SIZE)
                    : ((tx - JCE_TILEMAP_CHUNK_SIZE + 1) / JCE_TILEMAP_CHUNK_SIZE);
    *cy = (ty >= 0) ? (ty / JCE_TILEMAP_CHUNK_SIZE)
                    : ((ty - JCE_TILEMAP_CHUNK_SIZE + 1) / JCE_TILEMAP_CHUNK_SIZE);
}

JceTileCell jce_tilemap_get(const JceTilemap *m, int32_t tx, int32_t ty)
{
    JceTileCell empty = { JCE_TILEMAP_EMPTY, 0, 0 };
    if (!m) return empty;
    int32_t cx, cy;
    cell_coords_to_chunk(tx, ty, &cx, &cy);
    int idx = find_chunk(m, cx, cy);
    if (idx < 0) return empty;
    return m->chunks[idx].cells[local_index(tx, ty)];
}

bool jce_tilemap_set(JceTilemap *m, int32_t tx, int32_t ty, JceTileCell cell)
{
    if (!m) return false;
    int32_t cx, cy;
    cell_coords_to_chunk(tx, ty, &cx, &cy);
    int idx = find_or_create_chunk(m, cx, cy);
    if (idx < 0) return false;
    m->chunks[idx].cells[local_index(tx, ty)] = cell;
    return true;
}

bool jce_tilemap_set_sprite(JceTilemap *m, int32_t tx, int32_t ty,
                             uint16_t sprite_index)
{
    JceTileCell c = { sprite_index, 0, 0 };
    return jce_tilemap_set(m, tx, ty, c);
}

bool jce_tilemap_clear_cell(JceTilemap *m, int32_t tx, int32_t ty)
{
    JceTileCell empty = { JCE_TILEMAP_EMPTY, 0, 0 };
    return jce_tilemap_set(m, tx, ty, empty);
}

uint32_t jce_tilemap_chunk_count(const JceTilemap *m)
{
    if (!m) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->chunk_count; ++i)
        if (m->chunks[i].active) n++;
    return n;
}

const JceTilemapChunk *jce_tilemap_chunk_at(const JceTilemap *m, uint32_t i)
{
    if (!m || i >= m->chunk_count) return NULL;
    return m->chunks[i].active ? &m->chunks[i] : NULL;
}

uint32_t jce_tilemap_nonempty_count(const JceTilemap *m)
{
    if (!m) return 0;
    uint32_t n = 0;
    for (uint32_t c = 0; c < m->chunk_count; ++c) {
        if (!m->chunks[c].active) continue;
        for (int i = 0;
             i < JCE_TILEMAP_CHUNK_SIZE * JCE_TILEMAP_CHUNK_SIZE; ++i)
            if (m->chunks[c].cells[i].sprite_index != JCE_TILEMAP_EMPTY) n++;
    }
    return n;
}

/* ── JSON I/O ────────────────────────────────────────────────── */

bool jce_tilemap_save_json(const JceTilemap *m, const char *path)
{
    if (!m || !path) return false;
    JceJson *root = jce_json_object();
    if (!root) return false;
    jce_json_set_string(root, "atlas",      m->atlas_path);
    jce_json_set_number(root, "tile_size",  m->tile_world_size);
    jce_json_set_number(root, "chunk_count", (double)m->chunk_count);
    /* Sparse cell list: write only non-empty entries. */
    uint32_t written = 0;
    for (uint32_t c = 0; c < m->chunk_count; ++c) {
        const JceTilemapChunk *ch = &m->chunks[c];
        if (!ch->active) continue;
        char key[40];
        snprintf(key, sizeof(key), "c%u_x", (unsigned)c);
        jce_json_set_number(root, key, ch->chunk_x);
        snprintf(key, sizeof(key), "c%u_y", (unsigned)c);
        jce_json_set_number(root, key, ch->chunk_y);
        for (int i = 0;
             i < JCE_TILEMAP_CHUNK_SIZE * JCE_TILEMAP_CHUNK_SIZE; ++i) {
            if (ch->cells[i].sprite_index == JCE_TILEMAP_EMPTY) continue;
            snprintf(key, sizeof(key), "w%u_i", written);
            jce_json_set_number(root, key, i);
            snprintf(key, sizeof(key), "w%u_c", written);
            jce_json_set_number(root, key, (double)c);
            snprintf(key, sizeof(key), "w%u_s", written);
            jce_json_set_number(root, key, ch->cells[i].sprite_index);
            snprintf(key, sizeof(key), "w%u_f", written);
            jce_json_set_number(root, key, ch->cells[i].flags);
            snprintf(key, sizeof(key), "w%u_v", written);
            jce_json_set_number(root, key, ch->cells[i].variant);
            written++;
        }
    }
    jce_json_set_number(root, "cell_count", written);
    return jce_json_write_file(path, root, true, true);
}

bool jce_tilemap_load_json(JceTilemap *m, const char *path)
{
    if (!m || !path) return false;
    JceJson *root = jce_json_parse_file(path);
    if (!root) return false;
    jce_tilemap_init(m,
                      jce_json_get_string(root, "atlas", ""),
                      (float)jce_json_get_number(root, "tile_size", 1.0));
    /* Read chunk coordinates first so we can reconstruct in same slot. */
    uint32_t chunk_count = (uint32_t)jce_json_get_number(root, "chunk_count", 0);
    if (chunk_count > JCE_TILEMAP_MAX_CHUNKS)
        chunk_count = JCE_TILEMAP_MAX_CHUNKS;
    for (uint32_t c = 0; c < chunk_count; ++c) {
        char key[40];
        snprintf(key, sizeof(key), "c%u_x", (unsigned)c);
        int32_t cx = (int32_t)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "c%u_y", (unsigned)c);
        int32_t cy = (int32_t)jce_json_get_number(root, key, 0);
        /* Touch the chunk to materialise it. */
        JceTileCell empty = { JCE_TILEMAP_EMPTY, 0, 0 };
        jce_tilemap_set(m, cx * JCE_TILEMAP_CHUNK_SIZE,
                          cy * JCE_TILEMAP_CHUNK_SIZE, empty);
    }
    uint32_t cells = (uint32_t)jce_json_get_number(root, "cell_count", 0);
    for (uint32_t w = 0; w < cells; ++w) {
        char key[40];
        snprintf(key, sizeof(key), "w%u_c", w);
        uint32_t cidx = (uint32_t)jce_json_get_number(root, key, 0);
        if (cidx >= m->chunk_count) continue;
        snprintf(key, sizeof(key), "w%u_i", w);
        int li = (int)jce_json_get_number(root, key, -1);
        if (li < 0 || li >= JCE_TILEMAP_CHUNK_SIZE * JCE_TILEMAP_CHUNK_SIZE)
            continue;
        snprintf(key, sizeof(key), "w%u_s", w);
        uint16_t sp = (uint16_t)jce_json_get_number(root, key,
                                                    JCE_TILEMAP_EMPTY);
        snprintf(key, sizeof(key), "w%u_f", w);
        uint8_t fl = (uint8_t)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "w%u_v", w);
        uint8_t va = (uint8_t)jce_json_get_number(root, key, 0);
        m->chunks[cidx].cells[li].sprite_index = sp;
        m->chunks[cidx].cells[li].flags        = fl;
        m->chunks[cidx].cells[li].variant      = va;
    }
    jce_json_free(root);
    return true;
}
