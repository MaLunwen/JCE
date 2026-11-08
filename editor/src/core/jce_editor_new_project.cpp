#include "jce_editor_new_project.h"
#include "jce_editor_project.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <cstdio>
#include <cstring>

namespace {
bool fail(char *error, size_t cap, const char *stage, const char *root)
{
    if (error && cap) std::snprintf(error, cap,
        "New project failed at %s. Any partial project remains at: %s", stage, root ? root : "");
    return false;
}
bool any_entry(const char *, bool, void *user)
{ *static_cast<bool *>(user) = true; return false; }
}

bool jce_editor_new_project_create(const char *root, const char *name,
    JceProjectTemplate type, const JceEditorNewProjectPorts &ports,
    char *error, size_t error_cap)
{
    if (error && error_cap) error[0] = '\0';
    if (!root || !root[0] || !name || !name[0] || !ports.activate ||
        !ports.new_scene || !ports.save_scene || !ports.update_field ||
        (type != JCE_PROJECT_TEMPLATE_BASIC_3D && type != JCE_PROJECT_TEMPLATE_BASIC_2D))
        return fail(error, error_cap, "request validation", root);
    bool occupied = false;
    if (jce_fs_host_exists_file(root))
        return fail(error, error_cap, "target is a file", root);
    if (jce_fs_host_exists_dir(root)) {
        if (!jce_fs_host_list_dir(root, any_entry, &occupied) || occupied)
            return fail(error, error_cap, "nonempty or unreadable target", root);
    }
    if (!jce_project_create_from_template(root, name, type, error, error_cap))
        return false; /* The actual template owner supplies its precise cause. */

    char settings[1024], pipeline_path[1024];
    if (!jce_path_join(settings, sizeof(settings), root, "Settings") ||
        !jce_path_join(pipeline_path, sizeof(pipeline_path), settings, "RenderPipeline.rp.json") ||
        !jce_fs_host_create_directory(settings))
        return fail(error, error_cap, "render-pipeline directory", root);
    JceRenderPipelineDesc pipeline;
    jce_render_pipeline_preset_for_current_tier(&pipeline);
    if (!jce_render_pipeline_save(pipeline_path, &pipeline))
        return fail(error, error_cap, "render-pipeline publication", root);

    JceProject *project = jce_project_load(root);
    if (!project) return fail(error, error_cap, "generated manifest admission", root);
    char asset_dir[1024], scene_dir[1024], scene_path[1024];
    const char *source = project->source_assets && project->source_assets[0]
        ? project->source_assets : "assets";
    const bool paths_ok =
        jce_path_join(asset_dir, sizeof(asset_dir), root, source) &&
        jce_path_join(scene_dir, sizeof(scene_dir), asset_dir, "scenes") &&
        jce_path_join(scene_path, sizeof(scene_path), scene_dir, "Main.scene");
    jce_project_free(project);
    if (!paths_ok || !jce_fs_host_create_directory(scene_dir))
        return fail(error, error_cap, "startup-scene directory", root);

    /* After activation, any failure is explicit partial state. Never announce
       success/recents or silently delete a partially-created user directory. */
    if (!ports.activate(root)) return fail(error, error_cap, "project activation", root);
    if (!ports.new_scene(type == JCE_PROJECT_TEMPLATE_BASIC_2D))
        return fail(error, error_cap, "default-scene creation", root);
    if (!ports.save_scene(scene_path)) return fail(error, error_cap, "startup-scene save", root);
    /* startup_scene is relative to source_assets, not the project directory. */
    if (!ports.update_field("startup_scene", "scenes/Main.scene"))
        return fail(error, error_cap, "startup-scene manifest publication", root);
    /* A successful writer can still lose its subsequent cached reload. Read
       the public manifest back and confirm the actual active project too. */
    JceProject *published = jce_project_load(root);
    const JceProject *current = jce_editor_project_get();
    char requested_root[1024], active_root[1024];
    const bool complete = published && current && current->project_root &&
        jce_path_normalize(requested_root, sizeof(requested_root), root) &&
        jce_path_normalize(active_root, sizeof(active_root), current->project_root) &&
        std::strcmp(requested_root, active_root) == 0 &&
        published->startup_scene && current->startup_scene &&
        std::strcmp(published->startup_scene, "scenes/Main.scene") == 0 &&
        std::strcmp(current->startup_scene, "scenes/Main.scene") == 0;
    jce_project_free(published);
    if (!complete) return fail(error, error_cap, "startup-scene readback", root);
    return true;
}
