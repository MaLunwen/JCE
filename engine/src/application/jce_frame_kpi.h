#ifndef JCE_FRAME_KPI_INTERNAL_H
#define JCE_FRAME_KPI_INTERNAL_H
#include <SDL3/SDL_iostream.h>
#include <jce/renderer/jce_renderer.h>

typedef struct {
    char *bytes;
    size_t capacity;
} JceFrameKpiBuffer;

/* Main-thread only. Reuses the buffer and performs one complete stream write.
 * Invalid CSV phase names or incomplete output fail rather than truncating. */
bool jce_frame_kpi_export(JceFrameKpiBuffer *buffer, SDL_IOStream *stream,
                          uint64_t frame, double wall_ms, const JceGpuStats *stats);
void jce_frame_kpi_buffer_destroy(JceFrameKpiBuffer *buffer);
#endif
