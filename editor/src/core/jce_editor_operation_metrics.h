/* Main-thread editor operation phases, shared with the existing profiler. */
#ifndef JCE_EDITOR_OPERATION_METRICS_H
#define JCE_EDITOR_OPERATION_METRICS_H
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_timer.h>
#include <stdint.h>
class JceEditorOperationScope {
    const char *name;
    uint64_t start;
public:
    explicit JceEditorOperationScope(const char *phase)
        : name(phase), start(jce_perf_phase_enabled() ? jce_time_perf_counter() : 0) {}
    ~JceEditorOperationScope() {
        if (start)
            jce_perf_phase_add(name, jce_time_perf_to_ms(start, jce_time_perf_counter()));
    }
};
void jce_editor_operation_tick(void);
void jce_editor_inspector_metric_request(uint32_t focused);
void jce_editor_inspector_metric_complete(uint32_t focused, bool valid, const char *display_name);
void jce_editor_hierarchy_metric(int loaded, int roots, int rows, int capacity);
void jce_editor_hierarchy_reveal_metric(uint32_t entity, bool visible);
#endif
