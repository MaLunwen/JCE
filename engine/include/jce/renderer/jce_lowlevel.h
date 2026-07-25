/*
 * jce_lowlevel.h  Public low-level renderer API.
 *
 * Thin wrappers around the underlying graphics API (currently bgfx) so that
 * NO bgfx headers leak into editor / game / FFI code.  Editor overlays,
 * custom uniforms, transient buffers, and per-frame stats all go through
 * this header instead of #include <bgfx/c99/bgfx.h>.
 *
 * Numeric values for state / attribute / format constants are kept 1:1 with
 * the bgfx ABI; the implementation file static_asserts equality.
 *
 * Layer: Renderer (Layer 3) — public.
 */

#ifndef JCE_LOWLEVEL_H
#define JCE_LOWLEVEL_H

#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_gfx_types.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Opaque program handle                                               */
/* ================================================================== */

typedef struct { uint16_t idx; } JceProgramHandle;
#define JCE_INVALID_PROGRAM ((JceProgramHandle){ UINT16_MAX })
static inline bool jce_program_valid(JceProgramHandle h) { return h.idx != UINT16_MAX; }

/* ================================================================== */
/* Vertex layout                                                       */
/* ================================================================== */

/* Opaque storage; current bgfx_vertex_layout_t is ~80 bytes.  128 leaves
   headroom if attribute count grows. */
typedef struct { uint8_t _opaque[128]; } JceVertexLayout;

typedef enum {
    JCE_ATTRIB_POSITION  = 0,
    JCE_ATTRIB_NORMAL    = 1,
    JCE_ATTRIB_TANGENT   = 2,
    JCE_ATTRIB_BITANGENT = 3,
    JCE_ATTRIB_COLOR0    = 4,
    JCE_ATTRIB_COLOR1    = 5,
    JCE_ATTRIB_COLOR2    = 6,
    JCE_ATTRIB_COLOR3    = 7,
    JCE_ATTRIB_INDICES   = 8,
    JCE_ATTRIB_WEIGHT    = 9,
    JCE_ATTRIB_TEXCOORD0 = 10,
    JCE_ATTRIB_TEXCOORD1 = 11,
    JCE_ATTRIB_TEXCOORD2 = 12,
    JCE_ATTRIB_TEXCOORD3 = 13,
    JCE_ATTRIB_TEXCOORD4 = 14,
    JCE_ATTRIB_TEXCOORD5 = 15,
    JCE_ATTRIB_TEXCOORD6 = 16,
    JCE_ATTRIB_TEXCOORD7 = 17
} JceAttrib;

typedef enum {
    /* Values mirror bgfx_attrib_type_t for a zero-cost cast (asserted in
     * jce_lowlevel.c).  bgfx 1.146 inserted INT8 at 0 and UINT16 at 4. */
    JCE_ATTRIB_TYPE_INT8 = 0,
    JCE_ATTRIB_TYPE_UINT8,
    JCE_ATTRIB_TYPE_UINT10,
    JCE_ATTRIB_TYPE_INT16,
    JCE_ATTRIB_TYPE_UINT16,
    JCE_ATTRIB_TYPE_HALF,
    JCE_ATTRIB_TYPE_FLOAT
} JceAttribType;

JCE_API void JCE_CALL jce_vertex_layout_begin(JceVertexLayout *layout);
JCE_API void JCE_CALL jce_vertex_layout_add(JceVertexLayout *layout,
                                            JceAttrib attrib,
                                            uint8_t num,
                                            JceAttribType type,
                                            bool normalized,
                                            bool as_int);
JCE_API void JCE_CALL jce_vertex_layout_end(JceVertexLayout *layout);
JCE_API uint16_t JCE_CALL jce_vertex_layout_stride(const JceVertexLayout *layout);

/* ================================================================== */
/* Transient (per-frame) vertex / index buffers                        */
/* ================================================================== */

typedef struct {
    uint8_t  *data;
    uint32_t  size;
    uint32_t  start_vertex;
    uint16_t  stride;
    uint16_t  _handle;
    uint16_t  _layout_handle;
    uint16_t  _pad;
} JceTransientVertexBuffer;

typedef struct {
    uint8_t  *data;
    uint32_t  size;
    uint32_t  start_index;
    uint16_t  _handle;
    bool      is_index16;
    uint8_t   _pad;
} JceTransientIndexBuffer;

JCE_API bool JCE_CALL jce_alloc_transient_buffers(JceTransientVertexBuffer *tvb,
                                                  const JceVertexLayout *layout,
                                                  uint32_t num_vertices,
                                                  JceTransientIndexBuffer *tib,
                                                  uint32_t num_indices,
                                                  bool index32);

JCE_API void JCE_CALL jce_set_transient_vertex_buffer(uint8_t stream,
                                                      const JceTransientVertexBuffer *tvb,
                                                      uint32_t start_vertex,
                                                      uint32_t num_vertices);
JCE_API void JCE_CALL jce_set_transient_index_buffer(const JceTransientIndexBuffer *tib,
                                                     uint32_t first_index,
                                                     uint32_t num_indices);

/* ================================================================== */
/* Uniform / texture / program lifetime                                */
/* ================================================================== */

typedef enum {
    JCE_UNIFORM_TYPE_SAMPLER = 0,
    JCE_UNIFORM_TYPE_VEC4    = 2,
    JCE_UNIFORM_TYPE_MAT3    = 3,
    JCE_UNIFORM_TYPE_MAT4    = 4
} JceUniformType;

JCE_API JceUniformHandle JCE_CALL jce_uniform_create(const char *name,
                                                     JceUniformType type,
                                                     uint16_t num);
JCE_API void JCE_CALL jce_uniform_destroy(JceUniformHandle h);
JCE_API void JCE_CALL jce_uniform_set(JceUniformHandle h, const void *value, uint16_t num);

/* Memory reference for texture upload.  Returned pointer is bgfx-owned;
   contents are copied internally — no JCE-side free required. */
typedef struct JceGfxMemory JceGfxMemory;
JCE_API const JceGfxMemory * JCE_CALL jce_gfx_memory_copy(const void *data, uint32_t size);

/* Texture format subset used by editor.  Matches bgfx_texture_format_t. */
typedef enum {
    JCE_TEXTURE_FORMAT_BC1 = 0,
    /* ... many compressed formats ... */
    JCE_TEXTURE_FORMAT_RGBA8 = 71   /* matches bgfx::TextureFormat::RGBA8 (bgfx 1.146) */
} JceTextureFormat;

/* No-op flag set (matches BGFX_TEXTURE_NONE / BGFX_SAMPLER_NONE). */
#define JCE_TEXTURE_FLAGS_NONE ((uint64_t)0)
#define JCE_SAMPLER_U_CLAMP    ((uint64_t)0x0000000000000002)
#define JCE_SAMPLER_V_CLAMP    ((uint64_t)0x0000000000000008)
#define JCE_SAMPLER_W_CLAMP    ((uint64_t)0x0000000000000020)

/* "Use texture's own sampler state" sentinel — matches UINT32_MAX. */
#define JCE_SAMPLER_INHERIT    (UINT32_MAX)

JCE_API JceTextureHandle JCE_CALL jce_texture_create_2d(uint16_t w,
                                                        uint16_t h,
                                                        bool has_mips,
                                                        uint16_t num_layers,
                                                        JceTextureFormat fmt,
                                                        uint64_t flags,
                                                        const JceGfxMemory *mem);

/* Texture info populated by jce_texture_create_from_encoded(). */
typedef struct {
    uint32_t format;       /* JceTextureFormat */
    uint32_t storage_size;
    uint16_t width;
    uint16_t height;
    uint16_t depth;
    uint16_t num_layers;
    uint8_t  num_mips;
    uint8_t  bits_per_pixel;
    bool     cube_map;
} JceTextureInfo;

/* Decode a KTX/KTX2/DDS container.  Caller need NOT free `data`; bgfx
   internally references the JceGfxMemory we wrap. */
JCE_API JceTextureHandle JCE_CALL jce_texture_create_from_encoded(const JceGfxMemory *mem,
                                                                  uint64_t flags,
                                                                  JceTextureInfo *out_info);

JCE_API void JCE_CALL jce_gfx_texture_destroy(JceTextureHandle h);

JCE_API void JCE_CALL jce_program_destroy(JceProgramHandle h);

/* ================================================================== */
/* Per-draw state                                                      */
/* ================================================================== */

/* Render state bits — values match BGFX_STATE_*. */
#define JCE_STATE_WRITE_R                ((uint64_t)0x0000000000000001)
#define JCE_STATE_WRITE_G                ((uint64_t)0x0000000000000002)
#define JCE_STATE_WRITE_B                ((uint64_t)0x0000000000000004)
#define JCE_STATE_WRITE_A                ((uint64_t)0x0000000000000008)
#define JCE_STATE_WRITE_RGB              (JCE_STATE_WRITE_R | JCE_STATE_WRITE_G | JCE_STATE_WRITE_B)
#define JCE_STATE_DEPTH_TEST_LESS        ((uint64_t)0x0000000000000010)
#define JCE_STATE_DEPTH_TEST_LEQUAL      ((uint64_t)0x0000000000000020)
#define JCE_STATE_MSAA                   ((uint64_t)0x0100000000000000)

#define JCE_BLEND_ZERO                   ((uint64_t)0x0000000000001000)
#define JCE_BLEND_ONE                    ((uint64_t)0x0000000000002000)
#define JCE_BLEND_SRC_ALPHA              ((uint64_t)0x0000000000005000)
#define JCE_BLEND_INV_SRC_ALPHA          ((uint64_t)0x0000000000006000)
#define JCE_BLEND_DST_COLOR              ((uint64_t)0x0000000000003000)

#define JCE_STATE_BLEND_FUNC(_src, _dst) ( ((_src) | ((_dst) << 4)) | ((((_src) | ((_dst) << 4)) << 8)) )

/* Discard flags. */
#define JCE_DISCARD_ALL                  ((uint8_t)0xff)

JCE_API void JCE_CALL jce_set_state(uint64_t state, uint32_t rgba);
JCE_API void JCE_CALL jce_set_transform(const float *mtx, uint16_t num);
JCE_API void JCE_CALL jce_set_texture(uint8_t stage,
                                      JceUniformHandle sampler,
                                      JceTextureHandle texture,
                                      uint32_t flags);
JCE_API void JCE_CALL jce_submit(uint16_t view_id,
                                 JceProgramHandle program,
                                 uint32_t depth,
                                 uint8_t flags);

/* ================================================================== */
/* Capability / stats queries                                          */
/* ================================================================== */

typedef struct {
    bool homogeneous_depth;     /* OpenGL-style [-1,1] depth */
    bool origin_bottom_left;
} JceGfxCaps;

JCE_API JceGfxCaps JCE_CALL jce_gfx_caps(void);

/* Per-view stats subset. */
typedef struct {
    char     name[256];
    uint16_t view_id;
    int64_t  cpu_time_begin;
    int64_t  cpu_time_end;
    int64_t  gpu_time_begin;
    int64_t  gpu_time_end;
} JceViewStats;

/* Frame stats subset surfaced to the editor profiler.  Filled by
   jce_gfx_stats_capture(); pointers in `view_stats` are valid until the
   next capture call. */
typedef struct {
    int64_t  cpu_time_frame;
    int64_t  cpu_timer_freq;
    int64_t  gpu_time_begin;
    int64_t  gpu_time_end;
    int64_t  gpu_timer_freq;
    int64_t  wait_render;
    int64_t  wait_submit;
    uint32_t num_draw;
    uint32_t num_compute;
    uint32_t num_blit;
    uint16_t num_views;
    uint16_t num_dynamic_index_buffers;
    uint16_t num_dynamic_vertex_buffers;
    uint16_t num_frame_buffers;
    uint16_t num_index_buffers;
    uint16_t num_occlusion_queries;
    uint16_t num_programs;
    uint16_t num_shaders;
    uint16_t num_textures;
    uint16_t num_uniforms;
    uint16_t num_vertex_buffers;
    uint16_t num_vertex_layouts;
    int64_t  texture_memory_used;
    int64_t  rt_memory_used;
    int64_t  gpu_memory_used;
    int64_t  gpu_memory_max;
    uint16_t backbuffer_width;
    uint16_t backbuffer_height;
    /* View slice; null when unused.  Lifetime: until next capture. */
    const JceViewStats *view_stats;
    uint16_t            view_stats_count;
} JceFrameStats;

JCE_API const JceFrameStats * JCE_CALL jce_gfx_stats_capture(void);

JCE_EXTERN_C_END

#endif /* JCE_LOWLEVEL_H */
