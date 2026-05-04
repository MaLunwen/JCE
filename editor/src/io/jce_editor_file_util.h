/*
 * jce_editor_file_util.h  Common file I/O helpers for editor code.
 *
 * Two simple functions backed by the engine's host filesystem wrappers
 * (jce_fs_host_*) so the editor never touches Win32 / POSIX / stdio
 * directly. Buffers are returned as ED_MALLOC'd memory to keep the
 * editor's allocator accounting consistent.
 *
 *   void *ed_read_file(path, &out_size)   -- read whole file, caller frees
 *   bool  ed_write_file(path, data, size) -- write buffer to file
 */

#ifndef JCE_EDITOR_FILE_UTIL_H
#define JCE_EDITOR_FILE_UTIL_H

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>

#include "core/jce_editor_alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Read an entire file into an ED_MALLOC'd buffer.
 * Returns NULL on failure.  Appends a '\0' sentinel beyond out_size
 * so the result can safely be cast to (char *) for text files.
 * Caller must ED_FREE the returned pointer. */
static inline void *ed_read_file(const char *path, size_t *out_size)
{
    if (out_size) *out_size = 0;
    if (!path) return NULL;

    uint64_t n = 0;
    void *raw = jce_fs_host_read_all(path, &n);
    if (!raw) return NULL;

    void *buf = ED_MALLOC((size_t)n + 1);
    if (!buf) { jce_fs_buffer_free(raw); return NULL; }

    if (n > 0) memcpy(buf, raw, (size_t)n);
    ((char *)buf)[n] = '\0';
    jce_fs_buffer_free(raw);

    if (out_size) *out_size = (size_t)n;
    return buf;
}

/* Write a buffer to a file.  Returns true on success. */
static inline bool ed_write_file(const char *path,
                                 const void *data, size_t size)
{
    if (!path || (!data && size > 0)) return false;
    return jce_fs_host_write_all(path, data, size);
}

/* Serialize a JSON tree to a file and free the tree.
 * Returns true on success.  The root is always released. */
static inline bool ed_write_json_to_file(const char *path, JceJson *root)
{
    return jce_json_write_file(path, root, /*pretty=*/true,
                               /*take_ownership=*/true);
}

/* Read at most `max_bytes` of a file into an ED_MALLOC'd buffer.
 * Useful for previews where huge files must be capped.
 * Writes bytes actually read to *out_size and full file size to
 * *out_total (caller may pass NULL for either).  Appends a '\0'
 * sentinel so the buffer is safe to use as text. */
static inline void *ed_read_file_capped(const char *path,
                                        size_t max_bytes,
                                        size_t *out_size,
                                        size_t *out_total)
{
    if (out_size) *out_size = 0;
    if (out_total) *out_total = 0;
    if (!path) return NULL;

    uint64_t got = 0, total = 0;
    void *raw = jce_fs_host_read_capped(path, (uint64_t)max_bytes, &got, &total);
    if (!raw) return NULL;

    if (out_total) *out_total = (size_t)total;

    void *buf = ED_MALLOC((size_t)got + 1);
    if (!buf) { jce_fs_buffer_free(raw); return NULL; }

    if (got > 0) memcpy(buf, raw, (size_t)got);
    ((char *)buf)[got] = '\0';
    jce_fs_buffer_free(raw);

    if (out_size) *out_size = (size_t)got;
    return buf;
}

#endif /* JCE_EDITOR_FILE_UTIL_H */
