/* One temporary Bundle override over the Editor's project-owned pipeline. */
#ifndef JCE_EDITOR_BUNDLE_RENDER_PIPELINE_H
#define JCE_EDITOR_BUNDLE_RENDER_PIPELINE_H
#include <jce/os/core/jce_filesystem.h>
#include <jce/renderer/jce_render_pipeline.h>

struct JceEditorBundlePipeline {
    JceRenderPipelineDesc prepared{};
    JceRenderPipelineDesc previous{};
    bool ready = false;
    bool active = false;
    bool used_tier = false;
};
/* Trace scopes editor.bundle_pipeline.prepare/apply/restore cover synchronous
 * owner calls, never cross frames and do not certify GPU completion. Refused
 * prepare/apply report FAILED; confirmed absence is a successful tier fallback.
 * Actual publication nests under apply/restore. Inactive close has no restore.
 * Read/validate without mutating global rendering. Present invalid refuses;
 * only confirmed absence uses the current tier preset. */
bool jce_editor_bundle_pipeline_prepare(JceEditorBundlePipeline *state,
                                        const JceFileSystem *fs);
/* Commit deferred project edits, preserve their requested intent, then apply.
 * Must happen before scene deserialization creates rendering consumers. */
bool jce_editor_bundle_pipeline_apply(JceEditorBundlePipeline *state);
/* Restore once before returning to source-project rendering. */
void jce_editor_bundle_pipeline_close(JceEditorBundlePipeline *state);
#endif
