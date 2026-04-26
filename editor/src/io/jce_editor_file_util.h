/*
 * jce_editor_file_util.h  Common file I/O helpers for editor code.
 *
 * Two simple functions backed by SDL3 for cross-platform I/O:
 *
 *   void *ed_read_file(path, &out_size)   -- read whole file, caller frees
 *   bool  ed_write_file(path, data, size) -- write buffer to file
 */

#ifndef JCE_EDITOR_FILE_UTIL_H
#define JCE_EDITOR_FILE_UTIL_H

#include <jce/os/core/jce_json.h>

#include "jce_editor_alloc.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Read an entire file into an ED_MALLOC'd buffer.
 * Returns NULL on failure.  Appends a '\0' sentinel beyond out_size
 * so the result can safely be cast to (char *) for text files.
 * Caller must ED_FREE the returned pointer. */
static inline void *ed_read_file(const char *path, size_t *out_size)
{
    if (out_size) *out_size = 0;
    if (!path) return NULL;

    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return NULL;

    Sint64 file_size = SDL_GetIOSize(io);
    if (file_size <= 0) { SDL_CloseIO(io); return NULL; }

    void *buf = ED_MALLOC((size_t)file_size + 1);
    if (!buf) { SDL_CloseIO(io); return NULL; }

    size_t nread = SDL_ReadIO(io, buf, (size_t)file_size);
    SDL_CloseIO(io);

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
    if (!path || !data) return false;
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (!io) return false;
    size_t written = SDL_WriteIO(io, data, size);
    SDL_CloseIO(io);
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

    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return NULL;

    Sint64 file_size = SDL_GetIOSize(io);
    if (file_size < 0) { SDL_CloseIO(io); return NULL; }
    if (out_total) *out_total = (size_t)file_size;

    size_t read_size = (size_t)file_size;
    if (read_size > max_bytes) read_size = max_bytes;

    void *buf = ED_MALLOC(read_size + 1);
    if (!buf) { SDL_CloseIO(io); return NULL; }

    size_t nread = (read_size > 0) ? SDL_ReadIO(io, buf, read_size) : 0;
    SDL_CloseIO(io);

    ((char *)buf)[nread] = '\0';
    if (out_size) *out_size = nread;
    return buf;
}

#endif /* JCE_EDITOR_FILE_UTIL_H */
