/*
 * jce_editor_project_render_pipeline.cpp
 * Isolated project render-pipeline loading for editor project switches.
 */

#include "jce_editor_project_render_pipeline.h"

#include <jce/os/core/jce_path.h>

#include <cstring>

bool jce_editor_project_render_pipeline_load(
    const char *project_root, JceRenderPipelineDesc *out)
{
    if (!out)
        return false;

    std::memset(out, 0, sizeof(*out));
    if (!project_root || !project_root[0])
        return false;

    char settings_path[1024];
    char pipeline_path[1024];
    if (!jce_path_join(settings_path, sizeof(settings_path),
                       project_root, "Settings") ||
        !jce_path_join(pipeline_path, sizeof(pipeline_path),
                       settings_path, "RenderPipeline.rp.json"))
        return false;

    JceRenderPipelineDesc loaded{};
    if (!jce_render_pipeline_load(pipeline_path, &loaded)) {
        std::memset(out, 0, sizeof(*out));
        return false;
    }

    *out = loaded;
    return true;
}
