/* SDK default-entry CPU observations; no renderer or presentation authority. */
#ifndef JCE_DEFAULT_FRAME_OBSERVATION_INC_H
#define JCE_DEFAULT_FRAME_OBSERVATION_INC_H
#include <jce/application/jce_engine.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <string.h>

/* Source 1 is assigned by this host. Reasons are private to this publisher. */
enum {
    JCE_DEFAULT_OBSERVATION_HOST_POLICY = 1,
    JCE_DEFAULT_OBSERVATION_MISSING_OWNERS = 2,
    JCE_DEFAULT_OBSERVATION_CPU_INCOMPLETE = 3
};
static inline bool default_frame_observation_begin(JceFrameViewportObservation *value)
{
    if (!jce_engine_frame_observation_enabled()) return false;
    memset(value, 0, sizeof(*value));
    value->struct_size = sizeof(*value);
    value->source_id = 1;
    value->cpu_outcome = JCE_FRAME_VIEWPORT_CPU_FAILED;
    value->refusal_reason = JCE_DEFAULT_OBSERVATION_CPU_INCOMPLETE;
    return true;
}
static inline void default_frame_observation_publish(
    const JceFrameViewportObservation *value, bool enabled)
{
    if (enabled) (void)jce_engine_report_frame_observations(value, 1, sizeof(*value));
}
static inline void default_frame_observation_refuse(
    JceFrameViewportObservation *value, bool enabled, uint32_t reason)
{
    if (!enabled) return;
    value->cpu_outcome = JCE_FRAME_VIEWPORT_CPU_SKIPPED;
    value->refusal_reason = reason;
    default_frame_observation_publish(value, true);
}
static inline void default_frame_observation_request(
    JceFrameViewportObservation *value, bool enabled, uint32_t width, uint32_t height)
{
    if (!enabled) return;
    /* A direct fallback replaces this interval's bridge attempt, never its
     * retained allocation. Renderer trace retains the preparation outcome. */
    value->flags = 0;
    value->requested_width = value->requested_height = 0;
    value->allocated_width = value->allocated_height = 0;
    value->cpu_outcome = JCE_FRAME_VIEWPORT_CPU_FAILED;
    value->refusal_reason = JCE_DEFAULT_OBSERVATION_CPU_INCOMPLETE;
    if (width && height) {
        value->flags = JCE_FRAME_VIEWPORT_REQUESTED_SIZE_VALID;
        value->requested_width = width;
        value->requested_height = height;
    }
}
static inline void default_frame_observation_prepared(
    JceFrameViewportObservation *value, bool enabled, const JceOffscreenTarget *target)
{
    uint32_t width, height;
    if (!enabled) return;
    value->flags |= JCE_FRAME_VIEWPORT_PREPARE_SUCCEEDED;
    if (jce_offscreen_target_get_extent(target, &width, &height)) {
        value->flags |= JCE_FRAME_VIEWPORT_ALLOCATED_SIZE_VALID;
        value->allocated_width = width;
        value->allocated_height = height;
    }
}
static inline void default_frame_observation_cpu_returned(
    JceFrameViewportObservation *value, bool enabled)
{
    if (!enabled) return;
    /* render() can return its base view after internal refusal. This records
     * that CPU call returning, not successful rendering or GPU submission. */
    value->cpu_outcome = JCE_FRAME_VIEWPORT_CPU_COMPLETED;
    value->refusal_reason = 0;
}
#endif
