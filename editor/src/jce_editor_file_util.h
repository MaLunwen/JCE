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

#include "jce_editor_alloc.h"
#include <cjson/cJSON.h>
#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdbool.h>
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

/* Serialize a cJSON tree to a file and free the tree.
 * Returns true on success.  The cJSON root is always deleted. */
static inline bool ed_write_json_to_file(const char *path, cJSON *root)
{
    if (!path || !root) { cJSON_Delete(root); return false; }

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_str) return false;

    size_t len = strlen(json_str);
    bool ok = ed_write_file(path, json_str, len);
    cJSON_free(json_str);
    return ok;
}

#endif /* JCE_EDITOR_FILE_UTIL_H */
