/* Private capture lifecycle. The engine owns one instance per application. */
#ifndef JCE_ENGINE_KPI_CAPTURE_INTERNAL_H
#define JCE_ENGINE_KPI_CAPTURE_INTERNAL_H
#include "jce_frame_kpi.h"
#include "jce_frame_kpi_writer.h"
#include <jce/application/jce_engine.h>

/* Private atomic selector: finite bytes,records; an invalid request leaves the
 * initialized defaults untouched. This does not bound physical IO latency. */
static inline bool jce_engine_kpi_capture_queue_configure(
    JceFrameKpiWriterConfig *config, const char *selector)
{
    if (!config) return false;
    if (!selector || !selector[0]) return true;
    const uint32_t maxima[2] = {67108864u, 65536u};
    uint32_t values[2] = {0, 0};
    const char *cursor = selector;
    for (unsigned part = 0; part < 2; ++part) {
        if (*cursor < '0' || *cursor > '9') return false;
        do {
            uint32_t digit = (uint32_t)(*cursor - '0');
            if (values[part] > (maxima[part] - digit) / 10u) return false;
            values[part] = values[part] * 10u + digit;
            ++cursor;
        } while (*cursor >= '0' && *cursor <= '9');
        if (!values[part]) return false;
        if (part == 0) {
            if (*cursor++ != ',') return false;
        } else if (*cursor) return false;
    }
    config->byte_capacity = (size_t)values[0];
    config->record_capacity = values[1];
    return true;
}

typedef struct {
    JceFrameKpiWriter *writer;
    JceFrameKpiBuffer buffer;
    uint64_t index;
    uint32_t limit;
    bool failed, reported;
    bool context_enabled, context_begun, context_ended, context_sealed;
    bool viewport_draw_active;
    uint32_t context_version;
    JceFrameKpiContext context;
    const char *failure_reason; /* Static first-party diagnostic text. */
} JceEngineKpiCapture;

/* Empty path is disabled and performs no allocation or stream IO. The owner
 * must finish before returning a terminal application result or cancelling
 * its borrowed executor. Failed requested capture rejects initialization. */
bool jce_engine_kpi_capture_open(JceEngineKpiCapture *capture, const char *path,
    JceAsyncExecutor *executor, bool low_memory, uint32_t limit,
    uint32_t durable_interval);
/* Empty/"0" keeps legacy bytes; "1" selects window v1, "2" adds host viewports
 * and "3" additionally includes the existing completed performance interval.
 * JCE_KPI_BACKEND_TIMINGS is also read once here: empty/"0" preserves bytes,
 * "1" opts into latest-latched backend rows. An unknown nonempty selector
 * rejects a requested capture before stream IO. Neither option adds a getter.
 * JCE_KPI_WRITER_QUEUE is latched here before stream IO/allocation: empty keeps
 * the normal/low-memory defaults; positive decimal bytes,records may explicitly
 * select up to 64 MiB/65536 descriptors. Invalid requests fail initialization. */
bool jce_engine_kpi_capture_open_with_context(JceEngineKpiCapture *capture, const char *path,
    JceAsyncExecutor *executor, bool low_memory, uint32_t limit,
    uint32_t durable_interval, const char *context_selector);
/* True only while this optional capture still admits new completed records. */
bool jce_engine_kpi_capture_context_active(const JceEngineKpiCapture *capture);
/* V3 only: borrow already sampled Engine counters at completed export. An
 * invalid requested interval fails capture; legacy callers do not enter. */
bool jce_engine_kpi_capture_interval(JceEngineKpiCapture *capture,
    uint64_t frequency, uint64_t begin, uint64_t end);
bool jce_engine_kpi_capture_export(JceEngineKpiCapture *capture,
    double wall_ms, bool phases_eligible);
bool jce_engine_kpi_capture_poll(JceEngineKpiCapture *capture);
bool jce_engine_kpi_capture_finish(JceEngineKpiCapture *capture);
JceAppResult jce_engine_kpi_capture_result(JceEngineKpiCapture *capture,
                                          JceAppResult result);
/* False means active callback/non-owner cleanup was rejected; retain this
 * owner and retry after its current operation. Capture failure alone does not
 * prevent safe cleanup. True releases ownership and resets the instance. */
bool jce_engine_kpi_capture_destroy(JceEngineKpiCapture *capture);
#endif
