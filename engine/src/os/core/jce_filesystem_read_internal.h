/* Internal whole-asset read with pre-allocation bounds, owned by the VFS. */
#ifndef JCE_FILESYSTEM_READ_INTERNAL_H
#define JCE_FILESYSTEM_READ_INTERNAL_H
#include <jce/os/core/jce_filesystem.h>
/* NULL on miss, incomplete IO or oversize. out_size retains observed total on
 * rejection; max_bytes must be positive. Caller releases with jce_fs_buffer_free. */
void *jce_fs_read_all_bounded_internal(const JceFileSystem *fs,
    const char *path, uint64_t max_bytes, uint64_t *out_size);
/* A presence callback is bound only by the existing builtin active tuple.
 * Replacing its reader invalidates the callback; no custom-reader registry. */
typedef JceFsPresence (*JceFsPresenceFnInternal)(const JceFileSystem *, const char *);
void jce__fs_store_active(JceFileSystem *fs, JceFsPresenceFnInternal probe);
#endif
