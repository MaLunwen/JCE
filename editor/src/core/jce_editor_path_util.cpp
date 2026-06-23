/* jce_editor_path_util.cpp ───────────────────────────────────────────
 *
 * Path helpers for editor-side serialization (#4).
 * Convert absolute paths to project- or scene-relative paths.
 */

#include "ui/jce_editor_panels.h"

#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_filesystem.h>

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

const char *jce_editor_path_relative_or(char *buf, size_t buf_size,
                                        const char *src)
{
    if (!buf || buf_size == 0) return src;
    buf[0] = '\0';
    if (!src) return "";
    jce_editor_path_to_relative(buf, buf_size, src);
    return buf[0] ? buf : src;
}

void jce_editor_path_store_asset_ref(char *out, size_t out_size,
                                     const char *path)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;

    /* CWD-relative inputs (e.g. the engine model importer reports its
     * extracted embedded-texture files relative to the process CWD) are
     * meaningless once stored in a scene — the editor may be launched
     * from anywhere.  Anchor them to an absolute path first so the
     * project-relative conversion below has a real base.  Inputs that
     * are already project-relative do NOT resolve against the CWD and
     * pass through untouched. */
    char anchored_buf[1024];
    const char *anchored = path;
    if (!jce_path_is_absolute(path)) {
        char cwd[512];
        if (jce_fs_host_get_current_dir(cwd, sizeof(cwd))) {
            char joined[1024];
            snprintf(joined, sizeof(joined), "%s/%s", cwd, path);
            char norm[1024];
            const char *cand = joined;
            if (jce_path_normalize(norm, sizeof(norm), joined))
                cand = norm;
            if (jce_fs_host_exists_file(cand)) {
                snprintf(anchored_buf, sizeof(anchored_buf), "%s", cand);
                anchored = anchored_buf;
            }
        }
    }

    jce_editor_path_to_relative(out, out_size, anchored);
    if (!out[0])
        snprintf(out, out_size, "%s", anchored);
    for (char *p = out; *p; ++p) if (*p == '\\') *p = '/';
}

const char *jce_editor_path_basename_view(const char *path)
{
    if (!path || !path[0]) return "";
    const char *slash  = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *sep    = (slash > bslash) ? slash : bslash;
    return sep ? sep + 1 : path;
}

void jce_editor_path_trim_to_parent(char *path)
{
    if (!path || !path[0]) return;
    char *slash  = strrchr(path, '/');
    char *bslash = strrchr(path, '\\');
    char *sep    = (slash > bslash) ? slash : bslash;
    if (sep) *sep = '\0';
    else     path[0] = '\0';
}

void jce_editor_path_strip_extension(char *path)
{
    if (!path || !path[0]) return;
    char *dot    = strrchr(path, '.');
    if (!dot) return;
    char *slash  = strrchr(path, '/');
    char *bslash = strrchr(path, '\\');
    char *sep    = (slash > bslash) ? slash : bslash;
    /* Only strip when the dot belongs to the basename (not "../foo"). */
    if (sep && dot < sep) return;
    *dot = '\0';
}

} /* extern "C" */
