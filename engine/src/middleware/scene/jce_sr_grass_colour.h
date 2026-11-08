/* Complete colour draw emission; private to scene renderer implementation. */
#ifndef JCE_SR_GRASS_COLOUR_H
#define JCE_SR_GRASS_COLOUR_H
#include "jce_sr_internal.h"
#include "jce_sr_grass_submit_internal.h"
JceGrassColourReceipt sr_grass_submit_range(JceSceneRenderer *sr, int slot,
    const JceGrassFieldComponent *field, const JceCamera *camera,
    uint32_t receiver_layer, bgfx_program_handle_t program, uint64_t state,
    uint16_t view, uint16_t stride, bgfx_vertex_buffer_handle_t vertex,
    bgfx_index_buffer_handle_t index, uint32_t vertices, uint32_t indices,
    uint32_t start, uint32_t count);
#endif
