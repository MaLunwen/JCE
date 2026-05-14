/*
 * jce_asset_watcher.h  Directory-aware asset hot-reload watcher.
 *
 * Higher-level wrapper over jce_file_watcher.h that watches a whole
 * directory tree (recursive) and fires per-asset events when files
 * change.  Periodic rescan picks up newly-added files; deletions
 * surface when polling sees the path vanish.
 *
 * Designed for editor-time hot reload: shaders, JSON scenes,
 * `.curve.json` (B8.7), `.sh3` (B6.2), `.overrides.json` (B5.1).
 *
 * Layer: resource (Layer 4) — public.  Single-threaded; call
 * `jce_asset_watcher_tick` once per frame from the main thread.
 */

#ifndef JCE_ASSET_WATCHER_H
#define JCE_ASSET_WATCHER_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_ASSET_EVENT_CHANGED = 0,  /* existing file's mtime moved */
    JCE_ASSET_EVENT_ADDED   = 1,  /* file appeared since last scan */
    JCE_ASSET_EVENT_REMOVED = 2,  /* watched file no longer exists */
} JceAssetWatchEvent;

typedef struct JceAssetWatcher JceAssetWatcher;

typedef void (*JceAssetWatchFn)(JceAssetWatchEvent ev,
                                 const char        *path,
                                 void              *user_data);

/* Create a watcher that walks `root_dir` recursively, registering
 * every file currently present.  Returns NULL on OOM or if the
 * directory doesn't exist.  `rescan_seconds` controls how often the
 * tree is re-walked to pick up new files (default 2.0 if zero). */
JCE_API JceAssetWatcher *jce_asset_watcher_create(const char     *root_dir,
                                                   JceAssetWatchFn cb,
                                                   void           *user_data,
                                                   float           rescan_seconds);

JCE_API void             jce_asset_watcher_destroy(JceAssetWatcher *w);

/* Tick once per frame.  Polls the underlying file watcher and
 * advances the rescan timer by `dt`.  Returns the number of events
 * fired this tick. */
JCE_API uint32_t         jce_asset_watcher_tick(JceAssetWatcher *w, float dt);

/* Number of files currently tracked. */
JCE_API uint32_t         jce_asset_watcher_count(const JceAssetWatcher *w);

JCE_EXTERN_C_END

#endif /* JCE_ASSET_WATCHER_H */
