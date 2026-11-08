#include <jce/renderer/jce_render_readback.h>

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_trace.h>
#include <jce/renderer/jce_renderer.h>

#include "os/core/jce_memory.h"
#include "jce_render_readback_internal.h"
#include "jce_renderer_frame_serial_internal.h"
#include "jce_offscreen_retirement_internal.h"
#include "jce_renderer_lifecycle_internal.h"
#include "os/core/jce_trace_capture_internal.h"
#include "os/core/jce_trace_scope_internal.h"

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
    /* Observation identity is private and never changes the backend borrow. */
    uint64_t trace_operation, trace_context;
    uint32_t trace_capture, trace_ticket;
    bool trace_ready, trace_backend_stopped;
} JceRenderReadbackSlot;

struct JceRenderReadbackQueue {
    JceRenderer *renderer;
    JceRenderer *backend_owner;
    struct JceRenderReadbackQueue *next;
    bool closing,destroy_requested;
    JceRenderReadbackSlot slots[JCE_RENDER_READBACK_QUEUE_CAPACITY];
};
/* Intrusive metadata adds no allocation or competing frame/backend owner.
 * Detached wrappers are removed after join and may be destroyed later. */
static JceRenderer *s_readback_renderer;
static JceRenderReadbackQueue *s_readback_queues;
static uint32_t s_readback_attached,s_readback_retiring,s_readback_cpu_buffers;
static uint64_t s_readback_cpu_bytes,s_readback_stop_releases;
enum { JCE_READBACK_DESTROY_ADVANCE_LIMIT=8 };
/* Transition-only CPU ownership ledger, never GPU residency or a fence. */
static void readback_trace_ownership(void)
{
    if(!jce_trace_enabled()) return;
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER,"readback.cpu_service_open",s_readback_renderer!=NULL);
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER,"readback.cpu_attached_queues",s_readback_attached);
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER,"readback.cpu_retiring_queues",s_readback_retiring);
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER,"readback.cpu_owned_destinations",s_readback_cpu_buffers);
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER,"readback.cpu_owned_destination_bytes",(int64_t)s_readback_cpu_bytes);
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER,"readback.backend_stop_cpu_releases",(int64_t)s_readback_stop_releases);
}

/* Transition-only per-operation witnesses. READY means this readback's
 * provider-promised serial was observed, never whole-device idle or Present. */
enum { READBACK_TRACE_SUBMIT=1, READBACK_TRACE_READY, READBACK_TRACE_CANCEL,
       READBACK_TRACE_RELEASE, READBACK_TRACE_STOP_RELEASE };
static void readback_trace_operation(JceRenderReadbackSlot *slot, uint32_t stage,
                                    uint32_t frame, bool frame_known)
{
    if (stage==READBACK_TRACE_READY) {
        if (slot->trace_ready) return;
        slot->trace_ready=true;
    }
    if (!jce_trace_enabled()) return;
    JceTraceScope scope=jce_trace_scope_begin("readback.operation");
    if (!scope.id) return;
    const uint32_t capture=jce_trace_capture_generation();
    if (stage==READBACK_TRACE_SUBMIT) {
        slot->trace_operation=scope.id; slot->trace_capture=capture;
        uint32_t ignored=0;
        if (!jce_renderer_gpu_observation_context_internal(&slot->trace_context,&ignored))
            slot->trace_context=0;
    }
    const bool linked=slot->trace_operation && slot->trace_capture==capture;
    const uint64_t operation=linked ? slot->trace_operation : 0;
#define READBACK_COUNT(name_,value_) jce_trace_counter_scoped_internal( \
    JCE_TRACE_CATEGORY_RENDER,"readback.op." name_,(int64_t)(value_),scope.id,scope.id)
    READBACK_COUNT("operation_lo",(uint32_t)operation);
    READBACK_COUNT("operation_hi",(uint32_t)(operation>>32));
    READBACK_COUNT("capture_linked",linked);
    READBACK_COUNT("context_generation",linked && slot->trace_context &&
        slot->trace_context<=(uint64_t)INT64_MAX ? (int64_t)slot->trace_context : -1);
    READBACK_COUNT("ticket",slot->trace_ticket);
    READBACK_COUNT("stage",stage);
    READBACK_COUNT("ready_frame",slot->ready_frame);
    READBACK_COUNT("observed_frame",frame);
    READBACK_COUNT("observed_frame_known",frame_known);
    READBACK_COUNT("readback_ready_observed",slot->trace_ready);
    READBACK_COUNT("destination_released",slot->pixels==NULL);
    READBACK_COUNT("backend_stopped",slot->trace_backend_stopped);
    READBACK_COUNT("destination_bytes",slot->byte_size);
#undef READBACK_COUNT
    jce_trace_scope_end(scope,JCE_TRACE_TASK_SUCCEEDED);
}
static void readback_trace_current(JceRenderReadbackSlot *slot,uint32_t stage)
{
    const bool known=s_readback_renderer!=NULL;
    readback_trace_operation(slot,stage,
        known ? jce_renderer_get_frame_index(s_readback_renderer) : 0,known);
}

void jce_render_readback_backend_open(JceRenderer *renderer)
{
    if(!renderer || s_readback_renderer || s_readback_queues) return;
    s_readback_renderer=renderer;s_readback_stop_releases=0;
    readback_trace_ownership();
}

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
    const bool released=slot->pixels!=NULL;
    if (slot->pixels) {
        JCE_FREE(slot->pixels);
        --s_readback_cpu_buffers;s_readback_cpu_bytes-=slot->byte_size;
    }
    slot->staging.idx = UINT16_MAX;
    slot->pixels = NULL;
    if (released && slot->state!=JCE_RENDER_READBACK_INVALID)
        readback_trace_current(slot,slot->trace_backend_stopped ?
            READBACK_TRACE_STOP_RELEASE : READBACK_TRACE_RELEASE);
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
    if(!renderer || renderer!=s_readback_renderer) return NULL;
    JceRenderReadbackQueue *queue =
        (JceRenderReadbackQueue *)JCE_CALLOC(1, sizeof(*queue));
    uint32_t i;
    if (!queue) return NULL;
    queue->renderer = renderer;
    queue->backend_owner=renderer;queue->next=s_readback_queues;s_readback_queues=queue;
    ++s_readback_attached;
    for (i = 0; i < JCE_RENDER_READBACK_QUEUE_CAPACITY; i++) {
        queue->slots[i].generation = 1u;
        queue->slots[i].staging.idx = UINT16_MAX;
    }
    readback_trace_ownership();return queue;
}

static void queue_unlink(JceRenderReadbackQueue *queue)
{
    JceRenderReadbackQueue **link=&s_readback_queues;
    while(*link && *link!=queue) link=&(*link)->next;
    if(*link) {
        *link=queue->next;
        if(queue->closing) --s_readback_retiring;else --s_readback_attached;
    }
    queue->next=NULL;
}
static void queue_retire(JceRenderReadbackQueue *queue)
{
    if(!queue->closing){--s_readback_attached;++s_readback_retiring;}
    queue->closing=true;queue->renderer=NULL;
    for(uint32_t i=0;i<JCE_RENDER_READBACK_QUEUE_CAPACITY;++i) {
        JceRenderReadbackSlot *slot=&queue->slots[i];
        if(slot->staging.idx!=UINT16_MAX) {
            bgfx_destroy_texture(slot->staging);slot->staging.idx=UINT16_MAX;
        }
        /* A cancel changes intent, never the backend's CPU borrow. */
        if(slot->state!=JCE_RENDER_READBACK_PENDING && slot->state!=JCE_RENDER_READBACK_CANCELLED)
            slot_dispose(slot);
    }
}
void jce_render_readback_before_backend_shutdown(JceRenderer *renderer)
{
    if(!renderer || renderer!=s_readback_renderer) return;
    s_readback_renderer=NULL;
    for(JceRenderReadbackQueue *queue=s_readback_queues;queue;queue=queue->next)
        if(queue->backend_owner==renderer) queue_retire(queue);
    readback_trace_ownership();
}
void jce_render_readback_after_backend_shutdown(JceRenderer *renderer)
{
    JceRenderReadbackQueue **link=&s_readback_queues;
    while(*link) {
        JceRenderReadbackQueue *queue=*link;
        if(queue->backend_owner!=renderer || !queue->closing){link=&queue->next;continue;}
        *link=queue->next;queue->next=NULL;queue->backend_owner=NULL;--s_readback_retiring;
        for(uint32_t i=0;i<JCE_RENDER_READBACK_QUEUE_CAPACITY;++i) {
            JceRenderReadbackSlot *slot=&queue->slots[i];
            if(slot->pixels) ++s_readback_stop_releases;
            slot->trace_backend_stopped=true;
            slot_dispose(slot); /* GPU handles were retired before shutdown. */
            if(slot->state!=JCE_RENDER_READBACK_INVALID) {
                slot->state=JCE_RENDER_READBACK_FAILED;slot->error=JCE_RENDER_READBACK_ERROR_UNSUPPORTED;
            }
        }
        if(queue->destroy_requested) JCE_FREE(queue);
    }
    readback_trace_ownership();
}
static bool queue_pending(JceRenderReadbackQueue *queue,uint32_t current,bool *ambiguous)
{
    bool pending=false;*ambiguous=false;
    for(uint32_t i=0;i<JCE_RENDER_READBACK_QUEUE_CAPACITY;++i) {
        JceRenderReadbackSlot *slot=&queue->slots[i];
        if(!slot->pixels || (slot->state!=JCE_RENDER_READBACK_PENDING && slot->state!=JCE_RENDER_READBACK_CANCELLED)) continue;
        JceRendererFrameOrder order=jce_renderer_frame_order(current,slot->ready_frame);
        if(order==JCE_RENDERER_FRAME_AMBIGUOUS) *ambiguous=true;
        if(!jce_renderer_frame_ready(current,slot->ready_frame)) pending=true;
        else readback_trace_operation(slot,READBACK_TRACE_READY,current,true);
    }
    return pending;
}
static void queue_free_storage(JceRenderReadbackQueue *queue)
{
    for(uint32_t i=0;i<JCE_RENDER_READBACK_QUEUE_CAPACITY;++i) slot_dispose(&queue->slots[i]);
    JCE_FREE(queue);
}
bool jce_render_readback_deferred_admission(const JceRenderer *renderer,
    const JceRenderReadbackQueue *existing_queue)
{
    if(!renderer || renderer!=s_readback_renderer) return false;
    if(existing_queue) return !existing_queue->closing && existing_queue->renderer==renderer;
    return s_readback_retiring<JCE_RENDER_READBACK_QUEUE_CAPACITY;
}
void jce_render_readback_destroy_deferred(JceRenderReadbackQueue *queue)
{
    if(!queue) return;
    if(queue->closing && queue->backend_owner) {queue->destroy_requested=true;return;}
    if(queue->renderer && queue->renderer==s_readback_renderer) {
        bool ambiguous=false;
        uint32_t current=jce_renderer_get_frame_index(queue->renderer);
        if(queue_pending(queue,current,&ambiguous)) {
            queue->destroy_requested=true;queue_retire(queue);
            readback_trace_ownership();
            return;
        }
    }
    queue_unlink(queue);queue_free_storage(queue);readback_trace_ownership();
}
void jce_render_readback_collect(const JceRenderer *renderer,uint32_t frame)
{
    if(!renderer || renderer!=s_readback_renderer) return;
    bool changed=false;
    JceRenderReadbackQueue **link=&s_readback_queues;
    while(*link) {
        JceRenderReadbackQueue *queue=*link;
        bool ambiguous=false;
        if(queue->backend_owner!=renderer || !queue->closing || !queue->destroy_requested ||
            queue_pending(queue,frame,&ambiguous)) {
            link=&queue->next;continue;
        }
        *link=queue->next;--s_readback_retiring;
        queue_free_storage(queue);changed=true;
    }
    if(changed) readback_trace_ownership();
}
void jce_render_readback_destroy(JceRenderReadbackQueue *queue)
{
    if (!queue) return;
    if(queue->closing && queue->backend_owner) {queue->destroy_requested=true;return;}
    if(queue->renderer && queue->renderer==s_readback_renderer) {
        uint32_t current=jce_renderer_get_frame_index(queue->renderer),advances=0;
        bool ambiguous=false;
        while(queue_pending(queue,current,&ambiguous)) {
            if(ambiguous || advances>=JCE_READBACK_DESTROY_ADVANCE_LIMIT) break;
            uint32_t next=jce_renderer_submit_frame(queue->renderer);
            ++advances;
            if(jce_renderer_frame_order(next,current)!=JCE_RENDERER_FRAME_AFTER) break;
            current=next;
        }
        if(queue_pending(queue,current,&ambiguous)) {
            queue->destroy_requested=true;queue_retire(queue);
            LOG_WARN(LOG_TAG,"queue destroy deferred until backend stop: advances=%u ambiguous=%d",
                (unsigned)advances,ambiguous ? 1 : 0);
            readback_trace_ownership();
            return;
        }
    }
    queue_unlink(queue);
    queue_free_storage(queue);
    readback_trace_ownership();
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
    if (!queue || queue->closing || queue->renderer!=s_readback_renderer || !desc || !out_ticket ||
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
    slot->byte_size=byte_size;++s_readback_cpu_buffers;s_readback_cpu_bytes+=byte_size;
    slot->staging = bgfx_create_texture_2d(
        (uint16_t)desc->width, (uint16_t)desc->height, false, 1,
        native_format,
        BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK |
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
            BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
        NULL, 0);
    if (slot->staging.idx == UINT16_MAX) {
        slot_dispose(slot);readback_trace_ownership();
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
    slot->trace_ticket=*out_ticket;
    readback_trace_current(slot,READBACK_TRACE_SUBMIT);
    readback_trace_ownership();
    return true;
}

JceRenderReadbackState jce_render_readback_poll(
    JceRenderReadbackQueue *queue, JceRenderReadbackTicket ticket)
{
    JceRenderReadbackSlot *slot = find_slot(queue, ticket);
    uint32_t frame;
    if (!slot) return JCE_RENDER_READBACK_INVALID;
    if(queue->closing) return (JceRenderReadbackState)slot->state;
    if (slot->state != JCE_RENDER_READBACK_PENDING &&
        slot->state != JCE_RENDER_READBACK_CANCELLED)
        return (JceRenderReadbackState)slot->state;
    frame = queue->renderer ? jce_renderer_get_frame_index(queue->renderer) : 0u;
    if (jce_renderer_frame_ready(frame,slot->ready_frame)) {
        readback_trace_operation(slot,READBACK_TRACE_READY,frame,true);
        if (slot->state == JCE_RENDER_READBACK_CANCELLED) {
            if(slot->pixels) {slot_dispose(slot);readback_trace_ownership();}
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
    if(queue->closing && queue->backend_owner) return true;
    if (slot->state == JCE_RENDER_READBACK_PENDING) {
        slot->state = JCE_RENDER_READBACK_CANCELLED;
        readback_trace_current(slot,READBACK_TRACE_CANCEL);
        readback_trace_ownership();
        return true;
    }
    if (slot->state == JCE_RENDER_READBACK_READY ||
        slot->state == JCE_RENDER_READBACK_FAILED) {
        slot_dispose(slot);
        slot->state = JCE_RENDER_READBACK_CANCELLED;
        readback_trace_ownership();
        return true;
    }
    return slot->state == JCE_RENDER_READBACK_CANCELLED;
}

bool jce_render_readback_release(JceRenderReadbackQueue *queue,
                                 JceRenderReadbackTicket ticket)
{
    JceRenderReadbackSlot *slot = find_slot(queue, ticket);
    if (!slot) return false;
    if(queue->closing && queue->backend_owner) return true;
    if (slot->state == JCE_RENDER_READBACK_PENDING ||
        slot->state == JCE_RENDER_READBACK_CANCELLED) {
        /* Cancellation changes consumer intent, not bgfx's CPU borrow.
         * A repeated release/cancel must retain the same destination until
         * the promised completion frame has actually been observed. */
        uint32_t frame = queue->renderer ?
            jce_renderer_get_frame_index(queue->renderer) : 0u;
        if (slot->state!=JCE_RENDER_READBACK_CANCELLED) {
            slot->state=JCE_RENDER_READBACK_CANCELLED;
            readback_trace_current(slot,READBACK_TRACE_CANCEL);
        }
        if (!jce_renderer_frame_ready(frame,slot->ready_frame)) return true;
        readback_trace_operation(slot,READBACK_TRACE_READY,frame,true);
    }
    slot_reset(slot);
    readback_trace_ownership();
    return true;
}
