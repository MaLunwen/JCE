#include "jce_sr_particle_batch.h"
#include <jce/api_core.h>
#include <string.h>

static void warn_partial(SrParticleArena *arena, uint32_t frame, uint32_t requested,
                         uint32_t available)
{
    if (arena->warned && (uint32_t)(frame - arena->warned_frame) < 120u) return;
    arena->warned = true;
    arena->warned_frame = frame;
    LOG_WARN("particles", "CPU particle upload unavailable: requested=%u submitted-cap=%u frame=%u",
             requested, available, frame);
}

bool sr_particle_upload_begin(SrParticleArena *arena, uint32_t frame,
                              uint32_t requested, uint32_t budget_mb, SrParticleUpload *out)
{
    if (!arena || !out) return false;
    memset(out, 0, sizeof *out);
    uint32_t limit = budget_mb <= 256 ? budget_mb * (1048576u / SR_PARTICLE_INSTANCE_BYTES) : 0;
    bool frame_edge = !arena->frame_valid || arena->frame != frame;
    if (sr_particle_arena_trim_needed(arena->capacity, limit, frame_edge)) {
        bgfx_dynamic_vertex_buffer_handle_t old = {arena->handle};
        if (BGFX_HANDLE_IS_VALID(old)) bgfx_destroy_dynamic_vertex_buffer(old);
        arena->handle = UINT16_MAX;
        arena->capacity = 0;
    }
    if (frame_edge) {
        arena->frame = frame; arena->frame_valid = true; arena->cursor = 0;
    }
    if (!requested) return false;
    uint32_t available = bgfx_get_avail_instance_data_buffer(requested, SR_PARTICLE_INSTANCE_BYTES);
    if (available >= requested) goto transient;
    uint32_t capacity, offset;
    if (!sr_particle_arena_plan(arena->capacity, arena->cursor, requested, limit, &capacity, &offset))
        goto partial;
    if (capacity != arena->capacity) {
        bgfx_vertex_layout_t layout;
        bgfx_vertex_layout_begin(&layout, BGFX_RENDERER_TYPE_NOOP);
        bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD7, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD6, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD5, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD4, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_end(&layout);
        bgfx_dynamic_vertex_buffer_handle_t next = bgfx_create_dynamic_vertex_buffer(capacity, &layout, BGFX_BUFFER_NONE);
        if (!BGFX_HANDLE_IS_VALID(next)) goto partial;
        bgfx_dynamic_vertex_buffer_handle_t old = {arena->handle};
        if (BGFX_HANDLE_IS_VALID(old)) bgfx_destroy_dynamic_vertex_buffer(old);
        arena->handle = next.idx; arena->capacity = capacity;
    }
    const bgfx_memory_t *memory = bgfx_alloc(requested * SR_PARTICLE_INSTANCE_BYTES);
    if (!memory) goto partial;
    arena->cursor = offset + requested;
    out->memory = memory; out->data = (float *)memory->data;
    out->count = requested; out->offset = offset; out->dynamic = true;
    return true;
partial:
    warn_partial(arena, frame, requested, available);
transient:
    out->count = requested < available ? requested : available;
    if (!out->count) return false;
    bgfx_alloc_instance_data_buffer(&out->transient, out->count, SR_PARTICLE_INSTANCE_BYTES);
    out->data = (float *)out->transient.data;
    return true;
}

void sr_particle_upload_bind(SrParticleArena *arena, SrParticleUpload *upload, uint32_t actual)
{
    if (!arena || !upload || actual > upload->count) return;
    if (upload->dynamic) {
        if (actual < upload->count)
            memset(upload->memory->data + actual * SR_PARTICLE_INSTANCE_BYTES, 0,
                   (upload->count - actual) * SR_PARTICLE_INSTANCE_BYTES);
        bgfx_dynamic_vertex_buffer_handle_t buffer = {arena->handle};
        bgfx_update_dynamic_vertex_buffer(buffer, upload->offset, upload->memory);
        if (actual) bgfx_set_instance_data_from_dynamic_vertex_buffer(buffer, upload->offset, actual);
    } else if (actual) bgfx_set_instance_data_buffer(&upload->transient, 0, actual);
}
void sr_particle_arena_destroy(SrParticleArena *arena)
{
    bgfx_dynamic_vertex_buffer_handle_t buffer = {arena->handle};
    if (BGFX_HANDLE_IS_VALID(buffer)) bgfx_destroy_dynamic_vertex_buffer(buffer);
    memset(arena, 0, sizeof *arena);
    arena->handle = UINT16_MAX;
}
