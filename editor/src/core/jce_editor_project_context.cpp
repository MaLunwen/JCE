#include "jce_editor_project_context.h"
#include <jce/application/jce_project.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_filesystem.h>
#include <cstring>

namespace {
constexpr size_t path_capacity = 1024;
bool absolute_path(const char *input, char (&result)[path_capacity])
{
    if (!input || !input[0]) return false;
    char joined[path_capacity];
    if (jce_path_is_absolute(input))
        return jce_path_normalize(result, sizeof(result), input);
    char cwd[path_capacity];
    return jce_fs_host_get_current_dir(cwd, sizeof(cwd)) &&
           jce_path_join(joined, sizeof(joined), cwd, input) &&
           jce_path_normalize(result, sizeof(result), joined);
}
}

bool jce_editor_project_context_resolve(const char *declared, char *out, size_t cap)
{
    char path[path_capacity];
    const bool normalized = absolute_path(declared, path);
    if (!out || !cap) return false;
    out[0] = '\0';
    if (!normalized) return false;
    if (jce_fs_host_exists_file(path)) {
        char name[path_capacity];
        if (!jce_path_basename(name, sizeof(name), path) ||
            std::strcmp(name, JCE_PROJECT_FILENAME) != 0) return false;
    } else if (!jce_fs_host_exists_dir(path)) {
        return false;
    }
    JceProject *project = jce_project_load(path);
    if (!project) return false;
    bool ok = false;
    if (project->project_root) {
        const size_t length = std::strlen(project->project_root);
        if (length < cap) {
            std::memcpy(out, project->project_root, length + 1);
            ok = true;
        }
    }
    jce_project_free(project);
    return ok;
}

bool jce_editor_project_context_select(const char *content_path, const char *active_root,
                                       bool preserve_active, char *out, size_t cap)
{
    char content[path_capacity];
    const bool normalized = absolute_path(content_path, content);
    char active[path_capacity];
    const bool active_valid = preserve_active &&
        jce_editor_project_context_resolve(active_root, active, sizeof(active));
    if (!out || !cap) return false;
    out[0] = '\0';
    if (active_valid) {
        const size_t length = std::strlen(active);
        if (length >= cap) return false;
        std::memcpy(out, active, length + 1);
        return true;
    }
    char parent[path_capacity], found[path_capacity];
    return normalized && jce_path_parent(parent, sizeof(parent), content) &&
           jce_project_find_root(parent, found, sizeof(found)) &&
           jce_editor_project_context_resolve(found, out, cap);
}
