/*
 * jce_decals.c -- runtime surface-decal pool implementation.
 *
 * Each live decal is rendered as a single textured quad oriented by
 * its surface normal at hit point.  We rebuild a transient vertex
 * buffer once per frame containing all live decals, then submit a
 * single indexed draw with the decal program.
 *
 * Memory: O(max_decals) host slots; per-frame TVB scales with live
 * count.
 */

#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/renderer/jce_decals.h>
#include <jce/renderer/jce_shaders.h>   /* jce_shaders_embedded_engine_pak fallback */

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "decals"

typedef struct {
    jce_vec3   position;
    jce_vec3   normal;
    jce_vec3   tangent;
    jce_vec3   bitangent;
    float      half_size;
    float      thickness;
    JceTexture texture;
    jce_vec4   tint;
    float      lifetime;        /* total seconds; 0 = persistent */
    float      age;             /* seconds since spawn */
    uint64_t   spawn_seq;       /* monotonic for LRU eviction */
    bool       alive;
} DecalSlot;

struct JceDecalPool {
    DecalSlot                  *slots;
    uint32_t                    capacity;
    uint64_t                    spawn_counter;

    /* Shared GPU resources. */
    bgfx_vertex_layout_t        layout;
    bgfx_program_handle_t       program;
    bgfx_uniform_handle_t       s_decal;
};

/* ---- shader loading (mirrors jce_gpu_particles.c) -------------------- */

static const char *decal_backend_suffix(void)
{
    switch (bgfx_get_renderer_type()) {
    case BGFX_RENDERER_TYPE_DIRECT3D11:
    case BGFX_RENDERER_TYPE_DIRECT3D12: return "dx11";
    case BGFX_RENDERER_TYPE_VULKAN:     return "spv";
    case BGFX_RENDERER_TYPE_OPENGL:     return "glsl";
    case BGFX_RENDERER_TYPE_OPENGLES:   return "essl";
    case BGFX_RENDERER_TYPE_METAL:      return "mtl";
    default:                            return NULL;
    }
}

static bgfx_shader_handle_t decal_load_shader(const JcePakArchive *pak,
                                                const char *name,
                                                const char *sfx)
{
    bgfx_shader_handle_t invalid = { UINT16_MAX };
    char path[256];
    snprintf(path, sizeof(path), "shaders/%s_%s.bin", name, sfx);

    /* The scene/editor PAK rarely carries engine shaders (the editor ships
     * editor_assets.pak with zero shaders; engine shaders are baked into
     * jce_renderer).  Mirror load_single(): try the caller pak, then fall
     * back to the embedded engine-shader pak. */
    const JcePakAsset *asset = pak ? jce_pak_find(pak, path) : NULL;
    if (!asset) {
        const JcePakArchive *fb = jce_shaders_embedded_engine_pak();
        if (fb && fb != pak) {
            asset = jce_pak_find(fb, path);
            if (asset) pak = fb;
        }
    }
    if (!asset) {
        LOG_ERROR(LOG_TAG, "shader not found in pak: %s", path);
        return invalid;
    }
    void *buf = JCE_MALLOC((size_t)asset->original_size);
    if (!buf) return invalid;
    size_t n = jce_pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) { JCE_FREE(buf); return invalid; }
    const bgfx_memory_t *mem = bgfx_copy(buf, (uint32_t)asset->original_size);
    JCE_FREE(buf);
    return bgfx_create_shader(mem);
}

/* ---- pool create/destroy --------------------------------------------- */

JceDecalPool *jce_decals_create(const JceDecalPoolDesc *desc)
{
    if (!desc || !desc->pak || desc->max_decals == 0) return NULL;

    const char *sfx = decal_backend_suffix();
    if (!sfx) {
        LOG_ERROR(LOG_TAG, "unsupported renderer backend");
        return NULL;
    }

    JceDecalPool *pool = (JceDecalPool *)JCE_CALLOC(1, sizeof(JceDecalPool));
    if (!pool) return NULL;

    pool->capacity = desc->max_decals < 16u ? 16u : desc->max_decals;
    pool->slots = (DecalSlot *)JCE_CALLOC(pool->capacity, sizeof(DecalSlot));
    if (!pool->slots) { JCE_FREE(pool); return NULL; }

    /* Layout: vec3 pos + vec2 uv + vec4 colour. */
    bgfx_vertex_layout_begin(&pool->layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&pool->layout, BGFX_ATTRIB_POSITION,  3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&pool->layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&pool->layout, BGFX_ATTRIB_COLOR0,    4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&pool->layout);

    bgfx_shader_handle_t vsh = decal_load_shader(desc->pak, "vs_decal", sfx);
    bgfx_shader_handle_t fsh = decal_load_shader(desc->pak, "fs_decal", sfx);
    if (vsh.idx == UINT16_MAX || fsh.idx == UINT16_MAX) {
        if (vsh.idx != UINT16_MAX) bgfx_destroy_shader(vsh);
        if (fsh.idx != UINT16_MAX) bgfx_destroy_shader(fsh);
        JCE_FREE(pool->slots); JCE_FREE(pool);
        return NULL;
    }
    pool->program = bgfx_create_program(vsh, fsh, true);
    pool->s_decal = bgfx_create_uniform("s_decal", BGFX_UNIFORM_TYPE_SAMPLER, 1);

    return pool;
}

void jce_decals_destroy(JceDecalPool *pool)
{
    if (!pool) return;
    if (pool->program.idx != UINT16_MAX)  bgfx_destroy_program(pool->program);
    if (pool->s_decal.idx != UINT16_MAX)  bgfx_destroy_uniform(pool->s_decal);
    JCE_FREE(pool->slots);
    JCE_FREE(pool);
}

/* ---- spawn / update -------------------------------------------------- */

static int decal_pick_slot(JceDecalPool *pool)
{
    /* Prefer dead slot, else oldest live (LRU). */
    int dead = -1;
    int oldest = 0;
    uint64_t oldest_seq = UINT64_MAX;
    for (uint32_t i = 0; i < pool->capacity; i++) {
        if (!pool->slots[i].alive) { dead = (int)i; break; }
        if (pool->slots[i].spawn_seq < oldest_seq) {
            oldest_seq = pool->slots[i].spawn_seq;
            oldest = (int)i;
        }
    }
    return dead >= 0 ? dead : oldest;
}

static jce_vec3 decal_safe_normalize(jce_vec3 v, jce_vec3 fallback)
{
    float l = sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
    if (l < 1e-5f) return fallback;
    return jce_v3(v.x/l, v.y/l, v.z/l);
}

bool jce_decals_spawn(JceDecalPool *pool, const JceDecalSpawn *spawn)
{
    if (!pool || !spawn) return false;
    if (spawn->size <= 0.0f) return false;
    if (spawn->texture.idx == UINT16_MAX) return false;

    int idx = decal_pick_slot(pool);
    DecalSlot *s = &pool->slots[idx];

    s->position  = spawn->position;
    s->normal    = decal_safe_normalize(spawn->normal, jce_v3(0, 1, 0));

    /* Build tangent: prefer hint, else pick a stable axis perpendicular
     * to the normal. */
    jce_vec3 tan = spawn->tangent_hint;
    if (sqrtf(tan.x*tan.x + tan.y*tan.y + tan.z*tan.z) < 1e-4f) {
        jce_vec3 up = (fabsf(s->normal.y) > 0.95f) ? jce_v3(1, 0, 0)
                                                    : jce_v3(0, 1, 0);
        tan = jce_v3_cross(up, s->normal);
    }
    /* Ensure tangent is perpendicular to normal. */
    float dot_tn = tan.x*s->normal.x + tan.y*s->normal.y + tan.z*s->normal.z;
    tan = jce_v3(tan.x - s->normal.x * dot_tn,
                 tan.y - s->normal.y * dot_tn,
                 tan.z - s->normal.z * dot_tn);
    s->tangent   = decal_safe_normalize(tan, jce_v3(1, 0, 0));
    s->bitangent = jce_v3_cross(s->normal, s->tangent);

    s->half_size = spawn->size * 0.5f;
    s->thickness = spawn->thickness > 0.0f ? spawn->thickness : 0.005f;
    s->texture   = spawn->texture;
    s->tint      = spawn->tint;
    s->lifetime  = spawn->lifetime_seconds;
    s->age       = 0.0f;
    s->spawn_seq = ++pool->spawn_counter;
    s->alive     = true;
    return true;
}

void jce_decals_update(JceDecalPool *pool, float dt)
{
    if (!pool || dt <= 0.0f) return;
    JCE_PROFILE_ZONE_N("Decals::Update");
    for (uint32_t i = 0; i < pool->capacity; i++) {
        DecalSlot *s = &pool->slots[i];
        if (!s->alive) continue;
        if (s->lifetime <= 0.0f) continue; /* persistent */
        s->age += dt;
        if (s->age >= s->lifetime) {
            s->alive = false;
        }
    }
    JCE_PROFILE_ZONE_END;
}

void jce_decals_clear(JceDecalPool *pool)
{
    if (!pool) return;
    for (uint32_t i = 0; i < pool->capacity; i++) pool->slots[i].alive = false;
}

uint32_t jce_decals_capacity(const JceDecalPool *pool)
{
    return pool ? pool->capacity : 0;
}

uint32_t jce_decals_live_count(const JceDecalPool *pool)
{
    if (!pool) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < pool->capacity; i++) {
        if (pool->slots[i].alive) n++;
    }
    return n;
}

/* ---- render ---------------------------------------------------------- */

typedef struct {
    float pos[3];
    float uv[2];
    float color[4];
} DecalVertex;

void jce_decals_render(JceDecalPool *pool, uint16_t view_id)
{
    if (!pool || pool->program.idx == UINT16_MAX) return;

    uint32_t live = jce_decals_live_count(pool);
    if (live == 0) return;

    JCE_PROFILE_ZONE_N("Decals::Render");

    const uint32_t verts_per   = 4;
    const uint32_t indices_per = 6;
    const uint32_t total_v     = live * verts_per;
    const uint32_t total_i     = live * indices_per;

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;
    if (!bgfx_alloc_transient_buffers(&tvb, &pool->layout, total_v,
                                       &tib, total_i, false))
    {
        LOG_WARN(LOG_TAG, "transient buffer alloc failed for %u verts", total_v);
        JCE_PROFILE_ZONE_END;
        return;
    }

    DecalVertex *vbuf = (DecalVertex *)tvb.data;
    uint16_t    *ibuf = (uint16_t    *)tib.data;

    uint32_t cur = 0;

    /* Group decals by texture for batching across draws. */
    /* Simple two-pass: first emit geometry for all live decals, then
     * group-by-texture and submit per-batch.  For now we submit one draw
     * per live decal — capacity is small and this keeps the code simple. */
    uint32_t emit = 0;
    for (uint32_t i = 0; i < pool->capacity && emit < live; i++) {
        DecalSlot *s = &pool->slots[i];
        if (!s->alive) continue;

        /* Fade alpha as decal nears end of life. */
        float fade = 1.0f;
        if (s->lifetime > 0.0f) {
            float t = s->age / s->lifetime;
            if (t > 0.75f) fade = 1.0f - (t - 0.75f) * 4.0f;
            if (fade < 0.0f) fade = 0.0f;
        }
        float r = s->tint.x;
        float g = s->tint.y;
        float b = s->tint.z;
        float a = s->tint.w * fade;

        /* Quad corners in surface frame, offset along normal to avoid
         * z-fight. */
        jce_vec3 c = jce_v3(
            s->position.x + s->normal.x * s->thickness,
            s->position.y + s->normal.y * s->thickness,
            s->position.z + s->normal.z * s->thickness);

        jce_vec3 t = jce_v3_scale(s->tangent,   s->half_size);
        jce_vec3 b3= jce_v3_scale(s->bitangent, s->half_size);

        /* 4 corners: -t-b, +t-b, +t+b, -t+b */
        jce_vec3 p0 = jce_v3(c.x - t.x - b3.x, c.y - t.y - b3.y, c.z - t.z - b3.z);
        jce_vec3 p1 = jce_v3(c.x + t.x - b3.x, c.y + t.y - b3.y, c.z + t.z - b3.z);
        jce_vec3 p2 = jce_v3(c.x + t.x + b3.x, c.y + t.y + b3.y, c.z + t.z + b3.z);
        jce_vec3 p3 = jce_v3(c.x - t.x + b3.x, c.y - t.y + b3.y, c.z - t.z + b3.z);

        DecalVertex *v = &vbuf[cur];
        v[0].pos[0]=p0.x; v[0].pos[1]=p0.y; v[0].pos[2]=p0.z;
        v[0].uv[0]=0.0f;  v[0].uv[1]=0.0f;
        v[1].pos[0]=p1.x; v[1].pos[1]=p1.y; v[1].pos[2]=p1.z;
        v[1].uv[0]=1.0f;  v[1].uv[1]=0.0f;
        v[2].pos[0]=p2.x; v[2].pos[1]=p2.y; v[2].pos[2]=p2.z;
        v[2].uv[0]=1.0f;  v[2].uv[1]=1.0f;
        v[3].pos[0]=p3.x; v[3].pos[1]=p3.y; v[3].pos[2]=p3.z;
        v[3].uv[0]=0.0f;  v[3].uv[1]=1.0f;
        for (int k = 0; k < 4; k++) {
            v[k].color[0] = r;
            v[k].color[1] = g;
            v[k].color[2] = b;
            v[k].color[3] = a;
        }

        ibuf[emit*6 + 0] = (uint16_t)(cur + 0);
        ibuf[emit*6 + 1] = (uint16_t)(cur + 1);
        ibuf[emit*6 + 2] = (uint16_t)(cur + 2);
        ibuf[emit*6 + 3] = (uint16_t)(cur + 0);
        ibuf[emit*6 + 4] = (uint16_t)(cur + 2);
        ibuf[emit*6 + 5] = (uint16_t)(cur + 3);

        cur += 4;
        emit++;
    }

    /* Submit one draw per decal, grouped by texture handle isn't worth
     * the complexity for typical game volumes (<= a few hundred live). */
    emit = 0;
    for (uint32_t i = 0; i < pool->capacity && emit < live; i++) {
        DecalSlot *s = &pool->slots[i];
        if (!s->alive) continue;

        bgfx_set_transient_vertex_buffer(0, &tvb, emit * 4u, 4u);
        bgfx_set_transient_index_buffer(&tib, emit * 6u, 6u);

        bgfx_texture_handle_t tex = { s->texture.idx };
        bgfx_set_texture(0, pool->s_decal, tex, UINT32_MAX);

        bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                       | BGFX_STATE_DEPTH_TEST_LEQUAL
                       | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                               BGFX_STATE_BLEND_INV_SRC_ALPHA)
                       | BGFX_STATE_MSAA, 0);

        bgfx_submit(view_id, pool->program, 0, BGFX_DISCARD_ALL);
        emit++;
    }
    JCE_PROFILE_ZONE_END;
}
