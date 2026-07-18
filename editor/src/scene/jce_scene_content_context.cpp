#include "jce_scene_content_context.h"

namespace {

std::string join_path(const char *root, const char *child,
                      const char *leaf = nullptr)
{
    std::string path = root ? root : "";
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
        path += '/';
    path += child ? child : "";
    if (leaf && leaf[0]) {
        if (!path.empty() && path.back() != '/' && path.back() != '\\')
            path += '/';
        path += leaf;
    }
    return path;
}

} // namespace

JceEditorSceneContentPaths jce_editor_scene_content_paths(
    bool isolated, const char *project_root, const char *source_assets,
    const char *cooked_assets)
{
    JceEditorSceneContentPaths paths;
    paths.isolated = isolated;
    if (isolated) {
        paths.render_settings_primary = "render_settings.json";
        return paths;
    }

    if (!project_root || !project_root[0])
        return paths;

    const char *source = source_assets && source_assets[0]
                             ? source_assets : "resources/assets";
    const char *cooked = cooked_assets && cooked_assets[0]
                             ? cooked_assets : "resources/_cooked";
    paths.render_settings_primary =
        join_path(project_root, source, "render_settings.json");
    paths.render_settings_fallback =
        join_path(project_root, cooked, "render_settings.json");
    paths.particle_asset_root = join_path(project_root, source);
    return paths;
}
