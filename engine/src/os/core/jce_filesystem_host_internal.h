#ifndef JCE_FILESYSTEM_HOST_INTERNAL_H
#define JCE_FILESYSTEM_HOST_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL_atomic.h>

/* Private read observations. These are request-string identity and this
 * owner's observations, not canonical paths, VFS miss reasons or IO durability.
 * Values are encoded as fixed scoped counters by the actual host owner. */
typedef enum JceFsHostReadDispatch {
    JCE_FS_HOST_READ_NONE = 0, JCE_FS_HOST_READ_HOST = 1,
    JCE_FS_HOST_READ_VFS = 2, JCE_FS_HOST_READ_VFS_THEN_HOST = 3
} JceFsHostReadDispatch;
typedef enum JceFsHostReadFailure {
    JCE_FS_HOST_READ_OK = 0, JCE_FS_HOST_READ_INVALID = 1,
    JCE_FS_HOST_READ_ISOLATED_VFS = 2, JCE_FS_HOST_READ_OPEN = 3,
    JCE_FS_HOST_READ_SIZE = 4, JCE_FS_HOST_READ_LIMIT = 5,
    JCE_FS_HOST_READ_ALLOC = 6, JCE_FS_HOST_READ_SHORT = 7
} JceFsHostReadFailure;
enum {
    JCE_FS_HOST_READ_KNOWN_TOTAL = 1u, JCE_FS_HOST_READ_KNOWN_REQUESTED = 2u,
    JCE_FS_HOST_READ_KNOWN_CAP = 4u, JCE_FS_HOST_READ_KNOWN_ACTUAL = 8u,
    JCE_FS_HOST_READ_CLOSE_NONE = 0, JCE_FS_HOST_READ_CLOSE_OK = 1,
    JCE_FS_HOST_READ_CLOSE_FAILED = 2,
    JCE_FS_HOST_READ_SHORT_NONE = 0, JCE_FS_HOST_READ_FULL = 1,
    JCE_FS_HOST_READ_SHORT_OBSERVED = 2
};
typedef struct JceFsHostReadTrace {
    uint64_t path_hash, total, requested, cap, actual;
    uint32_t known, dispatch, failure_stage, close, short_read;
} JceFsHostReadTrace;

/* Engine-embedded POD; the host-read service only borrows the enabled owner.
 * Quiescent begin follows owner publication; matching end follows all FS/log
 * producer joins, detaches before summary and precedes context/Engine release.
 * Active same/foreign begin and foreign/repeated end refuse without reset or
 * detach. OFF binds nothing. Return values observe ownership, not log delivery. */
typedef struct JceFsHostOpenFailureContext {
    SDL_AtomicInt admitted, refused;
} JceFsHostOpenFailureContext;
bool jce__fs_host_open_failure_detail_begin(JceFsHostOpenFailureContext *context,
                                           bool enabled);
bool jce__fs_host_open_failure_detail_end(JceFsHostOpenFailureContext *context);

/* Private host-only exclusive creation. A successful open sets created even
 * if the subsequent write/close fails. Never removes that diagnostic file;
 * never infers absence from a failed path query. Not a durability guarantee. */
bool jce__fs_host_reserve_new(const char *path, const void *data,
                             uint64_t size, bool *created);

/* Existing SDL host IO owner, private persistent exclusive writer. No heap
 * wrapper or alternate IO implementation. close clears the borrowed handle
 * before SDL close, including failure; callers never close it a second time. */
typedef struct SDL_IOStream SDL_IOStream;
SDL_IOStream *jce__fs_host_stream_new(const char *path, bool *created);
bool jce__fs_host_stream_write(SDL_IOStream *io, const void *data, uint64_t size);
bool jce__fs_host_stream_close(SDL_IOStream **io);
#endif
