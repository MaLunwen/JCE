/*
 * jce_lowlevel.c  Implementation of public low-level renderer wrappers.
 *
 * All functions defined in jce_lowlevel.h forward to bgfx; this is the
 * single TU that owns the JCE <-> bgfx ABI boundary.  The public types
 * are POD — we either reinterpret-cast (for layout-compatible structs
 * such as JceVertexLayout / transient buffers) or do a tiny manual copy
 * (for stats subsets).
 */

#include <jce/renderer/jce_lowlevel.h>
#include <jce/os/core/jce_log.h>

#include <bgfx/c99/bgfx.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>

#define LOG_TAG "jce_gfx"

/* ── Compile-time ABI sanity ─────────────────────────────────────── */

static_assert(sizeof(JceVertexLayout) >= sizeof(bgfx_vertex_layout_t),
               "JceVertexLayout opaque storage too small for bgfx_vertex_layout_t");

static_assert((int)JCE_ATTRIB_POSITION  == (int)BGFX_ATTRIB_POSITION,  "attrib enum mismatch");
static_assert((int)JCE_ATTRIB_COLOR0    == (int)BGFX_ATTRIB_COLOR0,    "attrib enum mismatch");
static_assert((int)JCE_ATTRIB_TEXCOORD0 == (int)BGFX_ATTRIB_TEXCOORD0, "attrib enum mismatch");
static_assert((int)JCE_ATTRIB_TYPE_FLOAT == (int)BGFX_ATTRIB_TYPE_FLOAT, "attrib type mismatch");
static_assert((int)JCE_ATTRIB_TYPE_UINT8 == (int)BGFX_ATTRIB_TYPE_UINT8, "attrib type mismatch");

static_assert((uint64_t)JCE_STATE_WRITE_RGB == (uint64_t)BGFX_STATE_WRITE_RGB, "state mismatch");
static_assert((uint64_t)JCE_STATE_WRITE_A   == (uint64_t)BGFX_STATE_WRITE_A,   "state mismatch");
static_assert((uint64_t)JCE_STATE_MSAA      == (uint64_t)BGFX_STATE_MSAA,      "state mismatch");
static_assert((uint64_t)JCE_STATE_DEPTH_TEST_LESS   == (uint64_t)BGFX_STATE_DEPTH_TEST_LESS,   "state mismatch");
static_assert((uint64_t)JCE_STATE_DEPTH_TEST_LEQUAL == (uint64_t)BGFX_STATE_DEPTH_TEST_LEQUAL, "state mismatch");
static_assert((uint64_t)JCE_BLEND_ONE           == (uint64_t)BGFX_STATE_BLEND_ONE,           "blend mismatch");
static_assert((uint64_t)JCE_BLEND_SRC_ALPHA     == (uint64_t)BGFX_STATE_BLEND_SRC_ALPHA,     "blend mismatch");
static_assert((uint64_t)JCE_BLEND_INV_SRC_ALPHA == (uint64_t)BGFX_STATE_BLEND_INV_SRC_ALPHA, "blend mismatch");
static_assert(JCE_DISCARD_ALL == BGFX_DISCARD_ALL, "discard mismatch");
static_assert((int)JCE_UNIFORM_TYPE_VEC4 == (int)BGFX_UNIFORM_TYPE_VEC4, "uniform type mismatch");
static_assert((int)JCE_TEXTURE_FORMAT_RGBA8 == (int)BGFX_TEXTURE_FORMAT_RGBA8, "texture format mismatch");

static_assert(sizeof(JceTransientVertexBuffer) == sizeof(bgfx_transient_vertex_buffer_t),
               "JceTransientVertexBuffer layout drift");
static_assert(sizeof(JceTransientIndexBuffer) == sizeof(bgfx_transient_index_buffer_t),
               "JceTransientIndexBuffer layout drift");

/* ── Vertex layout ─────────────────────────────────────────────── */

void jce_vertex_layout_begin(JceVertexLayout *layout)
{
    bgfx_vertex_layout_begin((bgfx_vertex_layout_t *)layout, bgfx_get_renderer_type());
}

void jce_vertex_layout_add(JceVertexLayout *layout,
                           JceAttrib attrib, uint8_t num,
                           JceAttribType type, bool normalized, bool as_int)
{
    bgfx_vertex_layout_add((bgfx_vertex_layout_t *)layout,
                           (bgfx_attrib_t)attrib, num,
                           (bgfx_attrib_type_t)type, normalized, as_int);
}

void jce_vertex_layout_end(JceVertexLayout *layout)
{
    bgfx_vertex_layout_end((bgfx_vertex_layout_t *)layout);
}

uint16_t jce_vertex_layout_stride(const JceVertexLayout *layout)
{
    return ((const bgfx_vertex_layout_t *)layout)->stride;
}

/* ── Transient buffers ─────────────────────────────────────────── */

bool jce_alloc_transient_buffers(JceTransientVertexBuffer *tvb,
                                 const JceVertexLayout *layout,
                                 uint32_t num_vertices,
                                 JceTransientIndexBuffer *tib,
                                 uint32_t num_indices,
                                 bool index32)
{
    return bgfx_alloc_transient_buffers((bgfx_transient_vertex_buffer_t *)tvb,
                                        (const bgfx_vertex_layout_t *)layout,
                                        num_vertices,
                                        (bgfx_transient_index_buffer_t *)tib,
                                        num_indices,
                                        index32);
}

void jce_set_transient_vertex_buffer(uint8_t stream,
                                     const JceTransientVertexBuffer *tvb,
                                     uint32_t start_vertex, uint32_t num_vertices)
{
    bgfx_set_transient_vertex_buffer(stream,
                                     (const bgfx_transient_vertex_buffer_t *)tvb,
                                     start_vertex, num_vertices);
}

void jce_set_transient_index_buffer(const JceTransientIndexBuffer *tib,
                                    uint32_t first_index, uint32_t num_indices)
{
    bgfx_set_transient_index_buffer((const bgfx_transient_index_buffer_t *)tib,
                                    first_index, num_indices);
}

/* ── Uniform / texture / program lifetime ───────────────────────── */

JceUniformHandle jce_uniform_create(const char *name, JceUniformType type, uint16_t num)
{
    bgfx_uniform_handle_t h = bgfx_create_uniform(name, (bgfx_uniform_type_t)type, num);
    JceUniformHandle r = { h.idx };
    return r;
}

void jce_uniform_destroy(JceUniformHandle h)
{
    if (h.idx == UINT16_MAX) return;
    bgfx_uniform_handle_t bh = { h.idx };
    bgfx_destroy_uniform(bh);
}

void jce_uniform_set(JceUniformHandle h, const void *value, uint16_t num)
{
    bgfx_uniform_handle_t bh = { h.idx };
    bgfx_set_uniform(bh, value, num);
}

const JceGfxMemory *jce_gfx_memory_copy(const void *data, uint32_t size)
{
    return (const JceGfxMemory *)bgfx_copy(data, size);
}

JceTextureHandle jce_texture_create_2d(uint16_t w, uint16_t h, bool has_mips,
                                       uint16_t num_layers, JceTextureFormat fmt,
                                       uint64_t flags, const JceGfxMemory *mem)
{
    bgfx_texture_handle_t bh = bgfx_create_texture_2d(w, h, has_mips, num_layers,
                                                      (bgfx_texture_format_t)fmt,
                                                      flags,
                                                      (const bgfx_memory_t *)mem);
    JceTextureHandle r = { bh.idx };
    return r;
}

JceTextureHandle jce_texture_create_from_encoded(const JceGfxMemory *mem,
                                                 uint64_t flags,
                                                 JceTextureInfo *out_info)
{
    bgfx_texture_info_t info;
    memset(&info, 0, sizeof(info));
    bgfx_texture_handle_t bh = bgfx_create_texture((const bgfx_memory_t *)mem,
                                                   flags, 0, &info);
    if (out_info) {
        out_info->format         = (uint32_t)info.format;
        out_info->storage_size   = info.storageSize;
        out_info->width          = info.width;
        out_info->height         = info.height;
        out_info->depth          = info.depth;
        out_info->num_layers     = info.numLayers;
        out_info->num_mips       = info.numMips;
        out_info->bits_per_pixel = info.bitsPerPixel;
        out_info->cube_map       = info.cubeMap;
    }
    JceTextureHandle r = { bh.idx };
    return r;
}

void jce_gfx_texture_destroy(JceTextureHandle h)
{
    if (h.idx == UINT16_MAX) return;
    bgfx_texture_handle_t bh = { h.idx };
    bgfx_destroy_texture(bh);
}

void jce_program_destroy(JceProgramHandle h)
{
    if (h.idx == UINT16_MAX) return;
    bgfx_program_handle_t bh = { h.idx };
    bgfx_destroy_program(bh);
}

/* ── Per-draw state ─────────────────────────────────────────────── */

void jce_set_state(uint64_t state, uint32_t rgba) { bgfx_set_state(state, rgba); }

void jce_set_transform(const float *mtx, uint16_t num) { bgfx_set_transform(mtx, num); }

void jce_set_texture(uint8_t stage, JceUniformHandle sampler,
                     JceTextureHandle texture, uint32_t flags)
{
    bgfx_uniform_handle_t  su = { sampler.idx };
    bgfx_texture_handle_t  tx = { texture.idx };
    bgfx_set_texture(stage, su, tx, flags);
}

void jce_submit(uint16_t view_id, JceProgramHandle program,
                uint32_t depth, uint8_t flags)
{
    bgfx_program_handle_t bp = { program.idx };
    bgfx_submit(view_id, bp, depth, flags);
}

/* ── Capability / stats ─────────────────────────────────────────── */

JceGfxCaps jce_gfx_caps(void)
{
    JceGfxCaps r = { false, false };
    const bgfx_caps_t *c = bgfx_get_caps();
    if (c) {
        r.homogeneous_depth = c->homogeneousDepth;
        r.origin_bottom_left = c->originBottomLeft;
    }
    return r;
}

/* Frame stats capture: copy bgfx values + view slice into TLS-ish buffer.
   Lifetime contract documented in header (valid until next capture). */

#ifndef JCE_GFX_VIEW_STATS_MAX
#define JCE_GFX_VIEW_STATS_MAX 256
#endif

static JceFrameStats s_stats;
static JceViewStats  s_view_stats[JCE_GFX_VIEW_STATS_MAX];

const JceFrameStats *jce_gfx_stats_capture(void)
{
    const bgfx_stats_t *st = bgfx_get_stats();
    if (!st) {
        memset(&s_stats, 0, sizeof(s_stats));
        return &s_stats;
    }

    s_stats.cpu_time_frame  = st->cpuTimeFrame;
    s_stats.cpu_timer_freq  = st->cpuTimerFreq;
    s_stats.gpu_time_begin  = st->gpuTimeBegin;
    s_stats.gpu_time_end    = st->gpuTimeEnd;
    s_stats.gpu_timer_freq  = st->gpuTimerFreq;
    s_stats.wait_render     = st->waitRender;
    s_stats.wait_submit     = st->waitSubmit;
    s_stats.num_draw        = st->numDraw;
    s_stats.num_compute     = st->numCompute;
    s_stats.num_blit        = st->numBlit;
    s_stats.num_views       = st->numViews;
    s_stats.num_dynamic_index_buffers  = st->numDynamicIndexBuffers;
    s_stats.num_dynamic_vertex_buffers = st->numDynamicVertexBuffers;
    s_stats.num_frame_buffers = st->numFrameBuffers;
    s_stats.num_index_buffers = st->numIndexBuffers;
    s_stats.num_occlusion_queries = st->numOcclusionQueries;
    s_stats.num_programs    = st->numPrograms;
    s_stats.num_shaders     = st->numShaders;
    s_stats.num_textures    = st->numTextures;
    s_stats.num_uniforms    = st->numUniforms;
    s_stats.num_vertex_buffers = st->numVertexBuffers;
    s_stats.num_vertex_layouts = st->numVertexLayouts;
    s_stats.texture_memory_used = st->textureMemoryUsed;
    s_stats.rt_memory_used      = st->rtMemoryUsed;
    s_stats.gpu_memory_used     = st->gpuMemoryUsed;
    s_stats.gpu_memory_max      = st->gpuMemoryMax;
    s_stats.backbuffer_width    = st->width;
    s_stats.backbuffer_height   = st->height;

    uint16_t n = st->numViews;
    if (n > JCE_GFX_VIEW_STATS_MAX) n = JCE_GFX_VIEW_STATS_MAX;
    for (uint16_t i = 0; i < n; ++i) {
        const bgfx_view_stats_t *vs = &st->viewStats[i];
        memcpy(s_view_stats[i].name, vs->name, sizeof(s_view_stats[i].name));
        s_view_stats[i].view_id        = vs->view;
        s_view_stats[i].cpu_time_begin = vs->cpuTimeBegin;
        s_view_stats[i].cpu_time_end   = vs->cpuTimeEnd;
        s_view_stats[i].gpu_time_begin = vs->gpuTimeBegin;
        s_view_stats[i].gpu_time_end   = vs->gpuTimeEnd;
    }
    s_stats.view_stats       = (n > 0) ? s_view_stats : NULL;
    s_stats.view_stats_count = n;
    return &s_stats;
}
