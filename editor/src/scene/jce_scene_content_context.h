#ifndef JCE_SCENE_CONTENT_CONTEXT_H
#define JCE_SCENE_CONTENT_CONTEXT_H

#include <string>

struct JceEditorSceneContentPaths {
    bool isolated = false;
    std::string render_settings_primary;
    std::string render_settings_fallback;
    std::string particle_asset_root;
};

JceEditorSceneContentPaths jce_editor_scene_content_paths(
    bool isolated, const char *project_root, const char *source_assets,
    const char *cooked_assets);

#endif
