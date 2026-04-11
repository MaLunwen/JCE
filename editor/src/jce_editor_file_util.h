/*
 * jce_editor_file_util.h  Common file I/O helpers for editor code.
 *
 * Replaces repetitive fopen/fseek/fread/fclose boilerplate with
 * two simple functions:
 *
 *   void *ed_read_file(path, &out_size)   -- read whole file, caller frees
 *   bool  ed_write_file(path, data, size) -- write buffer to file
 */

#ifndef JCE_EDITOR_FILE_UTIL_H
#define JCE_EDITOR_FILE_UTIL_H

#include "jce_editor_alloc.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

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

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (len <= 0) { fclose(fp); return NULL; }

    void *buf = ED_MALLOC((size_t)len + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t nread = fread(buf, 1, (size_t)len, fp);
    fclose(fp);

    if (nread != (size_t)len) {
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
    if (!path || !data) return false;
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    size_t written = fwrite(data, 1, size, fp);
    fclose(fp);
    return written == size;
}

#endif /* JCE_EDITOR_FILE_UTIL_H */
