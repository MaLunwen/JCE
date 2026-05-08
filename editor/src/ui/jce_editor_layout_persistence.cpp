/*
 * jce_editor_layout_persistence.cpp  Named-layout save/load on disk.
 *
 * Each layout is a full imgui.ini snapshot — ImGui's persistent state
 * already encodes window pos/size, dock node tree, and our panel
 * visibility flags.  We just round-trip those files through
 * <project>/.jce/layouts/<name>.ini.
 */

#include "jce_editor_layout_persistence.h"
#include "ui/jce_editor_panels.h"

#include <jce/tools/jce_imgui.hpp>

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_path.h>
}

#include <cstdio>
#include <cstring>
#include <vector>

#define LOG_TAG "layout"

namespace {

/* Build the on-disk path for a layout file.  Returns false if the
 * caller hasn't opened a project yet (no place to store layouts). */
bool layout_path(const char *name, char *out, size_t out_size)
{
    const char *root = jce_editor_assets_get_project();
    if (!root || !*root || !name || !*name) return false;

    char dir[1024];
    if (!jce_path_join(dir, sizeof(dir), root, ".jce/layouts")) return false;
    if (!jce_fs_host_exists_dir(dir)) {
        if (!jce_fs_host_create_directory(dir)) {
            LOG_ERROR(LOG_TAG, "failed to create %s", dir);
            return false;
        }
    }

    /* Sanitise: forbid path separators in `name`.  Caller is the
     * editor menu which validates input, but defence-in-depth. */
    for (const char *c = name; *c; ++c) {
        if (*c == '/' || *c == '\\' || *c == '.') return false;
    }

    int n = snprintf(out, out_size, "%s/%s.ini", dir, name);
    return n > 0 && (size_t)n < out_size;
}

struct ListCtx {
    char (*out_names)[64];
    uint32_t max_count;
    uint32_t count;
};

bool list_walk_cb(const char *path, bool is_dir, void *ud)
{
    auto *c = static_cast<ListCtx *>(ud);
    if (is_dir) return true;
    /* Strip path → basename → drop .ini suffix. */
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bs = strrchr(path, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
#endif
    const char *fname = slash ? slash + 1 : path;
    size_t len = strlen(fname);
    const char *ext = ".ini";
    size_t elen = 4;
    if (len <= elen) return true;
    if (memcmp(fname + len - elen, ext, elen) != 0) return true;
    if (c->count >= c->max_count) return false;
    size_t name_len = len - elen;
    if (name_len >= 64) name_len = 63;
    memcpy(c->out_names[c->count], fname, name_len);
    c->out_names[c->count][name_len] = '\0';
    c->count++;
    return true;
}

} /* namespace */

extern "C" bool jce_editor_layout_save(const char *name)
{
    char path[1280];
    if (!layout_path(name, path, sizeof(path))) return false;
    /* ImGui::SaveIniSettingsToDisk writes the current state (window
     * positions + dock layout + our panel visibility flags). */
    ImGui::SaveIniSettingsToDisk(path);
    return jce_fs_host_exists_file(path);
}

extern "C" bool jce_editor_layout_load(const char *name)
{
    char path[1280];
    if (!layout_path(name, path, sizeof(path))) return false;
    if (!jce_fs_host_exists_file(path)) return false;
    /* Replace the entire ImGui layout state with the named snapshot. */
    ImGui::LoadIniSettingsFromDisk(path);
    /* Mark settings dirty so ImGui re-applies the docking node tree on
     * the next NewFrame. */
    ImGui::GetIO().WantSaveIniSettings = false;
    return true;
}

extern "C" bool jce_editor_layout_delete(const char *name)
{
    char path[1280];
    if (!layout_path(name, path, sizeof(path))) return true;
    if (!jce_fs_host_exists_file(path)) return true;
    return jce_fs_host_remove_file(path);
}

extern "C" uint32_t jce_editor_layout_list(char (*out_names)[64],
                                            uint32_t max_count)
{
    if (!out_names || max_count == 0) return 0;
    const char *root = jce_editor_assets_get_project();
    if (!root || !*root) return 0;
    char dir[1024];
    if (!jce_path_join(dir, sizeof(dir), root, ".jce/layouts")) return 0;
    if (!jce_fs_host_exists_dir(dir)) return 0;

    ListCtx ctx{ out_names, max_count, 0 };
    jce_fs_host_walk(dir, list_walk_cb, &ctx);
    return ctx.count;
}
