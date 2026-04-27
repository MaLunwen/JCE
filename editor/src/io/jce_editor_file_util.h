/*
 * jce_editor_file_util.h  Common file I/O helpers for editor code.
 *
 * Two simple functions backed by C stdio for cross-platform I/O:
 *
 *   void *ed_read_file(path, &out_size)   -- read whole file, caller frees
 *   bool  ed_write_file(path, data, size) -- write buffer to file
 *
 * The editor relies on ED_MALLOC / ED_FREE so the host engine's own
 * filesystem helpers (which return JCE_MALLOC'd buffers) are not used
 * here — that would break the editor's allocator accounting.
 */

#ifndef JCE_EDITOR_FILE_UTIL_H
#define JCE_EDITOR_FILE_UTIL_H

#include <jce/os/core/jce_json.h>

#include "core/jce_editor_alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Read an entire file into an ED_MALLOC'd buffer.
 * Returns NULL on failure.  Appends a '\0' sentinel beyond out_size
 * so the result can safely be cast to (char *) for text files.
 * Caller must ED_FREE the returned pointer. */
static inline void *ed_read_file(const char *path, size_t *out_size)
{
    if (out_size) *out_size = 0;
    if (!path) return NULL;

    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long file_size = ftell(fp);
    if (file_size < 0) { fclose(fp); return NULL; }
    rewind(fp);

    void *buf = ED_MALLOC((size_t)file_size + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t nread = (file_size > 0)
        ? fread(buf, 1, (size_t)file_size, fp) : 0;
    fclose(fp);

    if (nread != (size_t)file_size) {
        ED_FREE(buf);
        return NULL;
    }

    ((char *)buf)[nread] = '\0';  /* sentinel for text use */
    if (out_size) *out_size = nread;
    return buf;
}

/* Write a buffer to a file.  Returns true on success. */
static inline bool ed_write_file(const char *path,
                                 const void *data, size_t size)
{
    if (!path || (!data && size > 0)) return false;
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    size_t written = (size > 0) ? fwrite(data, 1, size, fp) : 0;
    fclose(fp);
    return written == size;
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

    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long file_size = ftell(fp);
    if (file_size < 0) { fclose(fp); return NULL; }
    rewind(fp);
    if (out_total) *out_total = (size_t)file_size;

    size_t read_size = (size_t)file_size;
    if (read_size > max_bytes) read_size = max_bytes;

    void *buf = ED_MALLOC(read_size + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t nread = (read_size > 0) ? fread(buf, 1, read_size, fp) : 0;
    fclose(fp);

    ((char *)buf)[nread] = '\0';
    if (out_size) *out_size = nread;
    return buf;
}

#endif /* JCE_EDITOR_FILE_UTIL_H */
