/*
 * jce_render_encoder.h - internal thread-local bgfx encoder shim for draw
 * submission (lever ①). Requires bgfx built multi-threaded (maxEncoders>1).
 *
 * Draw code calls jce_enc_*() which routes to the calling thread's encoder when
 * one is installed (jce_render_encoder_set), and to the implicit bgfx_* path
 * when NULL. NULL on the main/API thread == bgfx_*() exactly (byte-identical
 * serial path); a worker installs bgfx_encoder_begin(true) for its range.
 */
#ifndef JCE_RENDER_ENCODER_H
#define JCE_RENDER_ENCODER_H

#include <bgfx/c99/bgfx.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
extern __declspec(thread) bgfx_encoder_t *jce_tls_encoder;
#else
extern __thread bgfx_encoder_t *jce_tls_encoder;
#endif

static inline void jce_render_encoder_set(bgfx_encoder_t *enc) { jce_tls_encoder = enc; }

/* Per-frame transform-matrix pressure counter (always-on, mirrors the rank-9
 * alloc counter).  bgfx caches every set_transform matrix in a fixed
 * BGFX_CONFIG_MAX_MATRIX_CACHE pool (65536 as built) that SATURATES SILENTLY
 * in release — once full, later draws get clamped/garbage transforms and
 * vanish (measured: per-char skinned crowds stop color-rendering between
 * ~500-800 characters; bones dominate at ~24-128 matrices per draw).  This
 * counts the two whale producers (this shim + jce_skinned_mesh_set_bones) as
 * a LOWER BOUND — raw single-matrix bgfx_set_transform call sites are not
 * routed — so a warning here is always a real overflow.  Reset each frame in
 * jce_renderer_end_frame, which also warns once near the cap and tracks the
 * peak.  Plain (non-atomic) increments: the opt-in parallel-submit path may
 * undercount slightly, which is acceptable for diagnostics. */
extern uint32_t jce_dbg_xform_matrices;

static inline uint32_t jce_enc_set_transform(const void *mtx, uint16_t num)
{
    bgfx_encoder_t *e = jce_tls_encoder;
    jce_dbg_xform_matrices += num;
    return e ? bgfx_encoder_set_transform(e, mtx, num) : bgfx_set_transform(mtx, num);
}
static inline void jce_enc_set_state(uint64_t state, uint32_t rgba)
{
    bgfx_encoder_t *e = jce_tls_encoder;
    if (e) bgfx_encoder_set_state(e, state, rgba); else bgfx_set_state(state, rgba);
}
static inline void jce_enc_set_texture(uint8_t stage, bgfx_uniform_handle_t sampler,
                                       bgfx_texture_handle_t handle, uint32_t flags)
{
    bgfx_encoder_t *e = jce_tls_encoder;
    if (e) bgfx_encoder_set_texture(e, stage, sampler, handle, flags);
    else   bgfx_set_texture(stage, sampler, handle, flags);
}
static inline void jce_enc_set_uniform(bgfx_uniform_handle_t handle, const void *value, uint16_t num)
{
    bgfx_encoder_t *e = jce_tls_encoder;
    if (e) bgfx_encoder_set_uniform(e, handle, value, num);
    else   bgfx_set_uniform(handle, value, num);
}
static inline void jce_enc_set_vertex_buffer(uint8_t stream, bgfx_vertex_buffer_handle_t handle,
                                             uint32_t startVertex, uint32_t numVertices)
{
    bgfx_encoder_t *e = jce_tls_encoder;
    if (e) bgfx_encoder_set_vertex_buffer(e, stream, handle, startVertex, numVertices);
    else   bgfx_set_vertex_buffer(stream, handle, startVertex, numVertices);
}
static inline void jce_enc_set_index_buffer(bgfx_index_buffer_handle_t handle,
                                            uint32_t firstIndex, uint32_t numIndices)
{
    bgfx_encoder_t *e = jce_tls_encoder;
    if (e) bgfx_encoder_set_index_buffer(e, handle, firstIndex, numIndices);
    else   bgfx_set_index_buffer(handle, firstIndex, numIndices);
}
static inline void jce_enc_set_instance_data_buffer(const bgfx_instance_data_buffer_t *idb,
                                                    uint32_t start, uint32_t num)
{
    bgfx_encoder_t *e = jce_tls_encoder;
    if (e) bgfx_encoder_set_instance_data_buffer(e, idb, start, num);
    else   bgfx_set_instance_data_buffer(idb, start, num);
}
static inline void jce_enc_set_instance_data_from_dynamic_vertex_buffer(
    bgfx_dynamic_vertex_buffer_handle_t handle, uint32_t startVertex, uint32_t num)
{
    bgfx_encoder_t *e = jce_tls_encoder;
    if (e) bgfx_encoder_set_instance_data_from_dynamic_vertex_buffer(e, handle, startVertex, num);
    else   bgfx_set_instance_data_from_dynamic_vertex_buffer(handle, startVertex, num);
}
static inline void jce_enc_submit(bgfx_view_id_t id, bgfx_program_handle_t program,
                                  uint32_t depth, uint8_t flags)
{
    bgfx_encoder_t *e = jce_tls_encoder;
    if (e) bgfx_encoder_submit(e, id, program, depth, flags);
    else   bgfx_submit(id, program, depth, flags);
}

#ifdef __cplusplus
}
#endif

#endif /* JCE_RENDER_ENCODER_H */
