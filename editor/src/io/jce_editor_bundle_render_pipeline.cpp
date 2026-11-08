#include "jce_editor_bundle_render_pipeline.h"
#include "core/jce_editor_cpu_scope.h"
#include <jce/os/core/jce_log.h>

static constexpr const char *kPipelineKey = "settings/render_pipeline.rp.json";

bool jce_editor_bundle_pipeline_prepare(JceEditorBundlePipeline *state,
                                        const JceFileSystem *fs)
{
    JceEditorCpuScope scope("editor.bundle_pipeline.prepare");
    if (!state || state->active) {
        scope.result(JCE_TRACE_TASK_FAILED);
        return false;
    }
    *state = JceEditorBundlePipeline{};
    const JceFsPakReadResult presence = jce_fs_read_pak_bounded(
        fs, kPipelineKey, 1u << 20, nullptr, nullptr);
    if (presence == JCE_FS_PAK_READ_ABSENT) {
        jce_render_pipeline_preset_for_current_tier(&state->prepared);
        state->used_tier = true;
    } else if (presence != JCE_FS_PAK_READ_OK ||
               !jce_render_pipeline_load_fs(fs, kPipelineKey, &state->prepared)) {
        scope.result(JCE_TRACE_TASK_FAILED);
        LOG_ERROR("bundle-pipeline", "Bundle render pipeline refused (metadata_status=%d): %s",
                  (int)presence, kPipelineKey);
        return false;
    }
    state->ready = true;
    if (jce_trace_enabled())
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "bundle_rp.tier_fallback", state->used_tier ? 1 : 0);
    return true;
}
bool jce_editor_bundle_pipeline_apply(JceEditorBundlePipeline *state)
{
    JceEditorCpuScope scope("editor.bundle_pipeline.apply");
    if (!state || !state->ready || state->active) {
        scope.result(JCE_TRACE_TASK_FAILED);
        return false;
    }
    jce_render_pipeline_end_frame();
    jce_render_pipeline_get_requested(&state->previous);
    state->active = true;
    jce_render_pipeline_apply(&state->prepared);
    if (jce_trace_enabled())
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "bundle_rp.active", state->active ? 1 : 0);
    LOG_INFO("bundle-pipeline", "Bundle render pipeline applied: %s",
             state->used_tier ? "current hardware tier (asset absent)" : kPipelineKey);
    return true;
}
void jce_editor_bundle_pipeline_close(JceEditorBundlePipeline *state)
{
    if (!state) return;
    const bool active = state->active;
    const JceRenderPipelineDesc previous = state->previous;
    *state = JceEditorBundlePipeline{};
    if (active) {
        JceEditorCpuScope scope("editor.bundle_pipeline.restore");
        jce_render_pipeline_apply(&previous);
        if (jce_trace_enabled())
            jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "bundle_rp.active", state->active ? 1 : 0);
        LOG_INFO("bundle-pipeline", "Project render pipeline restored after Bundle close");
    }
}
