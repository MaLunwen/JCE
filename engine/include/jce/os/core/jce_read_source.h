/* Shared, seekable input with bounded read-ahead and 64-bit offsets. */
#ifndef JCE_READ_SOURCE_H
#define JCE_READ_SOURCE_H

#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceReadSource JceReadSource;

typedef struct JceReadSourceDesc {
    uint64_t size;
    /* Return bytes read, never more than capacity. Short reads are allowed.
     * The source serializes callbacks; they must not re-enter this source. */
    size_t (*read_at)(void *user, uint64_t offset, void *out, size_t capacity);
    void (*close)(void *user);
    void *user;
} JceReadSourceDesc;

typedef struct JceReadSourceStats {
    uint64_t size;
    uint64_t bytes_read;
    uint64_t read_calls;
    uint64_t owned_bytes;
} JceReadSourceStats;

/* On success takes responsibility for close(user), exactly once after the
 * final reference. On failure the caller retains responsibility. */
JCE_API JceReadSource *jce_read_source_create(const JceReadSourceDesc *desc);
/* Host file, opened once. No full-file read or mapping fallback. */
JCE_API JceReadSource *jce_read_source_open_file(const char *path);
/* copy=true owns one immutable copy; otherwise caller keeps bytes alive until
 * the final close. All retained parsers share this same input, not copies. */
JCE_API JceReadSource *jce_read_source_open_memory(const void *data,
                                                size_t size, bool copy);
JCE_API JceReadSource *jce_read_source_acquire(JceReadSource *source);
JCE_API void jce_read_source_close(JceReadSource *source);
JCE_API uint64_t jce_read_source_size(const JceReadSource *source);
/* Thread-safe absolute read; clamps at EOF and returns bytes actually read.
 * Callers must hold a reference while reading. At most 64 KiB is read ahead. */
JCE_API size_t jce_read_source_read_at(JceReadSource *source, uint64_t offset,
                                     void *out, size_t capacity);
JCE_API bool jce_read_source_get_stats(JceReadSource *source,
                                     JceReadSourceStats *out);

JCE_EXTERN_C_END
#endif
