/*
 * jce_editor_project_render_pipeline.h
 * Project-owned render-pipeline asset loading.
 */

#ifndef JCE_EDITOR_PROJECT_RENDER_PIPELINE_H
#define JCE_EDITOR_PROJECT_RENDER_PIPELINE_H

#include <jce/renderer/jce_render_pipeline.h>

/* Load <project_root>/Settings/RenderPipeline.rp.json.
 * On every failure, out is reset to all-zeroes so callers cannot retain a
 * descriptor from the previously opened project. */
bool jce_editor_project_render_pipeline_load(
    const char *project_root, JceRenderPipelineDesc *out);

#endif /* JCE_EDITOR_PROJECT_RENDER_PIPELINE_H */
