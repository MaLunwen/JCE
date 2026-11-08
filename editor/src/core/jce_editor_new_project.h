#ifndef JCE_EDITOR_NEW_PROJECT_H
#define JCE_EDITOR_NEW_PROJECT_H
#include <jce/application/jce_project.h>
#include <cstddef>

/* UI-free continuation after the existing Save/Discard/Cancel gate. These
   ports are the editor's existing activation/scene/save/manifest owners. */
struct JceEditorNewProjectPorts {
    bool (*activate)(const char *project_root);
    bool (*new_scene)(bool is_2d);
    bool (*save_scene)(const char *host_scene_path);
    bool (*update_field)(const char *field, const char *value);
};
bool jce_editor_new_project_create(const char *project_root, const char *name,
    JceProjectTemplate type, const JceEditorNewProjectPorts &ports,
    char *error, size_t error_cap);
#endif
