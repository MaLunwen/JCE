/* API-version admission over the actual L2 context snapshot and existing L3
 * version authority. One fixed identity belongs to that authority, not a registry. */
#ifndef JCE_RENDERER_WEB_API_INTERNAL_H
#define JCE_RENDERER_WEB_API_INTERNAL_H
#include "jce_renderer_caps_internal.h"
#include "os/platform/jce_webgl_context_internal.h"
#include "os/core/jce_trace_scope_internal.h"
#include <jce/os/core/jce_log.h>
#include <string.h>

typedef struct JceRendererWebApiState {
    uint32_t context_id, epoch;
    bool admitted;
} JceRendererWebApiState;
typedef enum JceRendererWebApiReason {
    JCE_WEB_API_OK = 0, JCE_WEB_API_MISSING, JCE_WEB_API_MIXED_EPOCH,
    JCE_WEB_API_UNKNOWN, JCE_WEB_API_BELOW_FLOOR, JCE_WEB_API_SHADING,
    JCE_WEB_API_INVALIDATED
} JceRendererWebApiReason;
static inline void jce_renderer_web_api_record(JceTraceScope scope,
        const JceWebGlContextSnapshot *observation, JceRendererWebApiReason reason)
{
    if (scope.id) {
#define WEB_API_COUNTER(n, v) jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, \
        "renderer.web_api." n, (int64_t)(v), scope.id, scope.parent)
        WEB_API_COUNTER("reason", reason);
        WEB_API_COUNTER("context", observation->context_id);
        WEB_API_COUNTER("epoch.begin", observation->epoch_begin);
        WEB_API_COUNTER("epoch.end", observation->epoch_end);
        WEB_API_COUNTER("webgl.major", observation->webgl_major);
#undef WEB_API_COUNTER
    }
    if (reason != JCE_WEB_API_OK)
        LOG_WARN("renderer_caps", "Web API admission refused reason=%u context=%u epoch=%u/%u "
            "webgl=%u; observed API=%s GLSL=%s", (unsigned)reason,
            (unsigned)observation->context_id, (unsigned)observation->epoch_begin,
            (unsigned)observation->epoch_end, (unsigned)observation->webgl_major,
            observation->api, observation->shading);
    jce_trace_scope_end(scope, reason == JCE_WEB_API_OK ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
}
static inline bool jce_renderer_web_api_admit(JceRendererWebApiState *state,
                                              JceWebGlContextRead read)
{
    JceWebGlContextSnapshot observation = {0};
    JceTraceScope scope = jce_trace_scope_begin("renderer.web_api.admit");
    JceRendererWebApiReason reason = JCE_WEB_API_MISSING;
    state->admitted = false;
    jce_renderer_caps_api_reset();
    jce_renderer_caps_api_begin_attempt(JCE_BACKEND_OPENGLES);
    const bool available = read && read(&observation);
    /* Force bounded provider strings even after a failed/partial read. */
    observation.api[191] = observation.shading[191] = observation.renderer[191] = '\0';
    if (!available || !observation.context_id) goto rejected;
    reason = JCE_WEB_API_MIXED_EPOCH;
    if (!observation.epoch_begin || observation.epoch_begin != observation.epoch_end) goto rejected;
    reason = JCE_WEB_API_UNKNOWN;
    /* WebGL's leading 2.0 is a distinct version namespace. Parse the actual
     * embedded GLES reading; never substitute the requested context version. */
    const char *api = strstr(observation.api, "OpenGL ES ");
    const char *shading = strstr(observation.shading, "GLSL ES ");
    if (!api || api[10] < '0' || api[10] > '9' || !shading ||
        shading[8] < '0' || shading[8] > '9' ||
        (observation.webgl_major != 1 && observation.webgl_major != 2)) goto rejected;
    const bool floor = jce_renderer_caps_api_publish_probe(JCE_BACKEND_OPENGLES, api, shading);
    const JceRendererApiInfo info = jce_renderer_get_api_info();
    if (!info.runtime_version_verified) goto rejected;
    reason = JCE_WEB_API_BELOW_FLOOR;
    if (!floor || observation.webgl_major < 2) goto rejected;
    reason = JCE_WEB_API_SHADING;
    if (!info.shader_language_version_verified ||
        info.shader_language_version.major < info.minimum_version.major ||
        (info.shader_language_version.major == info.minimum_version.major &&
         info.shader_language_version.minor < info.minimum_version.minor)) goto rejected;
    state->context_id = observation.context_id;
    state->epoch = observation.epoch_end;
    state->admitted = true;
    jce_renderer_web_api_record(scope, &observation, JCE_WEB_API_OK);
    return true;
rejected:
    jce_renderer_caps_api_reset(); /* rejected or mixed observations cannot carry flags */
    jce_renderer_caps_api_begin_attempt(JCE_BACKEND_OPENGLES);
    jce_renderer_web_api_record(scope, &observation, reason);
    return false;
}
static inline bool jce_renderer_web_api_current(JceRendererWebApiState *state,
                                                JceWebGlContextRead read)
{
    if (!state->admitted) return false;
    JceWebGlContextSnapshot observation = {0};
    if (read && read(&observation) && observation.context_id == state->context_id &&
        observation.epoch_begin == state->epoch && observation.epoch_end == state->epoch)
        return true;
    /* Restoration/replacement must re-enter the actual renderer creation owner.
     * No automatic GPU resource reconstruction is certified by a new reading. */
    state->admitted = false;
    jce_renderer_caps_api_reset();
    jce_renderer_caps_api_begin_attempt(JCE_BACKEND_OPENGLES);
    JceTraceScope scope = jce_trace_scope_begin("renderer.web_api.invalidated");
    observation.api[191] = observation.shading[191] = '\0';
    jce_renderer_web_api_record(scope, &observation, JCE_WEB_API_INVALIDATED);
    return false;
}
#endif
