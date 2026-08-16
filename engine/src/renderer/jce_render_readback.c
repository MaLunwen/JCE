#include <jce/renderer/jce_render_readback.h>

#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_renderer.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>

#include <limits.h>
#include <string.h>

#define LOG_TAG "render_readback"
#define TICKET_SLOT_BITS 3u
#define TICKET_SLOT_MASK ((1u << TICKET_SLOT_BITS) - 1u)

typedef struct JceRenderReadbackSlot {
    uint32_t generation;
    uint32_t state;
    uint32_t error;
    uint32_t ready_frame;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint64_t row_pitch;
    uint64_t byte_size;
    bgfx_texture_handle_t staging;
    void *pixels;
} JceRenderReadbackSlot;

struct JceRenderReadbackQueue {
    JceRenderer *renderer;
    JceRenderReadbackSlot slots[JCE_RENDER_READBACK_QUEUE_CAPACITY];
};

static uint32_t format_bytes(JceRenderFormat format)
{
    switch (format) {
    case JCE_RENDER_FORMAT_RGBA8: return 4;
    case JCE_RENDER_FORMAT_RGBA16F: return 8;
    case JCE_RENDER_FORMAT_R32F: return 4;
    case JCE_RENDER_FORMAT_R16F: return 2;
    case JCE_RENDER_FORMAT_RG16F: return 4;
    case JCE_RENDER_FORMAT_RG32F: return 8;
    case JCE_RENDER_FORMAT_RGBA32F: return 16;
    default: return 0;
    }
}

static bgfx_texture_format_t to_bgfx_format(JceRenderFormat format)
{
    switch (format) {
    case JCE_RENDER_FORMAT_RGBA8: return BGFX_TEXTURE_FORMAT_RGBA8;
    case JCE_RENDER_FORMAT_RGBA16F: return BGFX_TEXTURE_FORMAT_RGBA16F;
    case JCE_RENDER_FORMAT_R32F: return BGFX_TEXTURE_FORMAT_R32F;
    case JCE_RENDER_FORMAT_R16F: return BGFX_TEXTURE_FORMAT_R16F;
    case JCE_RENDER_FORMAT_RG16F: return BGFX_TEXTURE_FORMAT_RG16F;
    case JCE_RENDER_FORMAT_RG32F: return BGFX_TEXTURE_FORMAT_RG32F;
    case JCE_RENDER_FORMAT_RGBA32F: return BGFX_TEXTURE_FORMAT_RGBA32F;
    default: return BGFX_TEXTURE_FORMAT_COUNT;
    }
}

JceRenderReadbackDesc jce_render_readback_desc_default(void)
{
    JceRenderReadbackDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.version = JCE_RENDER_READBACK_ABI_VERSION;
    desc.source = JCE_INVALID_TEXTURE;
    desc.format = JCE_RENDER_FORMAT_RGBA32F;
    return desc;
}

bool jce_render_readback_layout(JceRenderFormat format,
                                uint32_t width, uint32_t height,
                                uint64_t *out_row_pitch,
                                uint64_t *out_byte_size)
{
    uint32_t bytes = format_bytes(format);
    uint64_t row_pitch;
    uint64_t byte_size;
    if (!bytes || !width || !height || width > UINT16_MAX ||
        height > UINT16_MAX)
        return false;
    row_pitch = (uint64_t)width * bytes;
    if (row_pitch > SIZE_MAX || (uint64_t)height > UINT64_MAX / row_pitch)
        return false;
    byte_size = row_pitch * height;
    if (byte_size > SIZE_MAX) return false;
    if (out_row_pitch) *out_row_pitch = row_pitch;
    if (out_byte_size) *out_byte_size = byte_size;
    return true;
}

bool jce_render_readback_format_supported(JceRenderFormat format)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    bgfx_texture_format_t native_format = to_bgfx_format(format);
    if (!caps || native_format == BGFX_TEXTURE_FORMAT_COUNT) return false;
    if ((caps->supported &
         (BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK)) !=
        (BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK))
        return false;
    return (caps->formats[native_format] & BGFX_CAPS_FORMAT_TEXTURE_2D) != 0;
}

static JceRenderReadbackTicket make_ticket(uint32_t slot, uint32_t generation)
{
    return (generation << TICKET_SLOT_BITS) | (slot + 1u);
}

static JceRenderReadbackSlot *find_slot(JceRenderReadbackQueue *queue,
                                        JceRenderReadbackTicket ticket)
{
    uint32_t encoded_slot;
    uint32_t slot;
    uint32_t generation;
    if (!queue || ticket == JCE_RENDER_READBACK_INVALID_TICKET) return NULL;
    encoded_slot = ticket & TICKET_SLOT_MASK;
    if (!encoded_slot || encoded_slot > JCE_RENDER_READBACK_QUEUE_CAPACITY)
        return NULL;
    slot = encoded_slot - 1u;
    generation = ticket >> TICKET_SLOT_BITS;
    if (!generation || queue->slots[slot].generation != generation ||
        queue->slots[slot].state == JCE_RENDER_READBACK_INVALID)
        return NULL;
    return &queue->slots[slot];
}

static const JceRenderReadbackSlot *find_slot_const(
    const JceRenderReadbackQueue *queue, JceRenderReadbackTicket ticket)
{
    return find_slot((JceRenderReadbackQueue *)queue, ticket);
}

static void slot_dispose(JceRenderReadbackSlot *slot)
{
    if (!slot) return;
    if (slot->staging.idx != UINT16_MAX)
        bgfx_destroy_texture(slot->staging);
    if (slot->pixels) JCE_FREE(slot->pixels);
    slot->staging.idx = UINT16_MAX;
    slot->pixels = NULL;
}

static void slot_reset(JceRenderReadbackSlot *slot)
{
    uint32_t generation;
    if (!slot) return;
    generation = slot->generation + 1u;
    if (!generation) generation = 1u;
    slot_dispose(slot);
    memset(slot, 0, sizeof(*slot));
    slot->generation = generation;
    slot->staging.idx = UINT16_MAX;
}

JceRenderReadbackQueue *jce_render_readback_create(JceRenderer *renderer)
{
    JceRenderReadbackQueue *queue =
        (JceRenderReadbackQueue *)JCE_CALLOC(1, sizeof(*queue));
    uint32_t i;
    if (!queue) return NULL;
    queue->renderer = renderer;
    for (i = 0; i < JCE_RENDER_READBACK_QUEUE_CAPACITY; i++) {
        queue->slots[i].generation = 1u;
        queue->slots[i].staging.idx = UINT16_MAX;
    }
    return queue;
}

void jce_render_readback_destroy(JceRenderReadbackQueue *queue)
{
    uint32_t i;
    uint32_t current = queue && queue->renderer ?
        jce_renderer_get_frame_index(queue->renderer) : 0u;
    uint32_t maximum = current;
    if (!queue) return;
    for (i = 0; i < JCE_RENDER_READBACK_QUEUE_CAPACITY; i++) {
        JceRenderReadbackSlot *slot = &queue->slots[i];
        if ((slot->state == JCE_RENDER_READBACK_PENDING ||
             slot->state == JCE_RENDER_READBACK_CANCELLED) &&
            slot->ready_frame > maximum)
            maximum = slot->ready_frame;
    }
    while (current < maximum) current = bgfx_frame(false);
    for (i = 0; i < JCE_RENDER_READBACK_QUEUE_CAPACITY; i++)
        slot_dispose(&queue->slots[i]);
    JCE_FREE(queue);
}

bool jce_render_readback_submit(JceRenderReadbackQueue *queue,
                                const JceRenderReadbackDesc *desc,
                                JceRenderReadbackTicket *out_ticket)
{
    uint64_t row_pitch, byte_size;
    bgfx_texture_format_t native_format;
    JceRenderReadbackSlot *slot = NULL;
    uint32_t slot_index = 0;
    uint32_t i;
    if (out_ticket) *out_ticket = JCE_RENDER_READBACK_INVALID_TICKET;
    if (!queue || !desc || !out_ticket ||
        desc->struct_size != sizeof(*desc) ||
        desc->version != JCE_RENDER_READBACK_ABI_VERSION ||
        !jce_gfx_texture_valid(desc->source) ||
        desc->source_x > UINT16_MAX || desc->source_y > UINT16_MAX ||
        !jce_render_readback_layout((JceRenderFormat)desc->format,
            desc->width, desc->height, &row_pitch, &byte_size) ||
        desc->source_x + desc->width > UINT16_MAX + 1ull ||
        desc->source_y + desc->height > UINT16_MAX + 1ull)
        return false;
    if (!jce_render_readback_format_supported(
            (JceRenderFormat)desc->format))
        return false;
    for (i = 0; i < JCE_RENDER_READBACK_QUEUE_CAPACITY; i++) {
        if (queue->slots[i].state == JCE_RENDER_READBACK_INVALID) {
            slot = &queue->slots[i];
            slot_index = i;
            break;
        }
    }
    if (!slot) return false;

    native_format = to_bgfx_format((JceRenderFormat)desc->format);
    slot->pixels = JCE_MALLOC((size_t)byte_size);
    if (!slot->pixels) return false;
    slot->staging = bgfx_create_texture_2d(
        (uint16_t)desc->width, (uint16_t)desc->height, false, 1,
        native_format,
        BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK |
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
            BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
        NULL, 0);
    if (slot->staging.idx == UINT16_MAX) {
        JCE_FREE(slot->pixels);
        slot->pixels = NULL;
        return false;
    }

    slot->state = JCE_RENDER_READBACK_PENDING;
    slot->error = JCE_RENDER_READBACK_ERROR_NONE;
    slot->format = desc->format;
    slot->width = desc->width;
    slot->height = desc->height;
    slot->row_pitch = row_pitch;
    slot->byte_size = byte_size;
    bgfx_blit(desc->view_id, slot->staging, 0, 0, 0, 0,
        (bgfx_texture_handle_t){ desc->source.idx }, 0, 0,
        (uint16_t)desc->source_x, (uint16_t)desc->source_y,
        (uint16_t)desc->width, (uint16_t)desc->height, 1);
    slot->ready_frame = bgfx_read_texture(
        slot->staging, slot->pixels, 0, 0);
    *out_ticket = make_ticket(slot_index, slot->generation);
    return true;
}

JceRenderReadbackState jce_render_readback_poll(
    JceRenderReadbackQueue *queue, JceRenderReadbackTicket ticket)
{
    JceRenderReadbackSlot *slot = find_slot(queue, ticket);
    uint32_t frame;
    if (!slot) return JCE_RENDER_READBACK_INVALID;
    if (slot->state != JCE_RENDER_READBACK_PENDING &&
        slot->state != JCE_RENDER_READBACK_CANCELLED)
        return (JceRenderReadbackState)slot->state;
    frame = queue->renderer ? jce_renderer_get_frame_index(queue->renderer) : 0u;
    if (frame >= slot->ready_frame) {
        if (slot->state == JCE_RENDER_READBACK_CANCELLED) {
            slot_dispose(slot);
        } else {
            slot->state = JCE_RENDER_READBACK_READY;
        }
    }
    return (JceRenderReadbackState)slot->state;
}

bool jce_render_readback_get_info(const JceRenderReadbackQueue *queue,
                                  JceRenderReadbackTicket ticket,
                                  JceRenderReadbackInfo *out_info)
{
    const JceRenderReadbackSlot *slot = find_slot_const(queue, ticket);
    if (!slot || !out_info) return false;
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->state = slot->state;
    out_info->error = slot->error;
    out_info->format = slot->format;
    out_info->width = slot->width;
    out_info->height = slot->height;
    out_info->row_pitch = slot->row_pitch;
    out_info->byte_size = slot->byte_size;
    out_info->ready_frame = slot->ready_frame;
    return true;
}

bool jce_render_readback_copy(JceRenderReadbackQueue *queue,
                              JceRenderReadbackTicket ticket,
                              void *destination, size_t destination_size)
{
    JceRenderReadbackSlot *slot = find_slot(queue, ticket);
    if (!slot || !destination || slot->state != JCE_RENDER_READBACK_READY ||
        !slot->pixels || destination_size < slot->byte_size) {
        if (slot && destination_size < slot->byte_size)
            slot->error = JCE_RENDER_READBACK_ERROR_COPY_SIZE;
        return false;
    }
    memcpy(destination, slot->pixels, (size_t)slot->byte_size);
    return true;
}

bool jce_render_readback_cancel(JceRenderReadbackQueue *queue,
                                JceRenderReadbackTicket ticket)
{
    JceRenderReadbackSlot *slot = find_slot(queue, ticket);
    if (!slot) return false;
    if (slot->state == JCE_RENDER_READBACK_PENDING) {
        slot->state = JCE_RENDER_READBACK_CANCELLED;
        return true;
    }
    if (slot->state == JCE_RENDER_READBACK_READY ||
        slot->state == JCE_RENDER_READBACK_FAILED) {
        slot_dispose(slot);
        slot->state = JCE_RENDER_READBACK_CANCELLED;
        return true;
    }
    return slot->state == JCE_RENDER_READBACK_CANCELLED;
}

bool jce_render_readback_release(JceRenderReadbackQueue *queue,
                                 JceRenderReadbackTicket ticket)
{
    JceRenderReadbackSlot *slot = find_slot(queue, ticket);
    if (!slot) return false;
    if (slot->state == JCE_RENDER_READBACK_PENDING) {
        slot->state = JCE_RENDER_READBACK_CANCELLED;
        return true;
    }
    slot_reset(slot);
    return true;
}
