/*
 * jce_file_watcher.h  Cross-platform file modification watcher.
 *
 * Polls a set of registered file paths at a caller-driven cadence and
 * fires a callback whenever the modification time changes since the
 * last poll.  Backend uses SDL_GetPathInfo() for portability across
 * Windows / macOS / Linux / mobile.
 *
 * Designed for low-frequency hot-reload scenarios (shaders, JSON
 * configs, scenes) where sub-second latency is acceptable and where
 * adding native FS event APIs (inotify/FSEvents/RDCW) would explode
 * the platform matrix.
 *
 * NOT a replacement for true OS file events when watching thousands of
 * files — use a directory tree at most a few hundred entries.
 *
 * Threading: not thread-safe.  Drive jce_file_watcher_poll() from one
 * thread (typically the main loop, gated by a 200–500 ms timer).
 */

#ifndef JCE_FILE_WATCHER_H
#define JCE_FILE_WATCHER_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceFileWatcher JceFileWatcher;

/* Fired when a watched file's mtime has changed since the last poll.
 * `user_data` is the value passed to jce_file_watcher_add(). */
typedef void (*JceFileChangedFn)(const char *path, void *user_data);

/* Create / destroy a watcher.  Returns NULL on OOM. */
JCE_API JceFileWatcher *jce_file_watcher_create(void);
JCE_API void            jce_file_watcher_destroy(JceFileWatcher *fw);

/* Register a file to watch.  `cb` is invoked from jce_file_watcher_poll()
 * on the polling thread when the file's mtime changes.  Returns false on
 * OOM or if the file does not currently exist. */
JCE_API bool jce_file_watcher_add(JceFileWatcher  *fw,
                                  const char      *path,
                                  JceFileChangedFn cb,
                                  void            *user_data);

/* Stop watching a previously-registered path. */
JCE_API void jce_file_watcher_remove(JceFileWatcher *fw, const char *path);

/* Poll every registered path once.  Fires callbacks synchronously for
 * any file whose mtime has changed.  Returns the number of callbacks
 * fired.  Caller throttles call frequency. */
JCE_API uint32_t jce_file_watcher_poll(JceFileWatcher *fw);

/* Number of paths currently being watched. */
JCE_API uint32_t jce_file_watcher_count(const JceFileWatcher *fw);

JCE_EXTERN_C_END

#endif /* JCE_FILE_WATCHER_H */
