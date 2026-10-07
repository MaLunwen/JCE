/* Public console-only warm lighting transitions; no scene or material edits. */
#include "light_transition.h"

#include <jce/api_app.h>
#include <jce/api_core.h>
#include <stdlib.h>
#include <string.h>

bool bench_light_transition(unsigned frame)
{
    const char *mode = getenv("JCE_BENCH_FP_TRANSITION");
    if (!mode || !mode[0]) return true;
    bool starts_on = !strcmp(mode, "on-off");
    bool returns_off = !strcmp(mode, "off-on-off");
    if (!starts_on && !returns_off && strcmp(mode, "off-on")) {
        LOG_ERROR("benchmark_light", "benchmark-fp-transition: invalid mode=%s", mode);
        jce_engine_request_quit();
        return false;
    }
    JceCvar *gate = jce_cvar_find("r.forwardplus");
    if (!gate || jce_cvar_type(gate) != JCE_CVAR_BOOL) {
        LOG_ERROR("benchmark_light", "benchmark-fp-transition: missing bool authority");
        jce_engine_request_quit();
        return false;
    }
    bool expected = frame < 30 ? starts_on : !starts_on;
    if (returns_off && frame >= 70) expected = false;
    bool changed = frame == 1 || frame == 30 || (returns_off && frame == 70);
    if (changed) jce_cvar_set_bool(gate, expected);
    bool actual = jce_cvar_get_bool(gate);
    LOG_INFO("benchmark_light",
        "benchmark-fp-transition: frame=%u mode=%s expected=%u actual=%u changed=%u",
        frame, mode, (unsigned)expected, (unsigned)actual, (unsigned)changed);
    if (actual != expected) {
        LOG_ERROR("benchmark_light", "benchmark-fp-transition: authority readback mismatch");
        jce_engine_request_quit();
        return false;
    }
    return true;
}
