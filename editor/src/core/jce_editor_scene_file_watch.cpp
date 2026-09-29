/*
 * jce_editor_scene_file_watch.cpp — see jce_editor_scene_file_watch.h.
 *
 * The polling shape is the one editor/src/scene/jce_asset_cache_material.cpp
 * already uses for materials edited outside the editor: an interval, a stamp
 * taken at arm time so the first poll after a load reports nothing, and a stat
 * rather than a platform file-watch API.  One file is watched here, not a
 * table, so the budget is one stat every JCE_SCENE_WATCH_INTERVAL_FRAMES.
 */
#include "jce_editor_scene_file_watch.h"

#include <stdio.h>
#include <string.h>

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>

#include "jce_editor_state.h"
#include "jce_editor_state_internal.h"
#include "jce_editor_toast.h"
#include "jce_editor_i18n.h"

#define LOG_TAG "scene_watch"

namespace {

enum { JCE_SCENE_WATCH_INTERVAL_FRAMES = 30 };

/* THE STATE LIVES IN EditorInternalState, not here.  A file-scope object in
 * this translation unit would be the obvious shape and it is the wrong one
 * twice over: this folder's charter says all editor state goes through the
 * central struct, and the dedup audit counts the difference -- it read
 * global-state 859 -> 860 on the version that had one, and that baseline is
 * not for raising.  Measured after committing, which is the only time that
 * detector can see a new file at all: it enumerates sources with
 * `git ls-files`. */

/* A scene that is not a file on disk cannot be watched: bundle previews are
 * read out of a mounted .jbundle and an unsaved new scene has no path at all.
 * Reported as "nothing to watch" rather than as a failed stat, because a stat
 * that fails every frame reads like a broken watcher. */
bool watchable(const char *path)
{
    return path && path[0] && strncmp(path, "bundle://", 9) != 0;
}

void stamp(const char *path)
{
    snprintf(s.scene_watch_path, sizeof(s.scene_watch_path), "%s", path ? path : "");
    s.scene_watch_mtime = 0;
    s.scene_watch_armed = jce_fs_host_get_mtime(s.scene_watch_path, &s.scene_watch_mtime);
    s.scene_watch_stale = false;
}

} /* namespace */

extern "C" void jce_editor_scene_file_watch_restamp(void)
{
    const char *path = jce_state_get_current_scene_path();
    if (!watchable(path)) {
        s.scene_watch_armed = false;
        s.scene_watch_stale = false;
        s.scene_watch_path[0] = '\0';
        return;
    }
    stamp(path);
}

extern "C" bool jce_editor_scene_file_watch_is_stale(void)
{
    return s.scene_watch_stale;
}

extern "C" void jce_editor_scene_file_watch_reload_now(void)
{
    if (!s.scene_watch_armed || !s.scene_watch_path[0])
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s", s.scene_watch_path);
    s.scene_watch_stale = false;
    if (jce_state_load_scene_file_async(path)) {
        /* The load path re-stamps on completion; clearing the flag here only
         * stops the warning from being shown for the frames in between. */
        jce_toast_info("%s", jce_editor_i18n_or("toast.sceneReloaded",
                                                "Scene reloaded from disk"));
    } else {
        jce_toast_error("%s", jce_editor_i18n_or("toast.sceneReloadFailed",
                                                 "Could not reload the scene "
                                                 "from disk"));
    }
}

extern "C" void jce_editor_scene_file_watch_poll(void)
{
    /* Never operate on a half-built scene: the async open creates entities in
     * chunks and a reload issued mid-flight would race the chunk it is on. */
    if (jce_state_is_scene_loading())
        return;

    const char *path = jce_state_get_current_scene_path();
    if (!watchable(path)) {
        s.scene_watch_armed = false;
        s.scene_watch_stale = false;
        return;
    }
    /* Opened a different scene without anybody telling us. */
    if (!s.scene_watch_armed || strcmp(s.scene_watch_path, path) != 0) {
        stamp(path);
        return;
    }
    if (--s.scene_watch_countdown > 0)
        return;
    s.scene_watch_countdown = JCE_SCENE_WATCH_INTERVAL_FRAMES;

    int64_t now = 0;
    if (!jce_fs_host_get_mtime(s.scene_watch_path, &now))
        return;                     /* deleted or briefly unreadable mid-write */
    if (now == s.scene_watch_mtime)
        return;

    /* CONSUME THE EVENT FIRST.  Whatever is decided below, this modification
     * has been seen; leaving the old stamp in place would re-report it every
     * interval for as long as the file sits there. */
    s.scene_watch_mtime = now;

    if (jce_state_is_scene_modified()) {
        /* Both sides have edits.  Stand still and say so -- see the header. */
        s.scene_watch_stale = true;
        LOG_WARN(LOG_TAG,
                 "scene file changed on disk while the editor has unsaved "
                 "edits; reload withheld: %s", s.scene_watch_path);
        jce_toast_warn("%s", jce_editor_i18n_or(
            "toast.sceneChangedOnDiskUnsaved",
            "The scene file changed on disk, but this editor has unsaved "
            "edits. Nothing was reloaded."));
        return;
    }

    LOG_INFO(LOG_TAG, "scene file changed on disk; reloading: %s",
             s.scene_watch_path);
    jce_editor_scene_file_watch_reload_now();
}
