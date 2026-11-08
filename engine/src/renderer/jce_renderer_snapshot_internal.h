/* Typed backend observations shared by production and CPU-only controls. */
#ifndef JCE_RENDERER_SNAPSHOT_INTERNAL_H
#define JCE_RENDERER_SNAPSHOT_INTERNAL_H

#include "os/core/jce_trace_scope_internal.h"
#include <jce/os/core/jce_log.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct JceRendererSnapshot {
    uint32_t draw_count;
    uint32_t compute_count;
    uint16_t texture_count;
    uint16_t framebuffer_count;
    uint16_t program_count;
    uint16_t shader_count;
    uint16_t vertex_buffer_count;
    uint16_t index_buffer_count;
    int32_t transient_vb_used_bytes;
    int32_t transient_ib_used_bytes;
    int64_t texture_estimate_bytes;
    int64_t rt_estimate_bytes;
    int64_t adapter_reported_used_bytes;
    int64_t gpu_timer_frequency;
    int64_t gpu_time_begin;
    int64_t gpu_time_end;
    uint8_t encoder_frame_used;
    uint16_t adapter_reported_vendor_id;
    uint16_t adapter_reported_device_id;
    uint32_t bgfx_renderer_type;
    uint32_t host_submission_ordinal;
    uint64_t device_epoch; /* Existing upload owner context_generation. */
    uint32_t gpu_frame_num; /* Provider frame which generated GPU timestamps. */
    uint16_t gpu_view_count;
    bool gpu_view_data_available;
} JceRendererSnapshot;

typedef struct JceRendererViewSnapshot {
    uint16_t view_id;
    uint32_t gpu_frame_num;
    int64_t gpu_time_begin;
    int64_t gpu_time_end;
} JceRendererViewSnapshot;

/* A plausible nonempty provider interval, not a fence/query completion proof.
 * Zero source IDs and begin=0 are valid. All-zero initialization is ambiguous.
 * Check before subtracting to avoid signed overflow on malformed observations. */
static inline bool jce_renderer_gpu_interval_available(int64_t frequency,
    int64_t begin, int64_t end)
{
    return frequency > 0 && begin >= 0 && end >= begin && (begin != 0 || end != 0);
}

static inline bool jce_renderer_snapshot_epoch_available(uint64_t epoch)
{
    return epoch != 0 && epoch <= (uint64_t)INT64_MAX;
}

static inline bool jce_renderer_snapshot_due(uint32_t frame_index)
{
    return (frame_index % 60u) == 0u;
}

/* The private ordinal counts actual host submissions, independent of bgfx
 * initialization frame indices; first submission supplements the old cadence. */
static inline bool jce_renderer_snapshot_due_at_submission(uint32_t frame_index,
    uint32_t submission_ordinal)
{
    return submission_ordinal == 1u || jce_renderer_snapshot_due(frame_index);
}

/* The backend's most recent frame may trail this CPU submission index.
 * Encoder usage includes the reserved default/API slot 0, even without draws,
 * plus successful new handle acquisitions until the next frame boundary.
 * It counts neither OS threads nor peak simultaneous occupancy. Capacity is
 * effective after backend clamping. The borrowed encoder_capacity pointer
 * also identifies whether the shared capability snapshot is available.
 * Adapter IDs are selected backend observations; renderer type is the raw
 * bgfx enum, not JceRendererBackend. Valid zero values stay zero; a missing
 * capability snapshot remains unavailable rather than requested. */
static inline void jce_renderer_snapshot_emit(uint32_t frame_index,
    const JceRendererSnapshot *snapshot, const uint32_t *encoder_capacity)
{
    if (!snapshot || !jce_trace_enabled()) return;
#define JCE_RENDER_COUNTER(name_, value_) \
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, name_, (int64_t)(value_))
    JCE_RENDER_COUNTER("renderer.cpu_submission_index", frame_index);
    JCE_RENDER_COUNTER("renderer.draw_count", snapshot->draw_count);
    JCE_RENDER_COUNTER("renderer.compute_count", snapshot->compute_count);
    JCE_RENDER_COUNTER("renderer.texture_count", snapshot->texture_count);
    JCE_RENDER_COUNTER("renderer.framebuffer_count", snapshot->framebuffer_count);
    JCE_RENDER_COUNTER("renderer.program_count", snapshot->program_count);
    JCE_RENDER_COUNTER("renderer.shader_count", snapshot->shader_count);
    JCE_RENDER_COUNTER("renderer.vertex_buffer_count", snapshot->vertex_buffer_count);
    JCE_RENDER_COUNTER("renderer.index_buffer_count", snapshot->index_buffer_count);
    JCE_RENDER_COUNTER("renderer.transient_vb_used_bytes", snapshot->transient_vb_used_bytes);
    JCE_RENDER_COUNTER("renderer.transient_ib_used_bytes", snapshot->transient_ib_used_bytes);
    JCE_RENDER_COUNTER("renderer.texture_estimate_bytes", snapshot->texture_estimate_bytes);
    JCE_RENDER_COUNTER("renderer.rt_estimate_bytes", snapshot->rt_estimate_bytes);
    JCE_RENDER_COUNTER("gpu.adapter_reported_used_bytes",
        snapshot->adapter_reported_used_bytes == INT64_MAX
            ? -1 : snapshot->adapter_reported_used_bytes);
    JCE_RENDER_COUNTER("gpu.timer_frequency", snapshot->gpu_timer_frequency);
    const bool interval_available = jce_renderer_gpu_interval_available(
        snapshot->gpu_timer_frequency, snapshot->gpu_time_begin, snapshot->gpu_time_end);
    JCE_RENDER_COUNTER("gpu.reported_frame_ticks", interval_available
        ? snapshot->gpu_time_end - snapshot->gpu_time_begin : -1);
    JCE_RENDER_COUNTER("renderer.encoder_frame_used", snapshot->encoder_frame_used);
    JCE_RENDER_COUNTER("renderer.encoder_effective_capacity",
        encoder_capacity ? (int64_t)*encoder_capacity : -1);
    JCE_RENDER_COUNTER("gpu.adapter_reported_vendor_id",
        encoder_capacity ? (int64_t)snapshot->adapter_reported_vendor_id : -1);
    JCE_RENDER_COUNTER("gpu.adapter_reported_device_id",
        encoder_capacity ? (int64_t)snapshot->adapter_reported_device_id : -1);
    JCE_RENDER_COUNTER("gpu.bgfx_renderer_type",
        encoder_capacity ? (int64_t)snapshot->bgfx_renderer_type : -1);
    /* Keep the old CPU counter spelling, but expose its actual API identity.
     * None of these IDs promises full-device completion or resource retirement. */
    JCE_RENDER_COUNTER("renderer.api_frame_number", frame_index);
    JCE_RENDER_COUNTER("renderer.host_submission_ordinal", snapshot->host_submission_ordinal);
    JCE_RENDER_COUNTER("renderer.device_epoch", jce_renderer_snapshot_epoch_available(snapshot->device_epoch)
        ? (int64_t)snapshot->device_epoch : -1);
    JCE_RENDER_COUNTER("renderer.device_epoch_available", jce_renderer_snapshot_epoch_available(snapshot->device_epoch));
    JCE_RENDER_COUNTER("gpu.source_frame_number", snapshot->gpu_frame_num);
    JCE_RENDER_COUNTER("gpu.raw_begin_ticks", snapshot->gpu_time_begin);
    JCE_RENDER_COUNTER("gpu.raw_end_ticks", snapshot->gpu_time_end);
    JCE_RENDER_COUNTER("gpu.interval_available", interval_available);
    JCE_RENDER_COUNTER("gpu.view_count", snapshot->gpu_view_count);
    JCE_RENDER_COUNTER("gpu.view_data_available", snapshot->gpu_view_data_available);
#undef JCE_RENDER_COUNTER
}


/* Borrowed provider view data is emitted synchronously at the same sample node.
 * No per-view registry/history or heap allocation; identity is scoped by epoch,
 * view ID and provider source frame. Readers may recognize repeated latched
 * samples; CPU/API frame advancement alone never means a new GPU interval.
 * Stack-built names go only to the canonical ring, which copies synchronously;
 * optional profiler plots may borrow names asynchronously and require static names. */
static inline void jce_renderer_snapshot_emit_view(uint32_t frame_index,
    const JceRendererSnapshot *snapshot, const JceRendererViewSnapshot *view)
{
    if (!snapshot || !view || !snapshot->gpu_view_data_available || !jce_trace_enabled()) return;
    const uint64_t span = jce_trace_task_current_id();
    char name[48];
#define JCE_VIEW_COUNTER(suffix_, value_) \
    do { \
        (void)snprintf(name, sizeof(name), "gpu.view.%u.%s", (unsigned)view->view_id, suffix_); \
        jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER, name, (int64_t)(value_), span, span); \
    } while (0)
    JCE_VIEW_COUNTER("device_epoch", jce_renderer_snapshot_epoch_available(snapshot->device_epoch)
        ? (int64_t)snapshot->device_epoch : -1);
    JCE_VIEW_COUNTER("api_frame", frame_index);
    JCE_VIEW_COUNTER("submission_ordinal", snapshot->host_submission_ordinal);
    JCE_VIEW_COUNTER("source_frame", view->gpu_frame_num);
    JCE_VIEW_COUNTER("begin_ticks", view->gpu_time_begin);
    JCE_VIEW_COUNTER("end_ticks", view->gpu_time_end);
    JCE_VIEW_COUNTER("timer_frequency", snapshot->gpu_timer_frequency);
    const bool available = jce_renderer_gpu_interval_available(snapshot->gpu_timer_frequency,
        view->gpu_time_begin, view->gpu_time_end);
    JCE_VIEW_COUNTER("interval_available", available);
    JCE_VIEW_COUNTER("reported_ticks", available ? view->gpu_time_end - view->gpu_time_begin : -1);
#undef JCE_VIEW_COUNTER
}

/* The logger's canonical message has a 512-byte payload. Keep the mapping plus
 * worst-case integer prefix below that bound. Entries are atomic: never emit
 * half a name or UTF-8 byte sequence as a complete provider name. Unsafe bytes
 * are percent encoded; this preserves bounded provider bytes, not an upstream
 * requested name that bgfx may itself have clipped to its 256-byte field. */
typedef struct JceRendererViewNames {
    char text[224];
    size_t used;
    uint16_t actual, listed, start;
    bool truncated, unavailable;
} JceRendererViewNames;
static inline JceRendererViewNames jce_renderer_view_names_begin(uint16_t actual,
    uint32_t ordinal, bool data_available)
{
    JceRendererViewNames names = {{0}, 0, actual, 0, 0, false, false};
    if (actual) names.start = (uint16_t)((ordinal / 60u) % actual);
    names.unavailable = actual != 0 && !data_available;
    names.truncated = names.unavailable;
    return names;
}
static inline bool jce_renderer_view_names_append(JceRendererViewNames *names,
    uint16_t view_id, uint32_t source_frame, const char *name, size_t capacity)
{
    if (!names || names->unavailable) return false;
    if (!name || capacity == 0 || capacity > 256 || names->listed >= names->actual) {
        names->unavailable = names->truncated = true;
        return false;
    }
    size_t length = 0;
    while (length < capacity && name[length]) ++length;
    if (length == capacity) {
        names->unavailable = names->truncated = true;
        return false;
    }
    char entry[800];
    const int prefix = snprintf(entry, sizeof(entry), "%u@%u=",
        (unsigned)view_id, source_frame);
    if (prefix < 0 || (size_t)prefix >= sizeof(entry)) {
        names->unavailable = names->truncated = true;
        return false;
    }
    size_t written = (size_t)prefix;
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)name[i];
        if (c > 32 && c < 127 && c != '%' && c != ';' && c != '=' &&
            c != '@' && c != '"' && c != '\\') entry[written++] = (char)c;
        else { entry[written++] = '%'; entry[written++] = hex[c >> 4]; entry[written++] = hex[c & 15u]; }
    }
    /* At most255 provider bytes become765 ASCII bytes, plus <=17 prefix. */
    const size_t separator = names->listed ? 1u : 0u;
    if (names->used + separator + written >= sizeof(names->text)) {
        names->truncated = true;
        return false;
    }
    if (separator) names->text[names->used++] = ';';
    memcpy(names->text + names->used, entry, written);
    names->used += written;
    names->text[names->used] = '\0';
    ++names->listed;
    names->truncated = names->listed < names->actual;
    return true;
}
static inline void jce_renderer_snapshot_log_view_names(uint32_t api_frame,
    const JceRendererSnapshot *snapshot, const JceRendererViewNames *names)
{
    if (!snapshot || !names) return;
    jce_log_write(JCE_LOG_LEVEL_INFO, "jce_renderer", __FILE__, __LINE__,
        "gpu-view-names: api_frame=%u submission=%u device_epoch=%llu source_frame=%u "
        "actual=%u listed=%u start=%u truncated=%u unavailable=%u names=%s",
        api_frame, snapshot->host_submission_ordinal, (unsigned long long)snapshot->device_epoch,
        snapshot->gpu_frame_num, (unsigned)names->actual, (unsigned)names->listed,
        (unsigned)names->start, names->truncated ? 1u : 0u, names->unavailable ? 1u : 0u, names->text);
}

#endif
