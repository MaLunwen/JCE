/* Private on_update cost partition. Main-thread, process-latched selector. */
#ifndef JCE_RT_SCRIPT_COST_H
#define JCE_RT_SCRIPT_COST_H

#include <stdbool.h>

typedef struct RtScriptCostFrame {
    const char *focus_path;
    double focus_ms;
    double other_ms;
} RtScriptCostFrame;

/* Profiling is read by the caller once. No environment query while off. */
RtScriptCostFrame rt_script_cost_begin(bool profiling);
/* Borrow the duration already measured around this executed on_update. */
void rt_script_cost_add(RtScriptCostFrame *frame, const char *path, double ms);
/* Literal slots accumulate across runtime calls until the Engine frame tick.
 * Zero means an empty/zero subtotal, not proof that any callback ran. */
void rt_script_cost_end(const RtScriptCostFrame *frame);

#endif
