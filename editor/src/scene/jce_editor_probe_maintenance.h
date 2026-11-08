/* One render-admitted host pump; state owns no Engine or GPU resources. */
#ifndef JCE_EDITOR_PROBE_MAINTENANCE_H
#define JCE_EDITOR_PROBE_MAINTENANCE_H
#include "core/jce_editor_cpu_scope.h"
#include <jce/middleware/scene/jce_scene_probe_capture.h>
#include <jce/middleware/scene/jce_scene_reflection_probe.h>
#include <jce/renderer/jce_impostor.h>

struct JceEditorProbeMaintenanceState {
    bool busy_this_frame = false;
    uint64_t generation = 0, active_request = 0;
    JceScene *request_scene = nullptr;
    JceEntity request_entity = 0;
};

inline void jce_editor_probe_maintenance_retire_request(JceEditorProbeMaintenanceState &state)
{
    state.active_request = 0;
    state.request_scene = nullptr;
    state.request_entity = 0;
}

/* All Editor capture entry points share this identity authority. A failed
 * begin cannot claim or retire a different active request. Never wrap IDs. */
inline uint64_t jce_editor_probe_maintenance_begin(JceEditorProbeMaintenanceState &state,
    JceScene *scene, JceEntity entity, char *out_path, int capacity)
{
    JceEditorCpuScope scope("editor.probe.capture.begin");
    scope.result(JCE_TRACE_TASK_FAILED);
    if (state.generation == UINT64_MAX ||
        !jce_scene_reflection_probe_capture(scene, entity, out_path, capacity)) return 0;
    state.active_request = ++state.generation;
    state.request_scene = scene;
    state.request_entity = entity;
    scope.result(JCE_TRACE_TASK_SUCCEEDED);
    return state.active_request;
}

inline bool jce_editor_probe_maintenance_matches(const JceEditorProbeMaintenanceState &state,
    uint64_t request, JceScene *scene, JceEntity entity)
{
    return request != 0 && request == state.active_request &&
        scene == state.request_scene && entity == state.request_entity &&
        jce_scene_probe_capture_in_flight();
}

inline bool jce_editor_probe_maintenance_busy(const JceEditorProbeMaintenanceState &state)
{
    return state.busy_this_frame || jce_scene_probe_capture_in_flight();
}

/* Engine cancellation changes intent without submitting frames. The shared
 * readback owner retains pending CPU destinations through completion/backend
 * stop; destroying a GPU handle is not completion of its CPU buffer borrow. */
inline void jce_editor_probe_maintenance_cancel(JceEditorProbeMaintenanceState &state)
{
    JceEditorCpuScope scope("editor.probe.capture.cancel");
    const bool active = jce_scene_probe_capture_in_flight();
    state.busy_this_frame = state.busy_this_frame || active;
    jce_editor_probe_maintenance_retire_request(state);
    if (jce_trace_enabled())
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "editor.probe.cancel.active", active ? 1 : 0);
    jce_scene_probe_capture_cancel();
}

inline void jce_editor_probe_maintenance_tick(JceEditorProbeMaintenanceState &state,
    bool viewport_gpu_allowed, bool initialized, JceSceneRenderer *scene_renderer,
    JceScene *scene, JceRenderer *renderer)
{
    state.busy_this_frame = false;
    if (!viewport_gpu_allowed) return;
    if (!jce_scene_probe_capture_in_flight()) {
        jce_editor_probe_maintenance_retire_request(state); return;
    }
    state.busy_this_frame = true;
    /* Preserve the original modal priority. Never poll both owners' work. */
    if (jce_impostor_bake_in_flight()) return;
    JceEditorCpuScope scope("editor.probe.capture.poll");
    if (!initialized || !scene_renderer || !scene || !renderer) {
        jce_editor_probe_maintenance_cancel(state);
        scope.result(JCE_TRACE_TASK_FAILED);
        return;
    }
    const JceProbeCaptureStatus status = jce_scene_probe_capture_poll(scene_renderer, scene, renderer);
    if (status != JCE_PROBE_CAPTURE_RENDERING) jce_editor_probe_maintenance_retire_request(state);
    if (jce_trace_enabled())
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "editor.probe.capture.status", status);
    if (status == JCE_PROBE_CAPTURE_FAILED) scope.result(JCE_TRACE_TASK_FAILED);
    /* Keep busy even after DONE/FAILED: this poll may have used the same view
     * band. Scene/Game resume only after the next host pump resets it. */
}
#endif
