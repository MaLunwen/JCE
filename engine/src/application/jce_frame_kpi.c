/* Batch complete frame records through the existing phase snapshot authority. */
#include "jce_frame_kpi.h"
#include <jce/os/core/jce_perf_phase.h>
#include "os/core/jce_memory.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool append_row(JceFrameKpiBuffer *buffer, size_t *used, uint64_t frame,
                        const char *name, double ms)
{
    if (!name || strpbrk(name, ",\r\n\"") || !isfinite(ms) || ms < 0) return false;
    for (;;) {
        size_t available = buffer->capacity - *used;
        int length = snprintf(available ? buffer->bytes + *used : NULL, available,
                              "%llu,%s,%.4f\n", (unsigned long long)frame, name, ms);
        if (length < 0 || *used > SIZE_MAX - (size_t)length - 1u) return false;
        if ((size_t)length < available) {
            *used += (size_t)length;
            return true;
        }
        size_t needed = *used + (size_t)length + 1u;
        size_t capacity = buffer->capacity ? buffer->capacity : 8192u;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2u) { capacity = needed; break; }
            capacity *= 2u;
        }
        char *bytes = JCE_REALLOC(buffer->bytes, capacity);
        if (!bytes) return false;
        buffer->bytes = bytes;
        buffer->capacity = capacity;
    }
}

bool jce_frame_kpi_export(JceFrameKpiBuffer *buffer, SDL_IOStream *stream,
                          uint64_t frame, double wall_ms, const JceGpuStats *stats)
{
    if (!buffer || !stream) return false;
    size_t used = 0;
    if (jce_perf_phase_frame_has_data()) {
        int count = jce_perf_phase_count();
        for (int index = 0; index < count; ++index) {
            const char *name = NULL;
            double ms = 0;
            if (jce_perf_phase_peek_frame(index, &name, &ms) && name &&
                !append_row(buffer, &used, frame, name, ms)) return false;
        }
    }
    if (!append_row(buffer, &used, frame, "frame_wall", wall_ms)) return false;
    if (stats && stats->valid &&
        (!append_row(buffer, &used, frame, "frame_cpu", stats->cpu_frame_ms) ||
         !append_row(buffer, &used, frame, "frame_gpu", stats->gpu_ms))) return false;
    return SDL_WriteIO(stream, buffer->bytes, used) == used;
}

void jce_frame_kpi_buffer_destroy(JceFrameKpiBuffer *buffer)
{
    if (!buffer) return;
    JCE_FREE(buffer->bytes);
    memset(buffer, 0, sizeof(*buffer));
}
