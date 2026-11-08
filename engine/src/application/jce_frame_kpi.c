/* Batch complete frame records through the existing phase snapshot authority. */
#include "jce_frame_kpi.h"
#include "jce_engine_kpi_clock.h"
#include <jce/os/core/jce_perf_phase.h>
#include "os/core/jce_memory.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool append_row(JceFrameKpiBuffer *buffer, size_t *used, const char *frame_prefix,
                        const char *name, double ms, const char *suffix)
{
    if (!name || !name[0] || strpbrk(name, ",\r\n\"") || !isfinite(ms) || ms < 0) return false;
    for (;;) {
        size_t available = buffer->capacity - *used;
        int length = snprintf(available ? buffer->bytes + *used : NULL, available,
                              "%s%s,%.4f%s\n", frame_prefix, name, ms, suffix);
        if (length < 0 || *used > SIZE_MAX - (size_t)length - 1u) return false;
        size_t needed = *used + (size_t)length + 1u;
        if (buffer->max_capacity && needed > buffer->max_capacity) return false;
        if ((size_t)length < available) {
            *used += (size_t)length;
            return true;
        }
        JceFrameKpiScope reserve = jce_frame_kpi_scope_begin("kpi_reserve");
        if (reserve.sampled) ++buffer->last_profile.reserve_calls;
        size_t capacity = buffer->capacity ? buffer->capacity : 8192u;
        if (buffer->max_capacity && capacity > buffer->max_capacity)
            capacity = buffer->max_capacity;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2u) { capacity = needed; break; }
            capacity *= 2u;
            if (buffer->max_capacity && capacity > buffer->max_capacity)
                capacity = buffer->max_capacity;
        }
        char *bytes = JCE_REALLOC(buffer->bytes, capacity);
        if (!bytes) {
            buffer->last_profile.reserve_ms += jce_frame_kpi_scope_end(reserve, false);
            return false;
        }
        buffer->bytes = bytes;
        buffer->capacity = capacity;
        buffer->last_profile.reserve_ms += jce_frame_kpi_scope_end(reserve, true);
    }
}

const char *jce_frame_kpi_csv_header(bool context_json)
{
    return context_json ? "frame_index,phase,ms,context_json\n" :
                          "frame_index,phase,ms\n";
}

static void context_uint(char out[16], bool valid, uint32_t value)
{
    if (valid) (void)snprintf(out, 16, "%u", (unsigned)value);
    else memcpy(out, "null", 5);
}

static void context_size(char out[32], bool present, uint32_t width, uint32_t height)
{
    if (present && width && height)
        (void)snprintf(out, 32, "[%u,%u]", (unsigned)width, (unsigned)height);
    else memcpy(out, "null", 5);
}

/* Fixed keys and integer/null values need no arbitrary-string JSON escaping.
 * The surrounding CSV field doubles every JSON quote per CSV rules. All
 * scratch is bounded on the stack; steady-state capture adds no allocation. */
static bool context_suffix(const JceFrameKpiContext *context, char *out, size_t capacity)
{
    char flags_begin[16], flags_end[16], backend_begin[16], backend_end[16];
    char point[32], drawable[32], native_begin[32], native_end[32];
    context_uint(flags_begin, context->window_present && context->flags_begin_valid, context->flags_begin);
    context_uint(flags_end, context->window_present && context->flags_end_valid, context->flags_end);
    context_uint(backend_begin, context->backend_frame_begin_valid, context->backend_frame_begin);
    context_uint(backend_end, context->backend_frame_end_valid, context->backend_frame_end);
    context_size(point, context->window_present, context->point_width, context->point_height);
    context_size(drawable, context->window_present, context->drawable_width, context->drawable_height);
    context_size(native_begin, context->window_present, context->native_width_begin, context->native_height_begin);
    context_size(native_end, context->window_present, context->native_width_end, context->native_height_end);
    const int length = snprintf(out, capacity,
        ",\"{\"\"v\"\":1,\"\"window_present\"\":%s,\"\"flags_begin\"\":%s,\"\"flags_end\"\":%s,"
        "\"\"point_size\"\":%s,\"\"drawable_size\"\":%s,\"\"native_size_begin\"\":%s,"
        "\"\"native_size_end\"\":%s,\"\"backend_frame_begin\"\":%s,\"\"backend_frame_end\"\":%s}\"",
        context->window_present ? "true" : "false", flags_begin, flags_end,
        point, drawable, native_begin, native_end, backend_begin, backend_end);
    return length >= 0 && (size_t)length < capacity;
}

static bool viewport_size_valid(uint32_t flags, uint32_t bit, uint32_t w, uint32_t h)
{
    return flags & bit ? w != 0 && h != 0 : w == 0 && h == 0;
}

bool jce_frame_kpi_viewport_valid(const JceFrameViewportObservation *v)
{
    if (!v || v->struct_size < sizeof *v || v->struct_size > 4096 ||
        v->struct_size % sizeof(uint32_t) || !v->source_id || (v->flags & ~255u) ||
        v->cpu_outcome > JCE_FRAME_VIEWPORT_CPU_COMPLETED) return false;
    if (!viewport_size_valid(v->flags, JCE_FRAME_VIEWPORT_REQUESTED_SIZE_VALID,
            v->requested_width, v->requested_height) ||
        !viewport_size_valid(v->flags, JCE_FRAME_VIEWPORT_ALLOCATED_SIZE_VALID,
            v->allocated_width, v->allocated_height) ||
        !viewport_size_valid(v->flags, JCE_FRAME_VIEWPORT_DISPLAY_SIZE_VALID,
            v->display_width, v->display_height)) return false;
    if (v->flags & JCE_FRAME_VIEWPORT_TEXTURE_VALID) {
        if (v->texture_index >= UINT16_MAX) return false;
    } else if (v->texture_index) return false;
    if (!(v->flags & JCE_FRAME_VIEWPORT_IMAGE_COUNT_VALID) && v->image_count) return false;
    if (!(v->flags & JCE_FRAME_VIEWPORT_QUEUE_COUNT_VALID) && v->queued_commands) return false;
    if (!(v->flags & JCE_FRAME_VIEWPORT_SUBMIT_COUNTS_VALID) &&
        (v->main_submits || v->recording_submits)) return false;
    return true;
}

bool jce_frame_kpi_context_report(JceFrameKpiContext *context,
    const JceFrameViewportObservation *observations, uint32_t count, size_t stride)
{
    JceFrameViewportObservation copied[4];
    if (!context || (context->version != 2 && context->version != 3) ||
        context->viewport_reported || count > 4)
        return false;
    if (count) {
        if (!observations || (uintptr_t)observations % sizeof(uint32_t) ||
            stride < sizeof *observations || stride > 4096 || stride % sizeof(uint32_t))
            return false;
        for (uint32_t i = 0; i < count; ++i) {
            const unsigned char *source = (const unsigned char *)observations + i * stride;
            uint32_t declared_size = 0;
            memcpy(&declared_size, source, sizeof declared_size);
            if (declared_size < sizeof copied[i] || declared_size > stride ||
                declared_size % sizeof(uint32_t)) return false;
            memcpy(&copied[i], source, sizeof copied[i]);
            if (!jce_frame_kpi_viewport_valid(&copied[i])) return false;
            copied[i].struct_size = sizeof copied[i];
            for (uint32_t j = 0; j < i; ++j)
                if (copied[j].source_id == copied[i].source_id) return false;
        }
    } else if (observations || (stride &&
        (stride < sizeof *observations || stride > 4096 || stride % sizeof(uint32_t)))) return false;
    if (count) memcpy(context->viewports, copied, count * sizeof copied[0]);
    context->viewport_count = count;
    context->viewport_reported = true;
    return true;
}

/* Reuse the exact v1 window encoder, extending only the explicit v2 object.
 * At most four uint32-only records fit this fixed 4096-byte CSV suffix. This
 * does not enlarge the existing 256KiB/1MiB writer or reusable scratch budget. */
static bool context_suffix_v2(const JceFrameKpiContext *context, char *out, size_t capacity)
{
    if (context->viewport_count > 4 || (!context->viewport_reported && context->viewport_count) ||
        !context_suffix(context, out, capacity)) return false;
    size_t used = strlen(out);
    char *version = strstr(out, "\"\"v\"\":1");
    if (!version || used < 2) return false;
    version[6] = '2';
    used -= 2; /* Remove the v1 JSON close and the outer CSV close quote. */
    int length = snprintf(out + used, capacity - used,
        ",\"\"viewport_reported\"\":%s,\"\"viewports\"\":[",
        context->viewport_reported ? "true" : "false");
    if (length < 0 || (size_t)length >= capacity - used) return false;
    used += (size_t)length;
    for (uint32_t i = 0; i < context->viewport_count; ++i) {
        const JceFrameViewportObservation *v = &context->viewports[i];
        if (!jce_frame_kpi_viewport_valid(v)) return false;
        for (uint32_t j = 0; j < i; ++j)
            if (context->viewports[j].source_id == v->source_id) return false;
        char requested[32], allocated[32], display[32], texture[16];
        char images[16], queued[16], main_submits[16], recording_submits[16];
        context_size(requested, (v->flags & JCE_FRAME_VIEWPORT_REQUESTED_SIZE_VALID) != 0,
            v->requested_width, v->requested_height);
        context_size(allocated, (v->flags & JCE_FRAME_VIEWPORT_ALLOCATED_SIZE_VALID) != 0,
            v->allocated_width, v->allocated_height);
        context_size(display, (v->flags & JCE_FRAME_VIEWPORT_DISPLAY_SIZE_VALID) != 0,
            v->display_width, v->display_height);
        context_uint(texture, (v->flags & JCE_FRAME_VIEWPORT_TEXTURE_VALID) != 0, v->texture_index);
        context_uint(images, (v->flags & JCE_FRAME_VIEWPORT_IMAGE_COUNT_VALID) != 0, v->image_count);
        context_uint(queued, (v->flags & JCE_FRAME_VIEWPORT_QUEUE_COUNT_VALID) != 0, v->queued_commands);
        context_uint(main_submits, (v->flags & JCE_FRAME_VIEWPORT_SUBMIT_COUNTS_VALID) != 0, v->main_submits);
        context_uint(recording_submits, (v->flags & JCE_FRAME_VIEWPORT_SUBMIT_COUNTS_VALID) != 0, v->recording_submits);
        length = snprintf(out + used, capacity - used,
            "%s{\"\"source_id\"\":%u,\"\"flags\"\":%u,\"\"cpu_outcome\"\":%u,"
            "\"\"refusal_reason\"\":%u,\"\"requested_size\"\":%s,\"\"allocated_size\"\":%s,"
            "\"\"display_size\"\":%s,\"\"texture_index\"\":%s,\"\"image_count\"\":%s,"
            "\"\"queued_commands\"\":%s,\"\"main_submits\"\":%s,\"\"recording_submits\"\":%s}",
            i ? "," : "", (unsigned)v->source_id, (unsigned)v->flags,
            (unsigned)v->cpu_outcome, (unsigned)v->refusal_reason, requested, allocated,
            display, texture, images, queued, main_submits, recording_submits);
        if (length < 0 || (size_t)length >= capacity - used) return false;
        used += (size_t)length;
    }
    length = snprintf(out + used, capacity - used, "]}\"");
    return length >= 0 && (size_t)length < capacity - used;
}

/* Keep v1/v2 formatting untouched. V3 extends their same bounded suffix with
 * the exact borrowed interval. Decimal strings preserve every uint64 tick. */
static bool context_suffix_v3(const JceFrameKpiContext *context, double wall_ms,
    char *out, size_t capacity)
{
    double clock_ms = 0.0;
    if (!context->clock_valid || !isfinite(wall_ms) || wall_ms < 0.0 ||
        !jce_engine_frame_clock_wall_ms(context->clock_frequency,
            context->clock_begin, context->clock_end, &clock_ms) ||
        fabs(clock_ms - wall_ms) > 0.0000501 + 1e-12 * fmax(fabs(clock_ms), fabs(wall_ms)) ||
        !context_suffix_v2(context, out, capacity)) return false;
    const size_t used = strlen(out);
    char *version = strstr(out, "\"\"v\"\":2");
    if (!version || used < 2) return false;
    version[6] = '3';
    const int length = snprintf(out + used - 2, capacity - used + 2,
        ",\"\"clock\"\":{\"\"domain\"\":\"\"jce-performance-counter\"\","
        "\"\"frequency_hz\"\":\"\"%llu\"\",\"\"begin\"\":\"\"%llu\"\","
        "\"\"end\"\":\"\"%llu\"\"}}\"",
        (unsigned long long)context->clock_frequency,
        (unsigned long long)context->clock_begin, (unsigned long long)context->clock_end);
    return length >= 0 && (size_t)length < capacity - used + 2;
}

/* Preserve the original single traversal and dynamic growth. */
static bool encode_rows(JceFrameKpiBuffer *buffer, uint64_t frame,
                         double wall_ms, const JceGpuStats *stats,
                         bool phases_eligible, const JceFrameKpiContext *context,
                         size_t *out_size)
{
    size_t used = 0;
    char wall_suffix[4096];
    const char *empty_suffix = context ? "," : "";
    if (context) {
        if (context->version > 3) return false;
        if (context->version == 3) {
            if (!context_suffix_v3(context, wall_ms, wall_suffix, sizeof wall_suffix)) return false;
        } else if (context->version == 2) {
            if (!context_suffix_v2(context, wall_suffix, sizeof wall_suffix)) return false;
        } else if (!context_suffix(context, wall_suffix, sizeof wall_suffix)) return false;
    }
    if (buffer->max_capacity && buffer->capacity > buffer->max_capacity) return false;
    /* One uint64 conversion per packet, including every growth retry. */
    char frame_prefix[22]; /* 20 decimal digits, comma and terminating NUL. */
    int prefix_length = snprintf(frame_prefix, sizeof frame_prefix, "%llu,",
                                 (unsigned long long)frame);
    if (prefix_length < 0 || (size_t)prefix_length >= sizeof frame_prefix) return false;
    if (phases_eligible && jce_perf_phase_enabled() && jce_perf_phase_frame_has_data()) {
        int count = jce_perf_phase_count();
        for (int index = 0; index < count; ++index) {
            const char *name = NULL;
            double ms = 0;
            if (jce_perf_phase_peek_frame(index, &name, &ms) &&
                !append_row(buffer, &used, frame_prefix, name, ms, empty_suffix)) return false;
        }
    }
    if (!append_row(buffer, &used, frame_prefix, "frame_wall", wall_ms,
                    context ? wall_suffix : "")) return false;
    if (stats && stats->valid) {
        /* These timers are optional even when the snapshot is valid. */
        if (isfinite(stats->cpu_frame_ms) && stats->cpu_frame_ms >= 0 &&
            !append_row(buffer, &used, frame_prefix, "frame_cpu", stats->cpu_frame_ms, empty_suffix))
            return false;
        if (isfinite(stats->gpu_ms) && stats->gpu_ms >= 0 &&
            !append_row(buffer, &used, frame_prefix, "frame_gpu", stats->gpu_ms, empty_suffix))
            return false;
    }
    if (buffer->backend_timings && stats && stats->valid) {
        /* Reuse the caller's one stats snapshot. No extra backend query or
         * frame join: these three latched timers belong to different threads.
         * Missing/invalid observations remain absent, never invented zeroes.
         * A finite zero is retained but cannot prove timer availability. */
        static const char *const names[] = {
            "frame_backend_latched_cpu_submit", "frame_backend_latched_wait_submit",
            "frame_backend_latched_wait_render"
        };
        const double values[] = {stats->cpu_submit_ms, stats->wait_submit_ms,
                                  stats->wait_render_ms};
        for (unsigned index = 0; index < 3; ++index) {
            if (isfinite(values[index]) && values[index] >= 0 &&
                !append_row(buffer, &used, frame_prefix, names[index], values[index], empty_suffix))
                return false;
        }
    }
    *out_size = used;
    return true;
}

bool jce_frame_kpi_encode(JceFrameKpiBuffer *buffer, uint64_t frame,
                          double wall_ms, const JceGpuStats *stats,
                          bool phases_eligible, size_t *out_size)
{
    return jce_frame_kpi_encode_with_context(buffer, frame, wall_ms, stats,
                                             phases_eligible, NULL, out_size);
}

bool jce_frame_kpi_encode_with_context(JceFrameKpiBuffer *buffer, uint64_t frame,
    double wall_ms, const JceGpuStats *stats, bool phases_eligible,
    const JceFrameKpiContext *context, size_t *out_size)
{
    if (out_size) *out_size = 0;
    if (!buffer || !out_size) return false;
    memset(&buffer->last_profile, 0, sizeof buffer->last_profile);
    JceFrameKpiScope scope = jce_frame_kpi_scope_begin("kpi_encode");
    buffer->last_profile.sampled = scope.sampled;
    buffer->last_profile.phase_requested = scope.phase_requested;
    buffer->last_profile.trace_requested = scope.trace.id != 0;
    buffer->last_profile.encode_calls = scope.sampled ? 1u : 0u;
    bool succeeded = encode_rows(buffer, frame, wall_ms, stats, phases_eligible, context, out_size);
    buffer->last_profile.encode_ms = jce_frame_kpi_scope_end(scope, succeeded);
    return succeeded;
}

bool jce_frame_kpi_export(JceFrameKpiBuffer *buffer, SDL_IOStream *stream,
                          uint64_t frame, double wall_ms, const JceGpuStats *stats, bool phases_eligible)
{
    if (!buffer || !stream) return false;
    memset(&buffer->last_profile, 0, sizeof buffer->last_profile);
    JceFrameKpiScope encoded = jce_frame_kpi_scope_begin("kpi_snapshot_encode_write");
    buffer->last_profile.sampled = encoded.sampled;
    buffer->last_profile.phase_requested = encoded.phase_requested;
    buffer->last_profile.trace_requested = encoded.trace.id != 0;
    buffer->last_profile.snapshot_encode_write_calls = encoded.sampled ? 1u : 0u;
    size_t used = 0;
    bool succeeded = encode_rows(buffer, frame, wall_ms, stats, phases_eligible, NULL, &used);
    if (succeeded) {
        JceFrameKpiScope write = jce_frame_kpi_scope_begin("kpi_write");
        buffer->last_profile.write_calls = write.sampled ? 1u : 0u;
        succeeded = SDL_WriteIO(stream, buffer->bytes, used) == used;
        buffer->last_profile.write_ms = jce_frame_kpi_scope_end(write, succeeded);
    }
    buffer->last_profile.snapshot_encode_write_ms =
        jce_frame_kpi_scope_end(encoded, succeeded);
    return succeeded;
}

void jce_frame_kpi_buffer_destroy(JceFrameKpiBuffer *buffer)
{
    if (!buffer) return;
    JCE_FREE(buffer->bytes);
    memset(buffer, 0, sizeof(*buffer));
}
