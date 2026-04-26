/*
 * jce_sprite_batch.c  Batched 2D sprite renderer.
 *
 * Collects textured quads, sorts by texture then sort_key,
 * and flushes using transient vertex/index buffers.
 */

#include <jce/renderer/jce_sprite_batch.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/os/core/jce_log.h>
#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <string.h>
#include <stdlib.h>

#define LOG_TAG "sprite_batch"

/* ================================================================== */
/* Vertex layout                                                       */
/* ================================================================== */

typedef struct {
    float    x, y, z;
    uint32_t abgr;
    float    u, v;
} SpriteBatchVertex;

/* ================================================================== */
/* Sprite entry (queued quad)                                          */
/* ================================================================== */

typedef struct {
    bgfx_texture_handle_t texture;
    SpriteBatchVertex     verts[4];
    int32_t               sort_key;
} SpriteEntry;

/* ================================================================== */
/* Batch struct                                                        */
/* ================================================================== */

struct JceSpriteBatch {
    SpriteEntry *entries;
    uint32_t     count;
    uint32_t     capacity;
    bgfx_vertex_layout_t layout;
    bgfx_uniform_handle_t u_texture;
    bool         layout_ready;
};

/* ================================================================== */
/* Create / Destroy                                                    */
/* ================================================================== */

JceSpriteBatch *jce_sprite_batch_create(uint32_t max_sprites)
{
    if (max_sprites == 0) max_sprites = 512;

    JceSpriteBatch *b = (JceSpriteBatch *)JCE_CALLOC(1, sizeof(*b));
    if (!b) return NULL;

    b->entries  = (SpriteEntry *)JCE_CALLOC(max_sprites, sizeof(SpriteEntry));
    if (!b->entries) { JCE_FREE(b); return NULL; }

    b->capacity = max_sprites;
    b->count    = 0;

    b->u_texture = bgfx_create_uniform("s_texColor",
                                         BGFX_UNIFORM_TYPE_SAMPLER, 1);

    return b;
}

void jce_sprite_batch_destroy(JceSpriteBatch *batch)
{
    if (!batch) return;
    if (BGFX_HANDLE_IS_VALID(batch->u_texture))
        bgfx_destroy_uniform(batch->u_texture);
    JCE_FREE(batch->entries);
    JCE_FREE(batch);
}

/* ================================================================== */
/* Begin / Add                                                         */
/* ================================================================== */

void jce_sprite_batch_begin(JceSpriteBatch *batch)
{
    if (!batch) return;
    batch->count = 0;
}

void jce_sprite_batch_add(JceSpriteBatch *batch,
                           JceTexture texture,
                           const float *world,
                           float u0, float v0, float u1, float v1,
                           uint32_t color, int32_t sort_key)
{
    if (!batch || !world || batch->count >= batch->capacity) return;

    SpriteEntry *e = &batch->entries[batch->count++];
    e->texture  = (bgfx_texture_handle_t){ texture.idx };
    e->sort_key = sort_key;

    /* Local quad corners (centered, unit size). */
    static const float local_pos[4][3] = {
        { -0.5f, -0.5f, 0.0f },
        {  0.5f, -0.5f, 0.0f },
        {  0.5f,  0.5f, 0.0f },
        { -0.5f,  0.5f, 0.0f },
    };

    float uv[4][2] = {
        { u0, v1 }, { u1, v1 }, { u1, v0 }, { u0, v0 }
    };

    /* Transform local positions by the 4x4 world matrix (column-major). */
    for (int i = 0; i < 4; i++) {
        float lx = local_pos[i][0];
        float ly = local_pos[i][1];
        float lz = local_pos[i][2];

        e->verts[i].x = world[0]*lx + world[4]*ly + world[8]*lz  + world[12];
        e->verts[i].y = world[1]*lx + world[5]*ly + world[9]*lz  + world[13];
        e->verts[i].z = world[2]*lx + world[6]*ly + world[10]*lz + world[14];
        e->verts[i].abgr = color;
        e->verts[i].u = uv[i][0];
        e->verts[i].v = uv[i][1];
    }
}

/* ================================================================== */
/* Sort comparator                                                     */
/* ================================================================== */

static int sprite_cmp(const void *a, const void *b)
{
    const SpriteEntry *sa = (const SpriteEntry *)a;
    const SpriteEntry *sb = (const SpriteEntry *)b;

    /* Sort by texture first, then by sort_key. */
    if (sa->texture.idx != sb->texture.idx)
        return (int)sa->texture.idx - (int)sb->texture.idx;
    return sa->sort_key - sb->sort_key;
}

/* ================================================================== */
/* Flush                                                               */
/* ================================================================== */

void jce_sprite_batch_flush(JceSpriteBatch *batch,
                             const JceRenderer *renderer,
                             uint16_t view_id)
{
    if (!batch || batch->count == 0) return;

    /* Ensure vertex layout is initialized. */
    if (!batch->layout_ready) {
        bgfx_vertex_layout_begin(&batch->layout, bgfx_get_renderer_type());
        bgfx_vertex_layout_add(&batch->layout, BGFX_ATTRIB_POSITION, 3,
                               BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&batch->layout, BGFX_ATTRIB_COLOR0, 4,
                               BGFX_ATTRIB_TYPE_UINT8, true, false);
        bgfx_vertex_layout_add(&batch->layout, BGFX_ATTRIB_TEXCOORD0, 2,
                               BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_end(&batch->layout);
        batch->layout_ready = true;
    }

    /* Sort entries for batching. */
    qsort(batch->entries, batch->count, sizeof(SpriteEntry), sprite_cmp);

    /* Get the textured program from the renderer. */
    JceShaderHandle sh = jce_renderer_get_program_mesh(renderer);
    bgfx_program_handle_t prog;
    prog.idx = sh.idx;
    if (!BGFX_HANDLE_IS_VALID(prog)) return;

    /* Emit draw calls per texture batch. */
    uint32_t batch_start = 0;
    while (batch_start < batch->count) {
        bgfx_texture_handle_t cur_tex = batch->entries[batch_start].texture;
        uint32_t batch_end = batch_start + 1;
        while (batch_end < batch->count &&
               batch->entries[batch_end].texture.idx == cur_tex.idx)
            batch_end++;

        uint32_t sprite_count = batch_end - batch_start;
        uint32_t vert_count = sprite_count * 4;
        uint32_t idx_count  = sprite_count * 6;

        bgfx_transient_vertex_buffer_t tvb;
        bgfx_transient_index_buffer_t  tib;
        if (!bgfx_alloc_transient_buffers(&tvb, &batch->layout,
                                           vert_count, &tib, idx_count, false)) {
            batch_start = batch_end;
            continue;
        }

        SpriteBatchVertex *verts = (SpriteBatchVertex *)tvb.data;
        uint16_t *indices = (uint16_t *)tib.data;

        for (uint32_t i = 0; i < sprite_count; i++) {
            const SpriteEntry *se = &batch->entries[batch_start + i];
            uint32_t vi = i * 4;
            memcpy(&verts[vi], se->verts, 4 * sizeof(SpriteBatchVertex));

            uint32_t ii = i * 6;
            indices[ii + 0] = (uint16_t)(vi + 0);
            indices[ii + 1] = (uint16_t)(vi + 1);
            indices[ii + 2] = (uint16_t)(vi + 2);
            indices[ii + 3] = (uint16_t)(vi + 0);
            indices[ii + 4] = (uint16_t)(vi + 2);
            indices[ii + 5] = (uint16_t)(vi + 3);
        }

        bgfx_set_transient_vertex_buffer(0, &tvb, 0, vert_count);
        bgfx_set_transient_index_buffer(&tib, 0, idx_count);

        bgfx_set_texture(0, batch->u_texture, cur_tex, UINT32_MAX);

        uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                       | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                       | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA;
        bgfx_set_state(state, 0);

        jce_mat4 identity;
        memset(&identity, 0, sizeof(identity));
        identity.raw[0][0] = 1.0f;
        identity.raw[1][1] = 1.0f;
        identity.raw[2][2] = 1.0f;
        identity.raw[3][3] = 1.0f;
        bgfx_set_transform(identity.raw[0], 1);

        bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);

        batch_start = batch_end;
    }
}

uint32_t jce_sprite_batch_count(const JceSpriteBatch *batch)
{
    return batch ? batch->count : 0;
}
