/* jce_editor_path_util.cpp ───────────────────────────────────────────
 *
 * Path helpers for editor-side serialization (#4).
 * Convert absolute paths to project- or scene-relative paths.
 */

#include "ui/jce_editor_panels.h"

#include <jce/os/core/jce_path.h>

#include <cstdio>
#include <cstring>

extern "C" {

void jce_editor_path_to_relative_to(char *out, size_t out_size,
                                    const char *abs_or_rel_path,
                                    const char *base_dir)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!abs_or_rel_path || abs_or_rel_path[0] == '\0') return;

    /* Already relative — copy through (forward-slash normalize). */
    if (!jce_path_is_absolute(abs_or_rel_path)) {
        char norm[1024];
        if (jce_path_normalize(norm, sizeof(norm), abs_or_rel_path)) {
            snprintf(out, out_size, "%s", norm);
        } else {
            snprintf(out, out_size, "%s", abs_or_rel_path);
        }
        for (char *p = out; *p; ++p) if (*p == '\\') *p = '/';
        return;
    }

    if (!base_dir || base_dir[0] == '\0') {
        snprintf(out, out_size, "%s", abs_or_rel_path);
        for (char *p = out; *p; ++p) if (*p == '\\') *p = '/';
        return;
    }

    char rel[1024];
    if (jce_path_relative(rel, sizeof(rel), abs_or_rel_path, base_dir)) {
        /* Reject results that escape the base (start with "..").  Such a
         * path is more confusing than the absolute, so keep absolute. */
        if (!(rel[0] == '.' && rel[1] == '.' &&
              (rel[2] == '/' || rel[2] == '\\' || rel[2] == '\0'))) {
            snprintf(out, out_size, "%s", rel);
            for (char *p = out; *p; ++p) if (*p == '\\') *p = '/';
            return;
        }
    }

    /* Fall back to absolute. */
    snprintf(out, out_size, "%s", abs_or_rel_path);
    for (char *p = out; *p; ++p) if (*p == '\\') *p = '/';
}

void jce_editor_path_to_relative(char *out, size_t out_size,
                                 const char *abs_or_rel_path)
{
    const char *root = jce_editor_assets_get_project();
    jce_editor_path_to_relative_to(out, out_size, abs_or_rel_path, root);
}

} /* extern "C" */
