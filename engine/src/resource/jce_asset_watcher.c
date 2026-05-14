/*
 * jce_asset_watcher.c  Directory-tree hot-reload watcher.
 *
 * Layering:
 *   - jce_file_watcher (OS layer) tracks individual file mtimes.
 *   - This module walks a directory tree on startup + periodically,
 *     calling jce_file_watcher_add for new files and emitting ADDED
 *     events as it goes.  When a previously-tracked file is missing
 *     from a rescan, REMOVED fires and the file is unregistered.
 *
 * The set of tracked paths lives in a flat string table — linear-
 * search but bounded to a few hundred paths in typical editor use.
 */

#include <jce/resource/jce_asset_watcher.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/platform/jce_file_watcher.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "asset-watcher"
#define ASSET_WATCH_MAX_PATHS 1024
#define ASSET_PATH_LEN 512

typedef struct {
    char  path[ASSET_PATH_LEN];
    bool  seen_this_scan;
} Entry;

struct JceAssetWatcher {
    JceFileWatcher  *fw;
    JceAssetWatchFn  cb;
    void            *user_data;
    char             root[ASSET_PATH_LEN];

    Entry            entries[ASSET_WATCH_MAX_PATHS];
    uint32_t         entry_count;

    float            rescan_period_s;
    float            rescan_accum_s;
};

/* ── Internal helpers ──────────────────────────────────────────── */

static int find_entry(JceAssetWatcher *w, const char *path)
{
    for (uint32_t i = 0; i < w->entry_count; ++i)
        if (strncmp(w->entries[i].path, path, ASSET_PATH_LEN) == 0)
            return (int)i;
    return -1;
}

static void compact_remove_at(JceAssetWatcher *w, uint32_t idx)
{
    if (idx >= w->entry_count) return;
    if (idx + 1u < w->entry_count) {
        w->entries[idx] = w->entries[w->entry_count - 1];
    }
    w->entry_count--;
}

/* Called from jce_file_watcher when a tracked file's mtime moves. */
static void file_changed_trampoline(const char *path, void *user_data)
{
    JceAssetWatcher *w = (JceAssetWatcher *)user_data;
    if (w && w->cb) w->cb(JCE_ASSET_EVENT_CHANGED, path, w->user_data);
}

typedef struct {
    JceAssetWatcher *w;
    uint32_t         added_this_scan;
} ScanCtx;

static bool walk_cb(const char *path, bool is_dir, void *user)
{
    ScanCtx *ctx = (ScanCtx *)user;
    JceAssetWatcher *w = ctx->w;
    if (is_dir) return true;
    if (w->entry_count >= ASSET_WATCH_MAX_PATHS) return false;

    int idx = find_entry(w, path);
    if (idx >= 0) {
        w->entries[idx].seen_this_scan = true;
        return true;
    }

    /* New file. */
    Entry *e = &w->entries[w->entry_count];
    strncpy(e->path, path, ASSET_PATH_LEN - 1);
    e->path[ASSET_PATH_LEN - 1] = '\0';
    e->seen_this_scan = true;
    w->entry_count++;

    if (jce_file_watcher_add(w->fw, e->path,
                             file_changed_trampoline, w)) {
        ctx->added_this_scan++;
        if (w->cb) w->cb(JCE_ASSET_EVENT_ADDED, e->path, w->user_data);
    } else {
        /* Failed to register — pop the entry so we retry next scan. */
        w->entry_count--;
    }
    return true;
}

static uint32_t rescan(JceAssetWatcher *w)
{
    if (!w) return 0;
    /* Mark all entries unseen. */
    for (uint32_t i = 0; i < w->entry_count; ++i)
        w->entries[i].seen_this_scan = false;

    ScanCtx ctx = { w, 0 };
    jce_fs_host_walk(w->root, walk_cb, &ctx);

    /* Anything still unseen has disappeared. */
    uint32_t removed = 0;
    for (uint32_t i = w->entry_count; i-- > 0; ) {
        if (w->entries[i].seen_this_scan) continue;
        /* Snapshot the path before swap-removal. */
        char gone[ASSET_PATH_LEN];
        strncpy(gone, w->entries[i].path, sizeof(gone));
        gone[sizeof(gone) - 1] = '\0';
        jce_file_watcher_remove(w->fw, gone);
        compact_remove_at(w, i);
        if (w->cb) w->cb(JCE_ASSET_EVENT_REMOVED, gone, w->user_data);
        removed++;
    }
    return ctx.added_this_scan + removed;
}

/* ── Lifecycle ─────────────────────────────────────────────────── */

JceAssetWatcher *jce_asset_watcher_create(const char *root_dir,
                                            JceAssetWatchFn cb,
                                            void *user_data,
                                            float rescan_seconds)
{
    if (!root_dir || !*root_dir) return NULL;
    if (!jce_fs_host_exists_dir(root_dir)) {
        LOG_WARN(LOG_TAG, "root '%s' does not exist", root_dir);
        return NULL;
    }

    JceAssetWatcher *w = (JceAssetWatcher *)JCE_CALLOC(1, sizeof(*w));
    if (!w) return NULL;
    w->fw = jce_file_watcher_create();
    if (!w->fw) { JCE_FREE(w); return NULL; }
    w->cb = cb;
    w->user_data = user_data;
    strncpy(w->root, root_dir, sizeof(w->root) - 1);
    w->root[sizeof(w->root) - 1] = '\0';
    w->rescan_period_s = rescan_seconds > 0.0f ? rescan_seconds : 2.0f;
    w->rescan_accum_s = w->rescan_period_s; /* trigger immediate first scan via tick */

    /* Initial scan synchronously so callers see ADDED events for
     * everything already present before they start ticking. */
    rescan(w);
    w->rescan_accum_s = 0.0f;
    return w;
}

void jce_asset_watcher_destroy(JceAssetWatcher *w)
{
    if (!w) return;
    if (w->fw) jce_file_watcher_destroy(w->fw);
    JCE_FREE(w);
}

uint32_t jce_asset_watcher_tick(JceAssetWatcher *w, float dt)
{
    if (!w) return 0;
    uint32_t n = 0;
    /* Mtime poll on existing files. */
    n += jce_file_watcher_poll(w->fw);
    /* Periodic rescan for additions/deletions. */
    w->rescan_accum_s += dt;
    if (w->rescan_accum_s >= w->rescan_period_s) {
        w->rescan_accum_s = 0.0f;
        n += rescan(w);
    }
    return n;
}

uint32_t jce_asset_watcher_count(const JceAssetWatcher *w)
{
    return w ? w->entry_count : 0u;
}
